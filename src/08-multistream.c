// 07-fusedscan を土台にした ILP + 乗算減圧版。07 の asm 解析で見えた「Port 1 (乗算) 飽和・
// imul 13 本/行」への対策。07 からの差分:
//   - マルチストリーム ILP: 各スレッドの担当範囲をさらに 4 本の行整列された部分範囲に分け、
//     1 行ずつ交互に処理する。「行末確定 → 次の行のパース」という行内の直列依存が
//     ストリーム間では独立になり、乗算・ロードのレイテンシを互いに覆い隠せる。
//   - 数値 2 フィールドの同時パース: message_length + stamp_count は 99% 超が区切り込みで
//     8 バイトに収まるので、1 回の load8 から '\n' 位置と ',' 位置を求めて両方読む。
//     parse_uint 2 連発の「1 個目の桁数が決まるまで 2 個目のアドレスが決まらない」依存を切る。
//     stamp_count は 2 桁までを乗算なし (cmov + lea) で取り、3 桁以上だけ SWAR に落とす。
//   - 名前スキャンの movemask 乗算 2 回を cmov 選択 2 個に置き換え (Port 1 → ALU ポートへ移動)。
// これで 1 行あたりの imul は 13 → 8 (ts 3 + message_length 3 + ハッシュ 2)。
#define _GNU_SOURCE  // sched_getaffinity
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// llvm-mca 用の領域マーカー。tools/asm.zig が -DMCA_MARKERS 付きでアセンブリを生成した
// ときだけ有効になり、通常ビルド・提出物では何も生成しない (asm volatile は最適化バリア
// になるため常用してはならない)。ループ回転で BEGIN/END が前後逆になると領域認識に
// 失敗するので、ループ backedge をまたがない直線区間に置くこと。
#ifdef MCA_MARKERS
#define MCA_BEGIN(name) __asm__ volatile("# LLVM-MCA-BEGIN " name)
#define MCA_END(name) __asm__ volatile("# LLVM-MCA-END " name)
#else
#define MCA_BEGIN(name)
#define MCA_END(name)
#endif

constexpr int MAX_CHANNELS = 10'000;
constexpr int MAX_CHANNEL_LEN = 104;  // [0-9A-Za-z_-]{1,20} × 5 セグメント + '/' × 4
constexpr int MAX_LINE_LEN = 141;     // ts(10) + ',' + channel(104) + ',' + len(10) + ',' + stamps(10) + '\n'
constexpr int MONTHS = 12;
constexpr uint32_t TABLE_SIZE = 16'384;  // 開番地法。10,000 チャンネルで負荷率 0.61
constexpr int MAX_THREADS = 16;
constexpr int64_t YEAR_START = 1'798'761'600;  // 2027-01-01T00:00:00Z
static_assert((TABLE_SIZE & (TABLE_SIZE - 1)) == 0, "TABLE_SIZE はマスク演算のため 2 冪");

