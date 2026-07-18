// 03-fasthash を土台にした並列化版 (対策 3)。03 からの差分:
//   - ファイルを行境界でスレッド数分に分割し、各スレッドが専用のハッシュ表と
//     集計配列を持って独立に走る (共有状態なし・ロックなし)。
//   - 集計後、チャンネル名で突き合わせてスレッド 0 の表へマージする。
//     マージ対象は高々 10,000 チャンネル × 12 ヶ月 × スレッド数なので誤差。
//   - スレッド数は sched_getaffinity から取る (taskset や本番の 8 vCPU を尊重)。
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
    uint32_t min_len, max_len, total_len, count, total_stamps;
} Stats;

typedef struct {
    uint64_t first8, last8;  // 名前の先頭/末尾 8 バイト (8 バイト未満はマスク済み)
    int32_t id;              // チャンネル ID (-1 = 空)
    uint32_t len;
} Slot;

// スレッドごとの独立した集計状態。隣の Ctx とキャッシュラインを共有しないよう 64 バイト境界に置く
typedef struct {
    alignas(64) const char *begin;  // 64 バイト境界揃えで隣の Ctx とのライン共有を断つ
    const char *end;  // 担当する行範囲 (行境界に整列済み)
    char names[MAX_CHANNELS][MAX_CHANNEL_LEN + 1];
    Stats stats[MAX_CHANNELS][MONTHS];
    Slot table[TABLE_SIZE];
    int channel_count;
} Ctx;

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

static int channel_id(Ctx *c, const char *channel, uint32_t len) {
    // 先頭 8 バイトは名前の外 (',' 以降) を含み得るのでマスクして決定的にする。
    // 末尾 8 バイトは len >= 8 なら名前内で完結する。名前の長短は行順にランダムなので、
    // 分岐ではなく cmov に落ちる形 (三項演算子) で書く。
    uint32_t head_len = len < 8 ? len : 8;
    uint64_t first8 = load8(channel) & (~0ull >> (64 - 8 * head_len));
    uint64_t last8 = len >= 8 ? load8(channel + len - 8) : first8;

    uint32_t slot = (uint32_t)(mix(first8 ^ (last8 * 0x9E37'79B9'7F4A'7C15ull) ^ len) >> 32) & (TABLE_SIZE - 1);
    for (;; slot = (slot + 1) & (TABLE_SIZE - 1)) {
        Slot *e = &c->table[slot];
        if (e->id < 0) {
            memcpy(c->names[c->channel_count], channel, len);
            c->names[c->channel_count][len] = '\0';
            *e = (Slot){.first8 = first8, .last8 = last8, .id = c->channel_count, .len = len};
            return c->channel_count++;
        }
        if (e->len == len && e->first8 == first8 && e->last8 == last8 &&
            (len <= 16 || memcmp(c->names[e->id] + 8, channel + 8, len - 16) == 0)) {
            return e->id;
        }
    }
}

// 数字 8 文字を乗算 3 回で一括変換する (分割統治の SWAR。mizar さんの記事より)。
// 各段の乗算後に隣レーンへ漏れた上位ビットをマスクで落とすのが肝
static uint32_t parse8(const char *p) {
    uint64_t x = load8(p) & 0x0F0F'0F0F'0F0F'0F0Full;
    x = ((x * ((10ull << 8) | 1)) >> 8) & 0x00FF'00FF'00FF'00FFull;
    x = ((x * ((100ull << 16) | 1)) >> 16) & 0x0000'FFFF'0000'FFFFull;
    x = (x * ((10'000ull << 32) | 1)) >> 32;
    return (uint32_t)x;
}

// 1 行を処理して次の行の先頭を返す。行は '\n' で終端されていること。
static const char *process_row(Ctx *c, const char *p) {
    MCA_BEGIN("parse");
    uint32_t ts8 = parse8(p);  // 10 桁固定の先頭 8 桁 = ts / 100
    const char *channel = p + 11;  // 10 桁固定 + ','
    const char *q = memchr(channel, ',', MAX_CHANNEL_LEN + 1);
    uint32_t channel_len = (uint32_t)(q - channel);
    q++;
    uint32_t len = 0;
    for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) len = len * 10 + d;
    q++;  // ','
    uint32_t stamps = 0;
    for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) stamps = stamps * 10 + d;
    MCA_END("parse");

    // min_len は UINT32_MAX で事前初期化してあるので初回判定の分岐が要らない。
    // min/max の更新は集計が温まるとほぼ起きない = ほぼ常に不成立の予測well な分岐なので、
    // cmov (毎行ロード→比較→ストアの依存鎖) より分岐のほうが速い (実測 2.50 vs 2.74 秒)
    Stats *s = &c->stats[channel_id(c, channel, channel_len)][month_of(ts8)];
    if (len < s->min_len) s->min_len = len;
    if (len > s->max_len) s->max_len = len;
    s->total_len += len;
    s->count++;
    s->total_stamps += stamps;
    return q + 1;  // '\n' の次 (最終行も LF 終端の保証)
}

static void *worker(void *arg) {
    Ctx *c = arg;
    init_ctx(c);  // 表の初期化 (100 万書き込み超) も並列化し、ファーストタッチも自スレッドで行う
    const char *p = c->begin;
    while (p < c->end) p = process_row(c, p);
    return nullptr;
}

static void merge_into_first(Ctx *dst, const Ctx *src) {
    for (int sc = 0; sc < src->channel_count; sc++) {
        uint32_t len = (uint32_t)strlen(src->names[sc]);
        int dc = channel_id(dst, src->names[sc], len);
        for (int m = 0; m < MONTHS; m++) {
            const Stats *s = &src->stats[sc][m];
            if (s->count == 0) continue;
            Stats *d = &dst->stats[dc][m];
            if (s->min_len < d->min_len) d->min_len = s->min_len;
            if (s->max_len > d->max_len) d->max_len = s->max_len;
            d->total_len += s->total_len;
            d->count += s->count;
            d->total_stamps += s->total_stamps;
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

    // 最終行は 8 バイトロードが写像末尾を越えないよう、余白付きバッファへ移して処理する
    const char *last_line = end - 1;  // 末尾は '\n' の保証があるので、その直前の '\n' を探す
    while (last_line > body && last_line[-1] != '\n') last_line--;

    // [body, last_line) をスレッド数で等分し、各境界を次の行頭へ整列する
    int threads = thread_count();
    const char *cursor = body;
    for (int t = 0; t < threads; t++) {
        ctx[t].begin = cursor;
        if (t == threads - 1) {
            cursor = last_line;
        } else {
            const char *target = body + (size_t)(last_line - body) * (t + 1) / threads;
            const char *nl = memchr(target, '\n', (size_t)(last_line - target));
            cursor = nl != nullptr ? nl + 1 : last_line;
        }
        ctx[t].end = cursor;
    }

    pthread_t tid[MAX_THREADS];
    for (int t = 1; t < threads; t++) pthread_create(&tid[t], nullptr, worker, &ctx[t]);
    worker(&ctx[0]);
    for (int t = 1; t < threads; t++) pthread_join(tid[t], nullptr);

    if (last_line < end) {
        char buffer[MAX_LINE_LEN + 8] = {};
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
            if (s->count == 0) continue;
            fprintf(out, "%s,2027-%02d=%" PRIu32 "/%.2f/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
                    ctx[0].names[c], m + 1, s->min_len, (double)s->total_len / (double)s->count,
                    s->max_len, s->count, s->total_stamps);
        }
    }
    fclose(out);
    return 0;
}
