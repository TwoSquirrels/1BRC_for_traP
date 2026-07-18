// 02-fastio を土台にしたハッシュ軽量化版 (対策 2)。02 からの差分:
//   - FNV のバイトループ + strncmp を廃止し、チャンネル名の
//     「長さ + 先頭 8 バイト + 末尾 8 バイト」の u64 演算だけでハッシュと比較を行う。
//     16 文字以下ならこの 3 つ組が名前を一意に決めるので比較は u64 等値 2 回で済み、
//     17 文字以上のときだけ中間バイトを memcmp で検証する。
//   - 8 バイトロードが mmap 領域外へはみ出さないよう、最終行だけ別バッファで処理する。
// 根拠: アブレーションで 02 の残り時間の 78% がハッシュ + 集計だったため。
// 今後: 8 スレッド並列 (対策 3) を予定。
#define _DEFAULT_SOURCE  // 厳密な -std=c23 でも madvise 等の POSIX/BSD 拡張を出す
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr int MAX_CHANNELS = 10'000;
constexpr int MAX_CHANNEL_LEN = 104;  // [0-9A-Za-z_-]{1,20} × 5 セグメント + '/' × 4
constexpr int MAX_LINE_LEN = 141;     // ts(10) + ',' + channel(104) + ',' + len(10) + ',' + stamps(10) + '\n'
constexpr int MONTHS = 12;
constexpr uint32_t TABLE_SIZE = 16'384;  // 開番地法。10,000 チャンネルで負荷率 0.61
constexpr int64_t YEAR_START = 1'798'761'600;  // 2027-01-01T00:00:00Z
static_assert((TABLE_SIZE & (TABLE_SIZE - 1)) == 0, "TABLE_SIZE はマスク演算のため 2 冪");

