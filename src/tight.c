// naive と同じ愚直なアルゴリズムのまま、入力保証 (contest.md 02 章) に寸法と型を
// ぴったり合わせ、ループ内では保証を無条件に信頼する版。
// 保証を破る入力の検出は、制約を仮定しない naive の役割とする。
//   - 時刻は 2027 年内 → 暦計算を月境界 12 個との比較に置き換え
//   - チャンネルは最大 10,000 種類・104 文字 → 動的確保なしの固定配列
//   - 月ごとの count / total_length / total_stamp_count は u32 に収まる
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 10000
#define MAX_CHANNEL_LEN 104  // [0-9A-Za-z_-]{1,20} × 5 セグメント + '/' × 4
#define MAX_LINE_LEN 141     // ts(10) + ',' + channel(104) + ',' + len(10) + ',' + stamps(10) + '\n'
#define MONTHS 12
#define TABLE_SIZE 16384  // 開番地法。10,000 チャンネルで負荷率 0.61
#define YEAR_START 1798761600ll  // 2027-01-01T00:00:00Z

typedef struct {
    uint32_t min_len, max_len, total_len, count, total_stamps;
} Stats;

static char names[MAX_CHANNELS][MAX_CHANNEL_LEN + 1];
static Stats stats[MAX_CHANNELS][MONTHS];
static int32_t table[TABLE_SIZE];  // チャンネル名ハッシュ → チャンネル ID (-1 = 空)
static int64_t month_end[MONTHS];  // 各月の終端 (排他) の Unix 秒
static int channel_count = 0;

static void init_month_ends(void) {
    static const int days[MONTHS] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t t = YEAR_START;
    for (int m = 0; m < MONTHS; m++) {
        t += days[m] * 86400ll;
        month_end[m] = t;
    }
}

static int month_of(int64_t ts) {
    int m = 0;
    while (ts >= month_end[m]) m++;
    return m;
}

static int channel_id(const char *channel, size_t len) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)channel[i]) * 1099511628211ull;
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
    init_month_ends();
    memset(table, -1, sizeof(table));

    FILE *in = fopen(argv[1], "r");
    if (!in) {
        perror(argv[1]);
        return 1;
    }

    char line[MAX_LINE_LEN + 2];
    if (!fgets(line, sizeof(line), in)) {  // ヘッダ行 (ちょうど 1 行の保証)
        fprintf(stderr, "empty input\n");
        return 1;
    }

    while (fgets(line, sizeof(line), in)) {
        char *p = line;
        int64_t ts = strtoll(p, &p, 10);
        const char *channel = ++p;
        char *comma = strchr(p, ',');
        size_t channel_len = (size_t)(comma - channel);
        p = comma + 1;
        uint32_t len = (uint32_t)strtoul(p, &p, 10);
        uint32_t stamps = (uint32_t)strtoul(p + 1, NULL, 10);

        Stats *s = &stats[channel_id(channel, channel_len)][month_of(ts)];
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
    fclose(in);

    FILE *out = fopen(argv[2], "w");
    if (!out) {
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