// タイムスタンプは行頭 8 バイトを SWAR で一括変換した「先頭 8 桁 = ts / 100」で扱う。
// 月境界は 86400 = 864 × 100 の倍数なので、100 秒単位への切り詰めで月判定は変わらない。
// 1 日 = 864 単位なので、(ts8 - 年始) / 864 で年内の日番号が得られる。
constexpr uint32_t YEAR_START8 = (uint32_t)(YEAR_START / 100);
constexpr uint32_t UNITS_PER_DAY = 86'400 / 100;
constexpr int YEAR_DAYS = 365;
static_assert((YEAR_START + (int64_t)YEAR_DAYS * 86'400) / 100 == 18'302'976,
              "2028-01-01T00:00:00Z = 1830297600 の切り詰め表現");

// 日番号 → 月 (0 始まり)。時刻は行順にランダムで月境界との比較は分岐予測が効かないため、
// 比較の連鎖ではなく除算 1 回 + テーブル引きで分岐なしに求める。
static uint8_t day_month[YEAR_DAYS];

static void init_day_month(void) {
    static const int days[MONTHS] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int day = 0;
    for (int m = 0; m < MONTHS; m++) {
        for (int i = 0; i < days[m]; i++) day_month[day++] = (uint8_t)m;
    }
}

static int month_of(uint32_t ts8) {
    return day_month[(ts8 - YEAR_START8) / UNITS_PER_DAY];
}

typedef struct {
    uint64_t count_stamps;  // (count << 32) | total_stamps
    uint32_t min_len, max_len, total_len;
} Stats;

typedef struct {
    uint64_t first8, last8;  // 名前の先頭/末尾 8 バイト (8 バイト未満はマスク済み)
    int32_t id;              // チャンネル ID (-1 = 空)
    uint32_t len;
} Slot;

// スレッドごとの独立した集計状態。hot な配列は 64 バイト境界に揃える
// (構造体先頭の整列だけでは中の配列は揃わない)。Slot 24 B / Stats 24 B は 64 の
// 約数でないため、それでも要素の一部はライン跨ぎになる (パディングはフットプリント増との取引)。
typedef struct {
    alignas(64) const char *begin;  // 64 バイト境界揃えで隣の Ctx とのライン共有を断つ
    const char *end;  // 担当する行範囲 (行境界に整列済み)
    alignas(64) Slot table[TABLE_SIZE];
    alignas(64) Stats stats[MAX_CHANNELS][MONTHS];
    char names[MAX_CHANNELS][MAX_CHANNEL_LEN + 1];
    int channel_count;
} Ctx;
static_assert(sizeof(Slot) == 24 && sizeof(Stats) == 24, "サイズが変わったらライン跨ぎ率を再検討");

static Ctx ctx[MAX_THREADS];

static void init_ctx(Ctx *c) {
    for (uint32_t i = 0; i < TABLE_SIZE; i++) c->table[i].id = -1;
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        for (int m = 0; m < MONTHS; m++) c->stats[ch][m].min_len = UINT32_MAX;
    }
}

static uint64_t load8(const char *p) {
    uint64_t x;
    memcpy(&x, p, 8);
    return x;
}

// 注: C23 の [[unsequenced]] を付けたいところだが、この Clang (zig 0.16 同梱) では
// 未実装で unknown attribute として無視されるため付けていない
static uint64_t mix(uint64_t h) {  // MurmurHash3 の finalizer 前半
    h ^= h >> 33;
    return h * 0xFF51'AFD7'ED55'8CCDull;
}