// タイムスタンプの中央 7 桁と同じ切り詰め表現 (100 秒単位、先頭桁なし)。
// 1 日 = 864 単位なので、(ts7 - 年始) / 864 で年内の日番号が得られる。
#define TRUNCATE_TS(unix_sec) ((uint32_t)(((int64_t)(unix_sec) % 1'000'000'000) / 100))
constexpr uint32_t YEAR_START7 = TRUNCATE_TS(YEAR_START);
constexpr uint32_t UNITS_PER_DAY = 86'400 / 100;
constexpr int YEAR_DAYS = 365;
static_assert(TRUNCATE_TS(YEAR_START + (int64_t)YEAR_DAYS * 86'400) == 8'302'976,
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

typedef struct {
    uint32_t min_len, max_len, total_len, count, total_stamps;
} Stats;

typedef struct {
    uint64_t first8, last8;  // 名前の先頭/末尾 8 バイト (8 バイト未満はマスク済み)
    int32_t id;              // チャンネル ID (-1 = 空)
    uint32_t len;
} Slot;

static char names[MAX_CHANNELS][MAX_CHANNEL_LEN + 1];
static Stats stats[MAX_CHANNELS][MONTHS];
static Slot table[TABLE_SIZE];
static int channel_count = 0;

static int month_of(uint32_t ts7) {
    return day_month[(ts7 - YEAR_START7) / UNITS_PER_DAY];
}

static uint64_t load8(const char *p) {
    uint64_t x;
    memcpy(&x, p, 8);
    return x;
}

[[unsequenced]] static uint64_t mix(uint64_t h) {  // MurmurHash3 の finalizer 前半
    h ^= h >> 33;
    return h * 0xFF51'AFD7'ED55'8CCDull;
}

static int channel_id(const char *channel, uint32_t len) {
    // 先頭 8 バイトは名前の外 (',' 以降) を含み得るのでマスクして決定的にする。
    // 末尾 8 バイトは len >= 8 なら名前内で完結する。名前の長短は行順にランダムなので、
    // 分岐ではなく cmov に落ちる形 (三項演算子) で書く。
    uint32_t head_len = len < 8 ? len : 8;
    uint64_t first8 = load8(channel) & (~0ull >> (64 - 8 * head_len));
    uint64_t last8 = len >= 8 ? load8(channel + len - 8) : first8;

    uint32_t slot = (uint32_t)(mix(first8 ^ (last8 * 0x9E37'79B9'7F4A'7C15ull) ^ len) >> 32) & (TABLE_SIZE - 1);
    for (;; slot = (slot + 1) & (TABLE_SIZE - 1)) {
        Slot *e = &table[slot];
        if (e->id < 0) {
            memcpy(names[channel_count], channel, len);
            names[channel_count][len] = '\0';
            *e = (Slot){.first8 = first8, .last8 = last8, .id = channel_count, .len = len};
            return channel_count++;
        }
        if (e->len == len && e->first8 == first8 && e->last8 == last8 &&
            (len <= 16 || memcmp(names[e->id] + 8, channel + 8, len - 16) == 0)) {
            return e->id;
        }
    }
}

// 1 行を処理して次の行の先頭を返す。行は '\n' で終端されていること。
static const char *process_row(const char *p) {
    uint32_t ts7 = 0;
    for (int i = 1; i < 8; i++) ts7 = ts7 * 10 + (uint32_t)(p[i] - '0');
    const char *channel = p + 11;  // 10 桁固定 + ','
    const char *q = memchr(channel, ',', MAX_CHANNEL_LEN + 1);
    uint32_t channel_len = (uint32_t)(q - channel);
    q++;
    uint32_t len = 0;
    for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) len = len * 10 + d;
    q++;  // ','
    uint32_t stamps = 0;
    for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) stamps = stamps * 10 + d;

    // min_len は UINT32_MAX で事前初期化してあるので初回判定の分岐が要らない
    Stats *s = &stats[channel_id(channel, channel_len)][month_of(ts7)];
    if (len < s->min_len) s->min_len = len;
    if (len > s->max_len) s->max_len = len;
    s->total_len += len;
    s->count++;
    s->total_stamps += stamps;
    return q + 1;  // '\n' の次 (最終行も LF 終端の保証)
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.csv> <output.txt>\n", argv[0]);
        return 2;
    }
    init_day_month();
    for (uint32_t i = 0; i < TABLE_SIZE; i++) table[i].id = -1;
    for (int c = 0; c < MAX_CHANNELS; c++) {
        for (int m = 0; m < MONTHS; m++) stats[c][m].min_len = UINT32_MAX;
    }

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
    madvise((void *)data, (size_t)st.st_size, MADV_SEQUENTIAL | MADV_WILLNEED);
    const char *end = data + st.st_size;

    const char *p = memchr(data, '\n', (size_t)st.st_size);
    if (p == nullptr) {
        fprintf(stderr, "empty input\n");
        return 1;
    }
    p++;  // ヘッダ行を読み飛ばした位置

    // 最終行は 8 バイトロードが写像末尾を越えないよう、余白付きバッファへ移して処理する
    const char *last_line = end - 1;  // 末尾は '\n' の保証があるので、その直前の '\n' を探す
    while (last_line > p && last_line[-1] != '\n') last_line--;

    while (p < last_line) p = process_row(p);
    if (p < end) {
        char buffer[MAX_LINE_LEN + 8] = {};
        memcpy(buffer, p, (size_t)(end - p));
        process_row(buffer);
    }
    munmap((void *)data, (size_t)st.st_size);
    close(fd);

    FILE *out = fopen(argv[2], "w");
    if (out == nullptr) {
        perror(argv[2]);
        return 1;
    }
    for (int c = 0; c < channel_count; c++) {
        for (int m = 0; m < MONTHS; m++) {
            const Stats *s = &stats[c][m];
            if (s->count == 0) continue;
            fprintf(out, "%s,2027-%02d=%" PRIu32 "/%.2f/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
                    names[c], m + 1, s->min_len, (double)s->total_len / (double)s->count,
                    s->max_len, s->count, s->total_stamps);
        }
    }
    fclose(out);
    return 0;
}
