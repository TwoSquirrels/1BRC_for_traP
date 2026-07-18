// ベースライン実装。正しさ最優先の素朴版で、最適化版の検証相手と速度の基準点にする。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE_SIZE (1u << 20)
#define MAX_CHANNEL 1024

typedef struct Entry {
    struct Entry *next;
    char *channel;
    int year_month;  // year * 12 + (month - 1)
    int64_t min_len, max_len, total_len, count, total_stamps;
} Entry;

static Entry *table[TABLE_SIZE];

// Howard Hinnant の civil_from_days による Unix 秒 → UTC 年月変換
static int year_month_of(int64_t ts) {
    int64_t z = ts / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    int month = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (month <= 2) year++;
    return (int)(year * 12 + month - 1);
}

static uint64_t hash_key(const char *channel, int year_month) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = channel; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return (h ^ (uint64_t)year_month) * 1099511628211ull;
}

static Entry *find_or_insert(const char *channel, int year_month) {
    Entry **slot = &table[hash_key(channel, year_month) & (TABLE_SIZE - 1)];
    for (Entry *e = *slot; e; e = e->next) {
        if (e->year_month == year_month && strcmp(e->channel, channel) == 0) return e;
    }
    Entry *e = calloc(1, sizeof(Entry));
    if (!e) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    size_t name_size = strlen(channel) + 1;
    e->channel = malloc(name_size);
    if (!e->channel) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memcpy(e->channel, channel, name_size);
    e->year_month = year_month;
    e->min_len = INT64_MAX;
    e->next = *slot;
    *slot = e;
    return e;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.csv> <output.txt>\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) {
        perror(argv[1]);
        return 1;
    }

    char line[MAX_CHANNEL + 64];
    if (!fgets(line, sizeof(line), in)) {
        fprintf(stderr, "empty input\n");
        return 1;
    }

    while (fgets(line, sizeof(line), in)) {
        char *p = line;
        int64_t ts = strtoll(p, &p, 10);
        if (*p != ',') {
            fprintf(stderr, "malformed line: %s", line);
            return 1;
        }
        char *channel = ++p;
        char *comma = strchr(p, ',');
        if (!comma) {
            fprintf(stderr, "malformed line: %s", line);
            return 1;
        }
        *comma = '\0';
        p = comma + 1;
        int64_t len = strtoll(p, &p, 10);
        int64_t stamps = strtoll(p + 1, NULL, 10);

        Entry *e = find_or_insert(channel, year_month_of(ts));
        if (len < e->min_len) e->min_len = len;
        if (len > e->max_len) e->max_len = len;
        e->total_len += len;
        e->count++;
        e->total_stamps += stamps;
    }
    fclose(in);

    FILE *out = fopen(argv[2], "w");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        for (Entry *e = table[i]; e; e = e->next) {
            fprintf(out, "%s,%04d-%02d=%lld/%.2f/%lld/%lld/%lld\n", e->channel,
                    e->year_month / 12, e->year_month % 12 + 1, (long long)e->min_len,
                    (double)e->total_len / (double)e->count, (long long)e->max_len,
                    (long long)e->count, (long long)e->total_stamps);
        }
    }
    fclose(out);
    return 0;
}