// v のうち 0x00 のバイトの最上位ビットを立てる (ゼロバイト検出の定石)。
// 偽陽性 (借りで 0x01 のバイトに立つマーカー) は最初の真のゼロバイトより上位にしか
// 出ないので、tzcnt で「最初の一致」を取る用途に限って正確。2 個目以降のマーカーを
// 信用してはならない (数字 '0' が偽マーカーになる罠を踏みかけた)
static uint64_t zero_bytes(uint64_t v) {
    return (v - 0x0101'0101'0101'0101ull) & ~v & 0x8080'8080'8080'8080ull;
}

static uint64_t comma_mask(uint64_t w) {
    return zero_bytes(w ^ 0x2C2C'2C2C'2C2C'2C2Cull);
}

static uint64_t newline_mask(uint64_t w) {
    return zero_bytes(w ^ 0x0A0A'0A0A'0A0A'0A0Aull);
}

// 各バイトが 1 桁 (0〜9) の 8 桁を、乗算 3 回の分割統治で数値へ (mizar さんの記事より)。
// 各段の乗算後に隣レーンへ漏れた上位ビットをマスクで落とすのが肝
static uint32_t swar10(uint64_t x) {
    x = ((x * ((10ull << 8) | 1)) >> 8) & 0x00FF'00FF'00FF'00FFull;
    x = ((x * ((100ull << 16) | 1)) >> 16) & 0x0000'FFFF'0000'FFFFull;
    return (uint32_t)((x * ((10'000ull << 32) | 1)) >> 32);
}

// 数字列 1 個を SWAR で一括変換し、区切り文字の次を返す (07 と同じ)。
// 2 フィールド同時パースが 8 バイトに収まらなかったときのフォールバック専用
static const char *parse_uint(const char *q, uint32_t *out) {
    uint64_t w = load8(q);
    uint64_t nd = (w - 0x3030'3030'3030'3030ull) & 0x8080'8080'8080'8080ull;
    if (nd == 0) {  // 8 桁以上: ほぼ来ない予測 well な分岐
        uint32_t v = 0;
        for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) v = v * 10 + d;
        *out = v;
        return q + 1;
    }
    uint32_t k = (uint32_t)__builtin_ctzll(nd) >> 3;  // 桁数 (1〜7)
    *out = swar10((w << (8 * (8 - k))) & 0x0F0F'0F0F'0F0F'0F0Full);
    return q + k + 1;
}

// パース済みの 1 行分。前段 (パース + ハッシュ) と後段 (照合 + 集計) の受け渡しに使う
typedef struct {
    const char *channel;
    uint64_t first8, last8;
    uint32_t slot, channel_len, ts8, len, stamps;
} Row;

// channel と channel_len から指紋とスロットを決める。w0 は呼び出し側でロード済みの
// 先頭 8 バイト (融合スキャンの再ロード回避)。先頭 8 バイトは名前の外 (',' 以降) を
// 含み得るのでマスクして決定的にする。名前の長短は行順にランダムなので、
// 分岐ではなく cmov に落ちる形 (三項演算子) で書く。
static void fingerprint(Row *r, uint64_t w0) {
    uint32_t head = r->channel_len < 8 ? r->channel_len : 8;
    uint64_t keep = ~0ull >> (64 - 8 * head);
    r->first8 = w0 & keep;
    r->last8 = load8(r->channel + r->channel_len - head) & keep;  // len < 8 なら first8 と同じ位置
    r->slot = (uint32_t)(mix(r->first8 ^ (r->last8 * 0x9E37'79B9'7F4A'7C15ull) ^ r->channel_len) >> 32) &
        (TABLE_SIZE - 1);
}

// 1 行をパースして r に詰め、次の行の先頭を返す。行は '\n' で終端されていること
static const char *parse_row(Row *r, const char *p) {
    MCA_BEGIN("parse");
    r->ts8 = swar10(load8(p) & 0x0F0F'0F0F'0F0F'0F0Full);  // 10 桁固定の先頭 8 桁 = ts / 100
    const char *name = p + 11;                              // 10 桁 + ','
    r->channel = name;
    uint64_t w0 = load8(name);
    uint64_t m0 = comma_mask(w0);
    uint64_t m1 = comma_mask(load8(name + 8));
    uint32_t len;
    if ((m0 | m1) != 0) {  // 名前 15 文字以下 (実測で行数の 9 割): よく当たる分岐
        // どちらのワードで見つかったかは movemask 乗算 (07) ではなく cmov 2 個で選ぶ (Port 1 減圧)
        uint64_t m = m0 != 0 ? m0 : m1;
        uint32_t base = m0 != 0 ? 0 : 8;
        len = base + ((uint32_t)__builtin_ctzll(m) >> 3);
    } else {
        const char *s = name + 16;
        uint64_t m;
        while ((m = comma_mask(load8(s))) == 0) s += 8;
        len = (uint32_t)(s - name) + ((uint32_t)__builtin_ctzll(m) >> 3);
    }
    r->channel_len = len;
    fingerprint(r, w0);
    const char *q = name + len + 1;
    // 数値 2 フィールドの同時パース。'\n' が 8 バイト内なら全部この 1 ワードで確定する。
    // ',' と '\n' の位置はそれぞれ独立した一致マスクの「最初のマーカー」から取る
    // (非数字検出の 2 個目のマーカーは '0' の借り伝播で偽陽性になるため使えない)
    uint64_t w = load8(q);
    uint64_t nl = newline_mask(w);
    MCA_END("parse");
    if (nl != 0) {  // 区切り込み 8 バイト以内 (実測 99% 超): よく当たる分岐
        MCA_BEGIN("numfast");
        uint32_t k1 = (uint32_t)__builtin_ctzll(comma_mask(w)) >> 3;  // ',' = message_length の桁数
        uint32_t k2 = (uint32_t)__builtin_ctzll(nl) >> 3;             // '\n' の位置
        r->len = swar10((w << (8 * (8 - k1))) & 0x0F0F'0F0F'0F0F'0F0Full);
        // stamp_count は 1 桁 95% + 2 桁でほぼ全部なので、2 桁までは乗算なし (cmov + lea)
        uint32_t digits = k2 - k1 - 1;
        uint32_t lo = (uint32_t)(w >> (8 * (k2 - 1))) & 0x0F;
        uint32_t hi = digits >= 2 ? (uint32_t)(w >> (8 * (k2 - 2))) & 0x0F : 0;
        r->stamps = hi * 10 + lo;
        MCA_END("numfast");
        if (__builtin_expect(digits > 2, 0)) {  // 3 桁以上だけ SWAR で引き直す
            r->stamps = swar10((w << (8 * (8 - k2))) & (~0ull << (8 * (8 - digits))) &
                               0x0F0F'0F0F'0F0F'0F0Full);
        }
        return q + k2 + 1;
    }
    q = parse_uint(q, &r->len);
    q = parse_uint(q, &r->stamps);
    return q;  // stamp_count の区切りは '\n' なので、そのまま次の行頭
}

// 中間バイト [8, len-8) の照合 (len >= 17 のときだけ呼ぶ)。提出バイナリの musl では
// memcmp がスカラーループに落ちるため、8 バイト刻みの自前比較で置き換える。
// 末尾チャンクは last8 の領域と重なって二重比較になるが、一致確認済みの領域なので無害
static bool middle_equal(const char *a, const char *b, uint32_t len) {
    for (uint32_t i = 8; i < len - 8; i += 8) {
        if (load8(a + i) != load8(b + i)) return false;
    }
    return true;
}

static int channel_id(Ctx *c, const Row *r) {
    uint32_t len = r->channel_len;
    for (uint32_t slot = r->slot;; slot = (slot + 1) & (TABLE_SIZE - 1)) {
        Slot *e = &c->table[slot];
        if (e->id < 0) {
            memcpy(c->names[c->channel_count], r->channel, len);
            c->names[c->channel_count][len] = '\0';
            *e = (Slot){.first8 = r->first8, .last8 = r->last8, .id = c->channel_count, .len = len};
            return c->channel_count++;
        }
        if (e->len == len && e->first8 == r->first8 && e->last8 == r->last8 &&
            (len <= 16 || middle_equal(c->names[e->id], r->channel, len))) {
            return e->id;
        }
    }
}

static void update_row(Ctx *c, const Row *r) {
    // min_len は UINT32_MAX で事前初期化してあるので初回判定の分岐が要らない。
    // min/max の更新は集計が温まるとほぼ起きない = ほぼ常に不成立の予測 well な分岐なので、
    // cmov (毎行ロード→比較→ストアの依存鎖) より分岐のほうが速い (実測 2.50 vs 2.74 秒)
    Stats *s = &c->stats[channel_id(c, r)][month_of(r->ts8)];
    if (r->len < s->min_len) s->min_len = r->len;
    if (r->len > s->max_len) s->max_len = r->len;
    s->total_len += r->len;
    s->count_stamps += (1ull << 32) | r->stamps;
}

// 1 行だけ処理する互換ヘルパ (ストリーム末尾と最終行の処理用)
static const char *process_row(Ctx *c, const char *p) {
    Row r;
    const char *next = parse_row(&r, p);
    update_row(c, &r);
    return next;
}

// target 以降で最初に始まる行の先頭 (無ければ limit)。範囲分割の境界整列に使う
static const char *next_line_start(const char *target, const char *limit) {
    const char *nl = memchr(target, '\n', (size_t)(limit - target));
    return nl != nullptr ? nl + 1 : limit;
}

constexpr int STREAMS = 4;

static void *worker(void *arg) {
    Ctx *c = arg;
    init_ctx(c);  // 表の初期化 (100 万書き込み超) も並列化し、ファーストタッチも自スレッドで行う
    // 担当範囲をさらに STREAMS 本の行整列された部分範囲に分け、1 行ずつ交互に処理する。
    // 「行末確定 → 次の行のパース」の直列依存はストリーム間では独立なので、
    // 乗算・ロード・表参照のレイテンシが互いに覆い隠される (マルチストリーム ILP)
    const char *p[STREAMS], *e[STREAMS];
    const char *cursor = c->begin;
    for (int s = 0; s < STREAMS; s++) {
        p[s] = cursor;
        cursor = s == STREAMS - 1
            ? c->end
            : next_line_start(c->begin + (size_t)(c->end - c->begin) * (s + 1) / STREAMS, c->end);
        e[s] = cursor;
    }
    Row rows[STREAMS];
    static_assert(STREAMS == 4, "継続条件の並びを書き換えること");
    while (p[0] < e[0] && p[1] < e[1] && p[2] < e[2] && p[3] < e[3]) {
        for (int s = 0; s < STREAMS; s++) {
            p[s] = parse_row(&rows[s], p[s]);
            __builtin_prefetch(&c->table[rows[s].slot], 0, 3);
        }
        for (int s = 0; s < STREAMS; s++) update_row(c, &rows[s]);
    }
    for (int s = 0; s < STREAMS; s++) {  // 尽きたストリーム以外の残りを順に処理
        while (p[s] < e[s]) p[s] = process_row(c, p[s]);
    }
    return nullptr;
}

static void merge_into_first(Ctx *dst, const Ctx *src) {
    for (int sc = 0; sc < src->channel_count; sc++) {
        Row r = {.channel = src->names[sc], .channel_len = (uint32_t)strlen(src->names[sc])};
        fingerprint(&r, load8(r.channel));
        int dc = channel_id(dst, &r);
        for (int m = 0; m < MONTHS; m++) {
            const Stats *s = &src->stats[sc][m];
            if (s->count_stamps == 0) continue;
            Stats *d = &dst->stats[dc][m];
            if (s->min_len < d->min_len) d->min_len = s->min_len;
            if (s->max_len > d->max_len) d->max_len = s->max_len;
            d->total_len += s->total_len;
            d->count_stamps += s->count_stamps;
        }
    }
}

static int thread_count(void) {
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        return n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
    }
    return 8;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.csv> <output.txt>\n", argv[0]);
        return 2;
    }
    init_day_month();

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    const char *data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    madvise((void *)data, (size_t)st.st_size, MADV_WILLNEED);
    const char *end = data + st.st_size;

    const char *body = memchr(data, '\n', (size_t)st.st_size);
    if (body == nullptr) {
        fprintf(stderr, "empty input\n");
        return 1;
    }
    body++;  // ヘッダ行を読み飛ばした位置

    // 最終行は先読みロードが写像末尾を越えないよう、余白付きバッファへ移して処理する。
    // 名前スキャンは名前先頭から最大 16 バイト先読みするが、中間の行では次に最終行
    // (最短でも 17 バイト) が続くため写像内に収まる
    const char *last_line = end - 1;  // 末尾は '\n' の保証があるので、その直前の '\n' を探す
    while (last_line > body && last_line[-1] != '\n') last_line--;

    // [body, last_line) をスレッド数で等分し、各境界を次の行頭へ整列する
    int threads = thread_count();
    const char *cursor = body;
    for (int t = 0; t < threads; t++) {
        ctx[t].begin = cursor;
        cursor = t == threads - 1
            ? last_line
            : next_line_start(body + (size_t)(last_line - body) * (t + 1) / threads, last_line);
        ctx[t].end = cursor;
    }

    pthread_t tid[MAX_THREADS];
    for (int t = 1; t < threads; t++) pthread_create(&tid[t], nullptr, worker, &ctx[t]);
    worker(&ctx[0]);
    for (int t = 1; t < threads; t++) pthread_join(tid[t], nullptr);

    if (last_line < end) {
        char buffer[MAX_LINE_LEN + 16] = {};
        memcpy(buffer, last_line, (size_t)(end - last_line));
        process_row(&ctx[0], buffer);
    }
    for (int t = 1; t < threads; t++) merge_into_first(&ctx[0], &ctx[t]);
    munmap((void *)data, (size_t)st.st_size);
    close(fd);

    FILE *out = fopen(argv[2], "w");
    if (out == nullptr) {
        perror(argv[2]);
        return 1;
    }
    for (int c = 0; c < ctx[0].channel_count; c++) {
        for (int m = 0; m < MONTHS; m++) {
            const Stats *s = &ctx[0].stats[c][m];
            uint32_t count = (uint32_t)(s->count_stamps >> 32);
            if (count == 0) continue;
            fprintf(out, "%s,2027-%02d=%" PRIu32 "/%.2f/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
                    ctx[0].names[c], m + 1, s->min_len, (double)s->total_len / (double)count,
                    s->max_len, count, (uint32_t)s->count_stamps);
        }
    }
    fclose(out);
    return 0;
}
