// tight を土台にした入出力高速化版。tight からの差分:
//   - mmap でファイル全体を写像し、fgets と行バッファへのコピーを全廃
//   - strtoul をやめ、バッファ上を 1 パスで走る自前の数字パース
//   - unix_timestamp は 10 桁固定 (2027 年内保証) を利用し、中央 7 桁だけ読む。
//     先頭 1 桁は常に '1'、月境界は 86400 = 864 × 100 の倍数なので下 2 桁は
//     月判定に影響しない。チャンネル開始位置も 11 で確定する。
//   - 月境界テーブルは constexpr でコンパイル時に確定させ、month_of は
//     [[unsequenced]] な純粋関数として最適化器に自由を与える (C23)
// 今後: ハッシュ軽量化 (対策 2)、8 スレッド並列 (対策 3) を予定。
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
constexpr int MONTHS = 12;
constexpr uint32_t TABLE_SIZE = 16'384;  // 開番地法。10,000 チャンネルで負荷率 0.61
constexpr int64_t YEAR_START = 1'798'761'600;  // 2027-01-01T00:00:00Z
static_assert((TABLE_SIZE & (TABLE_SIZE - 1)) == 0, "TABLE_SIZE はマスク演算のため 2 冪");

// タイムスタンプの中央 7 桁と同じ切り詰め表現 (100 秒単位、先頭桁なし) での月境界。
// 年始からの累計日数 × 86400 を YEAR_START に足してコンパイル時に確定させる。
#define MONTH_END(cumulative_days) \
    ((uint32_t)(((YEAR_START + (int64_t)(cumulative_days) * 86'400) % 1'000'000'000) / 100))
constexpr uint32_t month_end[MONTHS] = {
    MONTH_END(31),  MONTH_END(59),  MONTH_END(90),  MONTH_END(120),
    MONTH_END(151), MONTH_END(181), MONTH_END(212), MONTH_END(243),
    MONTH_END(273), MONTH_END(304), MONTH_END(334), MONTH_END(365),
};
static_assert(MONTH_END(365) == 8'302'976, "2028-01-01T00:00:00Z = 1830297600 の切り詰め表現");

typedef struct {
    uint32_t min_len, max_len, total_len, count, total_stamps;
} Stats;

static char names[MAX_CHANNELS][MAX_CHANNEL_LEN + 1];
static Stats stats[MAX_CHANNELS][MONTHS];
static int32_t table[TABLE_SIZE];  // チャンネル名ハッシュ → チャンネル ID (-1 = 空)
static int channel_count = 0;

[[unsequenced]] static int month_of(uint32_t ts7) {
    for (int m = 0; m < MONTHS; m++) {
        if (ts7 < month_end[m]) return m;
    }
    unreachable();  // 2027 年内の保証
}

static int channel_id(const char *channel, size_t len) {
    uint64_t h = 1'469'598'103'934'665'603ull;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)channel[i]) * 1'099'511'628'211ull;
    uint32_t slot = h & (TABLE_SIZE - 1);
    while (table[slot] >= 0) {
        if (strncmp(names[table[slot]], channel, len) == 0 && names[table[slot]][len] == '\0') {
            return table[slot];
        }
        slot = (slot + 1) & (TABLE_SIZE - 1);
    }
    memcpy(names[channel_count], channel, len);
    names[channel_count][len] = '\0';
    table[slot] = channel_count;
    return channel_count++;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.csv> <output.txt>\n", argv[0]);
        return 2;
    }
    memset(table, -1, sizeof(table));

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

    while (p < end) {
        uint32_t ts7 = 0;
        for (int i = 1; i < 8; i++) ts7 = ts7 * 10 + (uint32_t)(p[i] - '0');
        const char *channel = p + 11;  // 10 桁固定 + ','
        const char *q = memchr(channel, ',', (size_t)(end - channel));
        size_t channel_len = (size_t)(q - channel);
        q++;
        uint32_t len = 0;
        for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) len = len * 10 + d;
        q++;  // ','
        uint32_t stamps = 0;
        for (uint32_t d; (d = (uint32_t)(*q - '0')) <= 9; q++) stamps = stamps * 10 + d;
        p = q + 1;  // '\n' (最終行も LF 終端の保証)

        Stats *s = &stats[channel_id(channel, channel_len)][month_of(ts7)];
        if (s->count == 0) {
            s->min_len = s->max_len = len;
        } else {
            if (len < s->min_len) s->min_len = len;
            if (len > s->max_len) s->max_len = len;
        }
        s->total_len += len;
        s->count++;
        s->total_stamps += stamps;
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
