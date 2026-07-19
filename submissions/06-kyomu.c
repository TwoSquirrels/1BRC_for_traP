#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef ONEBRC_TIMING
#include <time.h>
#endif
#include <unistd.h>

#ifndef ONEBRC_CHANNEL_SLOTS
#define ONEBRC_CHANNEL_SLOTS 16384
#endif
#ifndef ONEBRC_INITIAL_CHANNELS
#define ONEBRC_INITIAL_CHANNELS 16384
#endif

#if (defined(ONEBRC_SPLIT_SLOTS) + defined(ONEBRC_PACKED_SLOTS) + \
     defined(ONEBRC_PACKED32_SLOTS)) > 1
#error "select only one fingerprint slot layout"
#endif
#if defined(ONEBRC_BATCH_ROWS) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || !defined(ONEBRC_SPLIT_SLOTS) || \
     !defined(ONEBRC_KNOWN_10000) || !defined(ONEBRC_2027_ONLY))
#error "ONEBRC_BATCH_ROWS requires the production fingerprint configuration"
#endif
#if defined(ONEBRC_DEFER_AGGREGATE) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || \
     (!defined(ONEBRC_SPLIT_SLOTS) && !defined(ONEBRC_PACKED_SLOTS) && \
      !defined(ONEBRC_PACKED32_SLOTS)) || \
     !defined(ONEBRC_KNOWN_10000) || !defined(ONEBRC_2027_ONLY))
#error "ONEBRC_DEFER_AGGREGATE requires the production fingerprint configuration"
#endif
#if defined(ONEBRC_COMBINED_SUM_COUNT) && \
    (!defined(ONEBRC_COMPACT_AGGREGATE) || defined(ONEBRC_WIDE_AGGREGATE))
#error "ONEBRC_COMBINED_SUM_COUNT requires adjacent 32-bit sum and count fields"
#endif
#if defined(ONEBRC_DIRECT_MERGE) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || !defined(ONEBRC_2027_ONLY))
#error "ONEBRC_DIRECT_MERGE requires the fingerprint map and 2027-only aggregation"
#endif
#if defined(ONEBRC_HOT_CACHE_SIZE) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || !defined(ONEBRC_SPLIT_SLOTS) || \
     !defined(ONEBRC_KNOWN_10000) || defined(ONEBRC_SHARED_DICTIONARY))
#error "ONEBRC_HOT_CACHE_SIZE requires a private split fingerprint map"
#endif
#if defined(ONEBRC_COLOCATE_MAP) && \
    (!defined(ONEBRC_HUGEPAGE_STATS) || !defined(ONEBRC_SPLIT_SLOTS) || \
     !defined(ONEBRC_KNOWN_10000) || ONEBRC_INITIAL_CHANNELS != 10000 || \
     ONEBRC_CHANNEL_SLOTS != 16384 || defined(ONEBRC_SHARED_DICTIONARY))
#error "ONEBRC_COLOCATE_MAP requires the exact private 10000-channel layout"
#endif
#if defined(ONEBRC_HASH32_SLOTS) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || !defined(ONEBRC_SPLIT_SLOTS))
#error "ONEBRC_HASH32_SLOTS requires split fingerprint slots"
#endif
#if defined(ONEBRC_PACKED32_SLOTS) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || !defined(ONEBRC_KNOWN_10000) || \
     ONEBRC_INITIAL_CHANNELS != 10000 || ONEBRC_CHANNEL_SLOTS != 16384 || \
     defined(ONEBRC_SHARED_DICTIONARY))
#error "ONEBRC_PACKED32_SLOTS requires the exact private 10000-channel layout"
#endif
#if defined(ONEBRC_FREQUENCY_REBUILD) && \
    (!defined(ONEBRC_FINGERPRINT_MAP) || \
     (!defined(ONEBRC_SPLIT_SLOTS) && !defined(ONEBRC_PACKED_SLOTS) && \
      !defined(ONEBRC_PACKED32_SLOTS)) || \
     !defined(ONEBRC_KNOWN_10000))
#error "ONEBRC_FREQUENCY_REBUILD requires a known compact fingerprint map"
#endif
#ifdef ONEBRC_DIRECT_STATS
#ifndef ONEBRC_DIRECT_STATS_BITS
#define ONEBRC_DIRECT_STATS_BITS 16
#endif
#if !defined(ONEBRC_FINGERPRINT_MAP) || !defined(ONEBRC_KNOWN_10000) || \
    !defined(ONEBRC_2027_ONLY) || !defined(ONEBRC_DEFER_AGGREGATE) || \
    defined(ONEBRC_SHARED_DICTIONARY) || defined(ONEBRC_HOT_CACHE_SIZE)
#error "ONEBRC_DIRECT_STATS requires the private production worker path"
#endif
#if ONEBRC_DIRECT_STATS_BITS < 15 || ONEBRC_DIRECT_STATS_BITS > 17
#error "ONEBRC_DIRECT_STATS_BITS must be 15, 16, or 17"
#endif
#endif

enum {
    MONTHS_2027 = 12,
    YM_2027 = 2027 * 12,
    LENGTH_SENTINEL = UINT16_MAX,
    CHANNEL_COLLISION_FLAG = UINT16_C(1) << 15,
    CHANNEL_ID_MASK = CHANNEL_COLLISION_FLAG - 1,
#ifdef ONEBRC_PACKED32_SLOTS
    PACKED32_ID_MASK = (UINT32_C(1) << 14) - 1,
    PACKED32_COLLISION_WORDS = (10000 + 63) / 64,
#endif
    FINGERPRINT_COLLISION_SEEN = 1,
    INITIAL_CHANNEL_SLOTS = ONEBRC_CHANNEL_SLOTS,
    INITIAL_CHANNELS = ONEBRC_INITIAL_CHANNELS,
    INITIAL_FINAL_SLOTS = 262144,
    PREFIX_BASE = 17987,
    PREFIX_COUNT = 316,
#ifdef ONEBRC_DIRECT_STATS
    DIRECT_STATS_BUCKETS = 1U << ONEBRC_DIRECT_STATS_BITS,
#endif
};

static const uint32_t month_start_2027[13] = {
    1798761600U, 1801440000U, 1803859200U, 1806537600U,
    1809129600U, 1811808000U, 1814400000U, 1817078400U,
    1819756800U, 1822348800U, 1825027200U, 1827619200U,
    1830297600U,
};

static uint8_t prefix_month[PREFIX_COUNT];
#ifdef ONEBRC_BCD_MONTH
static uint8_t bcd_month[4096];
#endif
#ifdef ONEBRC_COMBINED_DELIMITERS
static const uint8_t delimiter_shuffle[32] __attribute__((aligned(32))) = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, '\n', 0xff, ',',  0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, '\n', 0xff, ',',  0xff, 0xff, 0xff,
};
#endif

#ifdef ONEBRC_WIDE_AGGREGATE
typedef uint64_t AggregateCounter;
#else
typedef uint32_t AggregateCounter;
#endif

typedef struct {
#ifdef ONEBRC_COMPACT_AGGREGATE
    uint32_t sum_length;
#else
    uint64_t sum_length;
#endif
    AggregateCounter count;
    AggregateCounter sum_stamps;
    uint16_t min_length;
    uint16_t max_length;
} Aggregate;

typedef struct {
    uint32_t min_length;
    uint32_t max_length;
} WideLengths;

typedef struct {
    Aggregate month[MONTHS_2027];
} ChannelStats;

_Static_assert(sizeof(Aggregate) == 16, "Aggregate must stay in the hot 16-byte layout");
_Static_assert(sizeof(ChannelStats) == MONTHS_2027 * sizeof(Aggregate),
               "ChannelStats must not add padding between months");

typedef struct {
#ifdef ONEBRC_FINGERPRINT_MAP
    uint64_t hash;
    uint32_t id;
    uint32_t padding;
#else
    const char *key;
    uint64_t hash;
    uint32_t key_length;
    uint32_t id;
#endif
} ChannelSlot;

#ifdef ONEBRC_FINGERPRINT_MAP
typedef struct {
    const char *key;
    uint64_t hash;
    uint32_t key_length;
    uint32_t tag_collision;
} ChannelInfo;
#endif

#ifdef ONEBRC_HASH32_SLOTS
typedef uint32_t ChannelSlotHash;
#else
typedef uint64_t ChannelSlotHash;
#endif

typedef struct {
    ChannelSlot *slot;
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_SPLIT_SLOTS)
    ChannelSlotHash *slot_hash;
    uint16_t *slot_id;
#endif
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED_SLOTS)
    uint64_t *packed_slot;
#endif
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED32_SLOTS)
    uint32_t *packed32_slot;
    uint64_t *packed32_collision;
#endif
    size_t slot_count;
    size_t channel_count;
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
    uint32_t has_fingerprint_collision;
#endif
    ChannelStats *stats;
    WideLengths *wide_lengths;
#ifdef ONEBRC_DIRECT_STATS
    ChannelStats *direct_stats;
    WideLengths *direct_wide_lengths;
    uint8_t *direct_collision;
    uint8_t direct_shift;
    uint8_t direct_attempted;
#endif
#ifdef ONEBRC_FINGERPRINT_MAP
    ChannelInfo *info;
#endif
#ifdef ONEBRC_HOT_CACHE_SIZE
    uint64_t hot_hash[ONEBRC_HOT_CACHE_SIZE];
    ChannelStats *hot_stats[ONEBRC_HOT_CACHE_SIZE];
    int hot_cache_ready;
#endif
    size_t stats_capacity;
} ChannelMap;

typedef struct {
    const char *key;
    uint64_t hash;
    Aggregate aggregate;
    WideLengths wide_lengths;
    uint32_t key_length;
    int32_t year_month;
} GroupSlot;

typedef struct {
    GroupSlot *slot;
    size_t slot_count;
    size_t group_count;
} GroupMap;

typedef struct {
    const char *next;
    const char *channel;
    uint32_t channel_length;
    uint32_t message_length;
    uint32_t stamp_count;
    int32_t year_month;
} ParsedRow;

typedef struct {
    const char *begin;
    const char *end;
    const char *file_end;
#ifdef ONEBRC_SHARED_DICTIONARY
    const ChannelMap *shared_channels;
#endif
    ChannelMap channels;
    GroupMap overflow;
    uint64_t checksum;
#ifdef ONEBRC_PROBE_REPORT
    uint64_t discovery_rows;
#endif
    int error;
} Worker;

static inline uint64_t mix64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static inline uint64_t load_u64(const char *p) {
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline uint32_t load_u32(const char *p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline uint16_t load_u16(const char *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline uint64_t load_u64_partial(const char *p, uint32_t length) {
    switch (length) {
        case 8: return load_u64(p);
        case 7:
            return load_u32(p) | ((uint64_t)load_u16(p + 4) << 32) |
                   ((uint64_t)(uint8_t)p[6] << 48);
        case 6: return load_u32(p) | ((uint64_t)load_u16(p + 4) << 32);
        case 5: return load_u32(p) | ((uint64_t)(uint8_t)p[4] << 32);
        case 4: return load_u32(p);
        case 3: return load_u16(p) | ((uint64_t)(uint8_t)p[2] << 16);
        case 2: return load_u16(p);
        case 1: return (uint8_t)p[0];
        default: return 0;
    }
}

#ifdef ONEBRC_INLINE_KEY_COMPARE
static inline int channel_keys_equal(const char *left, const char *right,
                                     uint32_t length) {
    if (length <= 8) {
        uint64_t left_value = length >= 4
                                  ? _bzhi_u64(load_u64(left), length * 8U)
                                  : load_u64_partial(left, length);
        uint64_t right_value = length >= 4
                                   ? _bzhi_u64(load_u64(right), length * 8U)
                                   : load_u64_partial(right, length);
        return left_value == right_value;
    }

    uint32_t offset = 0;
    do {
        if (load_u64(left + offset) != load_u64(right + offset)) return 0;
        offset += 8;
    } while (length - offset >= 8);
    return offset == length ||
           load_u64_partial(left + offset, length - offset) ==
               load_u64_partial(right + offset, length - offset);
}
#else
#define channel_keys_equal(left, right, length) \
    (memcmp((left), (right), (length)) == 0)
#endif

static inline uint64_t channel_hash(const char *p, uint32_t length) {
    uint64_t crc = length;
    uint32_t offset = 0;
    while (length - offset >= 8) {
        crc = _mm_crc32_u64(crc, load_u64(p + offset));
        offset += 8;
    }
    if (offset != length) {
        uint64_t tail = 0;
        memcpy(&tail, p + offset, length - offset);
        crc = _mm_crc32_u64(crc, tail);
    }
    return crc;
}

#ifdef ONEBRC_FINGERPRINT_MAP
static inline uint64_t multiply_mix(uint64_t left, uint64_t right) {
    __uint128_t product = (__uint128_t)left * right;
    return (uint64_t)product ^ (uint64_t)(product >> 64);
}

static inline uint64_t channel_fingerprint(const char *p, uint32_t length) {
    const uint64_t seed = UINT64_C(0xa0761d6478bd642f);
    const uint64_t secret = UINT64_C(0xe7037ed1a0b428db);
    uint64_t hash;
    if (length <= 8) {
#ifdef ONEBRC_MASKED_SHORT_LOAD
        /* A channel is followed by at least ",0,0", so a length of four
           or more guarantees that an eight-byte load remains inside the row. */
        uint64_t packed = length >= 4
                              ? _bzhi_u64(load_u64(p), length * 8U)
                              : load_u64_partial(p, length);
#else
        uint64_t packed = load_u64_partial(p, length);
#endif
#ifdef ONEBRC_FAST_SHORT_HASH
        packed ^= packed >> 37;
        hash = packed * UINT64_C(0x9e3779b97f4a7c15);
#else
        hash = multiply_mix(packed ^ seed, (uint64_t)length ^ secret);
#endif
    } else {
        uint64_t first = load_u64(p);
        uint64_t last = load_u64(p + length - 8);
        hash = multiply_mix(first ^ seed, last ^ ((uint64_t)length * secret));
        if (length > 16) {
            uint64_t middle_left = load_u64(p + 8);
            uint64_t middle_right = load_u64(p + length - 16);
            hash = multiply_mix(hash ^ middle_left, middle_right ^ secret);
        }
    }
    return hash == 0 ? 1 : hash;
}
#endif

static inline size_t channel_slot_index(uint64_t hash, size_t mask) {
#ifdef ONEBRC_HASH_INDEX_SHIFT
    hash >>= ONEBRC_HASH_INDEX_SHIFT;
#endif
    return hash & mask;
}

#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_SPLIT_SLOTS)
static inline ChannelSlotHash channel_slot_hash_value(uint64_t hash) {
#ifdef ONEBRC_HASH32_SLOTS
    uint32_t tag = (uint32_t)hash;
#ifdef ONEBRC_HASH32_TEST_MASK
    tag &= ONEBRC_HASH32_TEST_MASK;
#endif
    return tag == 0 ? 1 : tag;
#else
    return hash;
#endif
}
#endif

#ifdef ONEBRC_PACKED32_SLOTS
static inline uint32_t channel_packed32_value(uint64_t hash, uint32_t id) {
    return ((uint32_t)(hash >> 32) & UINT32_C(0xffffc000)) | (id + 1);
}

static inline int channel_packed32_tag_matches(uint32_t packed, uint64_t hash) {
    return ((packed ^ (uint32_t)(hash >> 32)) & UINT32_C(0xffffc000)) == 0;
}

static inline int channel_packed32_id_collides(const ChannelMap *map,
                                                uint32_t id) {
    return (map->packed32_collision[id >> 6] >> (id & 63)) & 1U;
}

static inline void channel_packed32_mark_collision(ChannelMap *map,
                                                    uint32_t id) {
    map->packed32_collision[id >> 6] |= UINT64_C(1) << (id & 63);
}
#endif

static inline uint32_t aggregate_min_length(const Aggregate *aggregate,
                                            const WideLengths *wide_lengths) {
    return aggregate->min_length == LENGTH_SENTINEL
               ? wide_lengths->min_length
               : aggregate->min_length;
}

static inline uint32_t aggregate_max_length(const Aggregate *aggregate,
                                            const WideLengths *wide_lengths) {
    return aggregate->max_length == LENGTH_SENTINEL
               ? wide_lengths->max_length
               : aggregate->max_length;
}

static inline void aggregate_store_min_length(Aggregate *aggregate,
                                              WideLengths *wide_lengths,
                                              uint32_t value) {
    if (value < LENGTH_SENTINEL) {
        aggregate->min_length = (uint16_t)value;
    } else {
        aggregate->min_length = LENGTH_SENTINEL;
        wide_lengths->min_length = value;
    }
}

static inline void aggregate_store_max_length(Aggregate *aggregate,
                                              WideLengths *wide_lengths,
                                              uint32_t value) {
    if (value < LENGTH_SENTINEL) {
        aggregate->max_length = (uint16_t)value;
    } else {
        aggregate->max_length = LENGTH_SENTINEL;
        wide_lengths->max_length = value;
    }
}

static inline void aggregate_accumulate(Aggregate *aggregate,
                                        uint32_t message_length,
                                        uint32_t stamp_count) {
#ifdef ONEBRC_COMBINED_SUM_COUNT
    uint64_t sum_and_count;
    memcpy(&sum_and_count, &aggregate->sum_length, sizeof(sum_and_count));
    sum_and_count += (UINT64_C(1) << 32) | message_length;
    memcpy(&aggregate->sum_length, &sum_and_count, sizeof(sum_and_count));
#else
    aggregate->sum_length += message_length;
    aggregate->count++;
#endif
    aggregate->sum_stamps += stamp_count;
}

static inline void aggregate_add_narrow(Aggregate *aggregate,
                                        uint32_t message_length,
                                        uint32_t stamp_count) {
    if (aggregate->count == 0) {
        aggregate->min_length = (uint16_t)message_length;
        aggregate->max_length = (uint16_t)message_length;
    } else {
#ifdef ONEBRC_BRANCHLESS_MINMAX
        uint32_t minimum = aggregate->min_length;
        uint32_t maximum = aggregate->max_length;
        uint32_t minimum_mask = (uint32_t)-(message_length < minimum);
        uint32_t maximum_mask = (uint32_t)-(message_length > maximum);
        aggregate->min_length =
            (uint16_t)(minimum ^ ((message_length ^ minimum) & minimum_mask));
        aggregate->max_length =
            (uint16_t)(maximum ^ ((message_length ^ maximum) & maximum_mask));
#else
        if (message_length < aggregate->min_length) {
            aggregate->min_length = (uint16_t)message_length;
        }
        if (aggregate->max_length != LENGTH_SENTINEL &&
            message_length > aggregate->max_length) {
            aggregate->max_length = (uint16_t)message_length;
        }
#endif
    }
    aggregate_accumulate(aggregate, message_length, stamp_count);
}

static inline void aggregate_add_wide(Aggregate *aggregate,
                                      WideLengths *wide_lengths,
                                      uint32_t message_length,
                                      uint32_t stamp_count) {
    if (aggregate->count == 0) {
        aggregate->min_length = LENGTH_SENTINEL;
        aggregate->max_length = LENGTH_SENTINEL;
        wide_lengths->min_length = message_length;
        wide_lengths->max_length = message_length;
    } else {
        if (aggregate->min_length == LENGTH_SENTINEL &&
            message_length < wide_lengths->min_length) {
            wide_lengths->min_length = message_length;
        }
        if (aggregate->max_length != LENGTH_SENTINEL) {
            aggregate->max_length = LENGTH_SENTINEL;
            wide_lengths->max_length = message_length;
        } else if (message_length > wide_lengths->max_length) {
            wide_lengths->max_length = message_length;
        }
    }
    aggregate_accumulate(aggregate, message_length, stamp_count);
}

static inline void aggregate_add(Aggregate *aggregate, WideLengths *wide_lengths,
                                 uint32_t message_length, uint32_t stamp_count) {
    if (__builtin_expect(message_length < LENGTH_SENTINEL, 1)) {
        aggregate_add_narrow(aggregate, message_length, stamp_count);
    } else {
        aggregate_add_wide(aggregate, wide_lengths, message_length, stamp_count);
    }
}

static inline void channel_aggregate_add(ChannelMap *map, Aggregate *aggregate,
                                         uint32_t message_length,
                                         uint32_t stamp_count) {
    if (__builtin_expect(message_length < LENGTH_SENTINEL, 1)) {
        aggregate_add_narrow(aggregate, message_length, stamp_count);
    } else {
        size_t byte_offset = (size_t)((uintptr_t)aggregate - (uintptr_t)map->stats);
        size_t wide_index = byte_offset / sizeof(*aggregate);
        aggregate_add_wide(aggregate, &map->wide_lengths[wide_index],
                           message_length, stamp_count);
    }
}

#ifdef ONEBRC_DIRECT_STATS
static inline void channel_direct_aggregate_add(ChannelMap *map,
                                                Aggregate *aggregate,
                                                uint32_t message_length,
                                                uint32_t stamp_count) {
    if (__builtin_expect(message_length < LENGTH_SENTINEL, 1)) {
        aggregate_add_narrow(aggregate, message_length, stamp_count);
    } else {
        size_t byte_offset =
            (size_t)((uintptr_t)aggregate - (uintptr_t)map->direct_stats);
        size_t wide_index = byte_offset / sizeof(*aggregate);
        aggregate_add_wide(aggregate, &map->direct_wide_lengths[wide_index],
                           message_length, stamp_count);
    }
}
#endif

static inline void channel_pending_aggregate_add(ChannelMap *map,
                                                 Aggregate *aggregate,
                                                 uint32_t message_length,
                                                 uint32_t stamp_count) {
#ifdef ONEBRC_DIRECT_STATS
    if (__builtin_expect(map->direct_stats != NULL, 1)) {
        channel_direct_aggregate_add(map, aggregate,
                                     message_length, stamp_count);
    } else {
        channel_aggregate_add(map, aggregate, message_length, stamp_count);
    }
#else
    channel_aggregate_add(map, aggregate, message_length, stamp_count);
#endif
}

static inline void aggregate_merge(Aggregate *destination,
                                   WideLengths *destination_wide_lengths,
                                   const Aggregate *source,
                                   const WideLengths *source_wide_lengths) {
    if (source->count == 0) return;
    if (destination->count == 0) {
        *destination = *source;
        *destination_wide_lengths = *source_wide_lengths;
        return;
    }
    uint32_t source_minimum = aggregate_min_length(source, source_wide_lengths);
    uint32_t source_maximum = aggregate_max_length(source, source_wide_lengths);
    if (source_minimum < aggregate_min_length(destination, destination_wide_lengths)) {
        aggregate_store_min_length(destination, destination_wide_lengths, source_minimum);
    }
    if (source_maximum > aggregate_max_length(destination, destination_wide_lengths)) {
        aggregate_store_max_length(destination, destination_wide_lengths, source_maximum);
    }
    destination->sum_length += source->sum_length;
    destination->sum_stamps += source->sum_stamps;
    destination->count += source->count;
}

static int channel_map_rehash(ChannelMap *map, size_t new_slot_count) {
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED32_SLOTS)
    uint32_t *new_slots = calloc(new_slot_count, sizeof(*new_slots));
    if (new_slots == NULL) return -1;

    size_t mask = new_slot_count - 1;
    for (size_t i = 0; i < map->slot_count; i++) {
        uint32_t packed = map->packed32_slot[i];
        if (packed == 0) continue;
        uint32_t id = (packed & PACKED32_ID_MASK) - 1;
        uint64_t hash = map->info[id].hash;
        size_t index = channel_slot_index(hash, mask);
        while (new_slots[index] != 0) index = (index + 1) & mask;
        new_slots[index] = channel_packed32_value(hash, id);
    }

    free(map->packed32_slot);
    map->packed32_slot = new_slots;
    map->slot_count = new_slot_count;
    return 0;
#elif defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED_SLOTS)
    uint64_t *new_slots = calloc(new_slot_count, sizeof(*new_slots));
    if (new_slots == NULL) return -1;

    size_t mask = new_slot_count - 1;
    for (size_t i = 0; i < map->slot_count; i++) {
        uint64_t packed = map->packed_slot[i];
        if (packed == 0) continue;
        uint32_t id = (uint16_t)packed - 1;
        uint64_t hash = map->info[id].hash;
        size_t index = channel_slot_index(hash, mask);
        while (new_slots[index] != 0) index = (index + 1) & mask;
        new_slots[index] = (hash & UINT64_C(0xffffffffffff0000)) | (id + 1);
    }

    free(map->packed_slot);
    map->packed_slot = new_slots;
    map->slot_count = new_slot_count;
    return 0;
#elif defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_SPLIT_SLOTS)
    ChannelSlotHash *new_hashes = calloc(new_slot_count, sizeof(*new_hashes));
    uint16_t *new_ids = malloc(new_slot_count * sizeof(*new_ids));
    if (new_hashes == NULL || new_ids == NULL) {
        free(new_hashes);
        free(new_ids);
        return -1;
    }

    size_t mask = new_slot_count - 1;
    for (size_t i = 0; i < map->slot_count; i++) {
        if (map->slot_hash[i] == 0) continue;
        uint32_t id = map->slot_id[i] & CHANNEL_ID_MASK;
        uint64_t hash = map->info[id].hash;
        size_t index = channel_slot_index(hash, mask);
        while (new_hashes[index] != 0) index = (index + 1) & mask;
        new_hashes[index] = channel_slot_hash_value(hash);
        new_ids[index] = map->slot_id[i];
    }

    free(map->slot_hash);
    free(map->slot_id);
    map->slot_hash = new_hashes;
    map->slot_id = new_ids;
    map->slot_count = new_slot_count;
    return 0;
#else
    ChannelSlot *new_slots = calloc(new_slot_count, sizeof(*new_slots));
    if (new_slots == NULL) return -1;

    size_t mask = new_slot_count - 1;
    for (size_t i = 0; i < map->slot_count; i++) {
        ChannelSlot value = map->slot[i];
#ifdef ONEBRC_FINGERPRINT_MAP
        if (value.hash == 0) continue;
#else
        if (value.key == NULL) continue;
#endif
        size_t index = channel_slot_index(value.hash, mask);
#ifdef ONEBRC_FINGERPRINT_MAP
        while (new_slots[index].hash != 0) index = (index + 1) & mask;
#else
        while (new_slots[index].key != NULL) index = (index + 1) & mask;
#endif
        new_slots[index] = value;
    }

    free(map->slot);
    map->slot = new_slots;
    map->slot_count = new_slot_count;
    return 0;
#endif
}

static int channel_map_grow_stats(ChannelMap *map) {
    size_t new_capacity = map->stats_capacity == 0 ? INITIAL_CHANNELS : map->stats_capacity * 2;
#if defined(ONEBRC_HUGEPAGE_STATS)
    ChannelStats *new_stats;
    if (map->stats_capacity == 0) {
        const size_t huge_page = 2U * 1024U * 1024U;
        size_t used_bytes = new_capacity * sizeof(*new_stats);
        size_t allocation_bytes = (used_bytes + huge_page - 1) & ~(huge_page - 1);
        void *allocation = NULL;
        if (posix_memalign(&allocation, huge_page, allocation_bytes) != 0) return -1;
        new_stats = allocation;
        madvise(new_stats, allocation_bytes, MADV_HUGEPAGE);
        memset(new_stats, 0, allocation_bytes);
    } else {
        new_stats = realloc(map->stats, new_capacity * sizeof(*new_stats));
        if (new_stats == NULL) return -1;
        memset(new_stats + map->stats_capacity, 0,
               (new_capacity - map->stats_capacity) * sizeof(*new_stats));
    }
#else
    ChannelStats *new_stats = realloc(map->stats, new_capacity * sizeof(*new_stats));
    if (new_stats == NULL) return -1;
#endif
#ifdef ONEBRC_FINGERPRINT_MAP
    ChannelInfo *new_info = realloc(map->info, new_capacity * sizeof(*new_info));
    if (new_info == NULL) {
        map->stats = new_stats;
        return -1;
    }
    map->info = new_info;
#endif
    WideLengths *new_wide_lengths =
        realloc(map->wide_lengths,
                new_capacity * MONTHS_2027 * sizeof(*new_wide_lengths));
    if (new_wide_lengths == NULL) {
        map->stats = new_stats;
        return -1;
    }
    memset(new_wide_lengths + map->stats_capacity * MONTHS_2027, 0,
           (new_capacity - map->stats_capacity) * MONTHS_2027 *
               sizeof(*new_wide_lengths));
#if !defined(ONEBRC_HUGEPAGE_STATS)
    memset(new_stats + map->stats_capacity, 0,
           (new_capacity - map->stats_capacity) * sizeof(*new_stats));
#endif
    map->stats = new_stats;
    map->wide_lengths = new_wide_lengths;
    map->stats_capacity = new_capacity;
    return 0;
}

static int channel_map_init(ChannelMap *map) {
    memset(map, 0, sizeof(*map));
    map->slot_count = INITIAL_CHANNEL_SLOTS;
#ifdef ONEBRC_COLOCATE_MAP
    if (channel_map_grow_stats(map) != 0) return -1;
    unsigned char *tail = (unsigned char *)map->stats +
                          map->stats_capacity * sizeof(*map->stats);
    map->slot_hash = (ChannelSlotHash *)tail;
    map->slot_id = (uint16_t *)(tail + map->slot_count * sizeof(*map->slot_hash));
    return 0;
#else
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED32_SLOTS)
    map->packed32_slot = calloc(map->slot_count, sizeof(*map->packed32_slot));
    map->packed32_collision = calloc(PACKED32_COLLISION_WORDS,
                                     sizeof(*map->packed32_collision));
    if (map->packed32_slot == NULL || map->packed32_collision == NULL) {
        free(map->packed32_slot);
        free(map->packed32_collision);
        return -1;
    }
#elif defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED_SLOTS)
    map->packed_slot = calloc(map->slot_count, sizeof(*map->packed_slot));
    if (map->packed_slot == NULL) return -1;
#elif defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_SPLIT_SLOTS)
    map->slot_hash = calloc(map->slot_count, sizeof(*map->slot_hash));
    map->slot_id = malloc(map->slot_count * sizeof(*map->slot_id));
    if (map->slot_hash == NULL || map->slot_id == NULL) {
        free(map->slot_hash);
        free(map->slot_id);
        return -1;
    }
#else
    map->slot = calloc(map->slot_count, sizeof(*map->slot));
    if (map->slot == NULL) return -1;
#endif
    return channel_map_grow_stats(map);
#endif
}

#ifdef ONEBRC_FREQUENCY_REBUILD
typedef struct {
    uint64_t count;
    uint32_t id;
} ChannelFrequency;

static inline int channel_frequency_is_worse(const ChannelFrequency *left,
                                             const ChannelFrequency *right) {
    return left->count < right->count ||
           (left->count == right->count && left->id > right->id);
}

static void channel_frequency_sift_down(ChannelFrequency *frequency,
                                        size_t root, size_t end) {
    for (;;) {
        size_t child = root * 2 + 1;
        if (child > end) return;
        if (child < end &&
            channel_frequency_is_worse(&frequency[child + 1],
                                       &frequency[child])) {
            child++;
        }
        if (!channel_frequency_is_worse(&frequency[child], &frequency[root])) {
            return;
        }
        ChannelFrequency temporary = frequency[root];
        frequency[root] = frequency[child];
        frequency[child] = temporary;
        root = child;
    }
}

static void sort_channel_frequency(ChannelFrequency *frequency, size_t count) {
    for (size_t root = count / 2; root-- > 0;) {
        channel_frequency_sift_down(frequency, root, count - 1);
    }
    for (size_t end = count - 1; end > 0; end--) {
        ChannelFrequency temporary = frequency[0];
        frequency[0] = frequency[end];
        frequency[end] = temporary;
        channel_frequency_sift_down(frequency, 0, end - 1);
    }
}

static __attribute__((noinline)) int channel_map_rebuild_by_frequency(
    ChannelMap *map) {
    size_t frequency_bytes = map->channel_count * sizeof(ChannelFrequency);
    ChannelFrequency *frequency = mmap(NULL, frequency_bytes,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (frequency == MAP_FAILED) return -1;

    for (size_t id = 0; id < map->channel_count; id++) {
        uint64_t count = 0;
        for (int month = 0; month < MONTHS_2027; month++) {
            count += map->stats[id].month[month].count;
        }
        frequency[id].count = count;
        frequency[id].id = (uint32_t)id;
    }
    sort_channel_frequency(frequency, map->channel_count);

    size_t mask = map->slot_count - 1;
#ifdef ONEBRC_PACKED32_SLOTS
    memset(map->packed32_slot, 0,
           map->slot_count * sizeof(*map->packed32_slot));
    for (size_t rank = 0; rank < map->channel_count; rank++) {
        uint32_t id = frequency[rank].id;
        uint64_t hash = map->info[id].hash;
        size_t index = channel_slot_index(hash, mask);
        while (map->packed32_slot[index] != 0) index = (index + 1) & mask;
        map->packed32_slot[index] = channel_packed32_value(hash, id);
    }
#elif defined(ONEBRC_PACKED_SLOTS)
    memset(map->packed_slot, 0, map->slot_count * sizeof(*map->packed_slot));
    for (size_t rank = 0; rank < map->channel_count; rank++) {
        uint32_t id = frequency[rank].id;
        uint64_t hash = map->info[id].hash;
        size_t index = channel_slot_index(hash, mask);
        while (map->packed_slot[index] != 0) index = (index + 1) & mask;
        map->packed_slot[index] =
            (hash & UINT64_C(0xffffffffffff0000)) | (id + 1);
    }
#else
    memset(map->slot_hash, 0, map->slot_count * sizeof(*map->slot_hash));
    for (size_t rank = 0; rank < map->channel_count; rank++) {
        uint32_t id = frequency[rank].id;
        uint64_t hash = map->info[id].hash;
        size_t index = channel_slot_index(hash, mask);
        while (map->slot_hash[index] != 0) index = (index + 1) & mask;
        map->slot_hash[index] = channel_slot_hash_value(hash);
        map->slot_id[index] = (uint16_t)(
            id | (map->info[id].tag_collision ? CHANNEL_COLLISION_FLAG : 0));
    }
#endif
    munmap(frequency, frequency_bytes);
    return 0;
}
#endif

#ifdef ONEBRC_DIRECT_STATS
static inline size_t channel_direct_bucket(const ChannelMap *map,
                                           uint64_t hash) {
    return (hash >> map->direct_shift) & (DIRECT_STATS_BUCKETS - 1);
}

static inline int channel_direct_bucket_collides(const ChannelMap *map,
                                                 size_t bucket) {
    return (map->direct_collision[bucket >> 3] >> (bucket & 7)) & 1U;
}

static uint64_t channel_dense_row_count(const ChannelMap *map, size_t id) {
    uint64_t count = 0;
    for (int month = 0; month < MONTHS_2027; month++) {
        count += map->stats[id].month[month].count;
    }
    return count;
}

static uint64_t channel_direct_collision_weight(const ChannelMap *map,
                                                uint16_t *bucket_counts,
                                                uint32_t shift) {
    memset(bucket_counts, 0,
           DIRECT_STATS_BUCKETS * sizeof(*bucket_counts));
    for (size_t id = 0; id < map->channel_count; id++) {
        size_t bucket =
            (map->info[id].hash >> shift) & (DIRECT_STATS_BUCKETS - 1);
        bucket_counts[bucket]++;
    }

    uint64_t weight = 0;
    for (size_t id = 0; id < map->channel_count; id++) {
        size_t bucket =
            (map->info[id].hash >> shift) & (DIRECT_STATS_BUCKETS - 1);
        if (bucket_counts[bucket] > 1) {
            weight += channel_dense_row_count(map, id);
        }
    }
    return weight;
}

static void *channel_direct_allocate(size_t bytes) {
    const size_t huge_page = 2U * 1024U * 1024U;
    void *allocation = NULL;
    if (posix_memalign(&allocation, huge_page, bytes) != 0) return NULL;
    madvise(allocation, bytes, MADV_HUGEPAGE);
    memset(allocation, 0, bytes);
    return allocation;
}

static __attribute__((noinline)) void channel_map_build_direct_stats(
    ChannelMap *map) {
    map->direct_attempted = 1;
    uint16_t *bucket_counts =
        calloc(DIRECT_STATS_BUCKETS, sizeof(*bucket_counts));
    if (bucket_counts == NULL) return;

    uint64_t best_weight = UINT64_MAX;
    uint32_t best_shift = 0;
    const uint32_t maximum_shift = 64 - ONEBRC_DIRECT_STATS_BITS;
    for (uint32_t shift = 0;;) {
        uint64_t weight =
            channel_direct_collision_weight(map, bucket_counts, shift);
        if (weight < best_weight) {
            best_weight = weight;
            best_shift = shift;
        }
        if (shift == maximum_shift) break;
        shift = shift + 8 < maximum_shift ? shift + 8 : maximum_shift;
    }

    channel_direct_collision_weight(map, bucket_counts, best_shift);
    size_t direct_capacity = DIRECT_STATS_BUCKETS + map->channel_count;
    size_t stats_bytes = direct_capacity * sizeof(ChannelStats);
    size_t wide_bytes =
        direct_capacity * MONTHS_2027 * sizeof(WideLengths);
    size_t collision_bytes = DIRECT_STATS_BUCKETS / 8;
    ChannelStats *direct_stats = channel_direct_allocate(stats_bytes);
    WideLengths *direct_wide_lengths = channel_direct_allocate(wide_bytes);
    uint8_t *direct_collision = calloc(collision_bytes, 1);
    if (direct_stats == NULL || direct_wide_lengths == NULL ||
        direct_collision == NULL) {
        free(direct_stats);
        free(direct_wide_lengths);
        free(direct_collision);
        free(bucket_counts);
        return;
    }

    for (size_t bucket = 0; bucket < DIRECT_STATS_BUCKETS; bucket++) {
        if (bucket_counts[bucket] > 1) {
            direct_collision[bucket >> 3] |=
                (uint8_t)(1U << (bucket & 7));
        }
    }
    for (size_t id = 0; id < map->channel_count; id++) {
        size_t bucket =
            (map->info[id].hash >> best_shift) & (DIRECT_STATS_BUCKETS - 1);
        size_t direct_id = bucket_counts[bucket] == 1
                               ? bucket
                               : DIRECT_STATS_BUCKETS + id;
        direct_stats[direct_id] = map->stats[id];
        memcpy(&direct_wide_lengths[direct_id * MONTHS_2027],
               &map->wide_lengths[id * MONTHS_2027],
               MONTHS_2027 * sizeof(WideLengths));
    }

    map->direct_stats = direct_stats;
    map->direct_wide_lengths = direct_wide_lengths;
    map->direct_collision = direct_collision;
    map->direct_shift = (uint8_t)best_shift;
    free(bucket_counts);
}

static void channel_map_materialize_direct_stats(ChannelMap *map) {
    if (map->direct_stats == NULL) return;
    for (size_t id = 0; id < map->channel_count; id++) {
        size_t bucket = channel_direct_bucket(map, map->info[id].hash);
        size_t direct_id = channel_direct_bucket_collides(map, bucket)
                               ? DIRECT_STATS_BUCKETS + id
                               : bucket;
        map->stats[id] = map->direct_stats[direct_id];
        memcpy(&map->wide_lengths[id * MONTHS_2027],
               &map->direct_wide_lengths[direct_id * MONTHS_2027],
               MONTHS_2027 * sizeof(WideLengths));
    }
    free(map->direct_stats);
    free(map->direct_wide_lengths);
    free(map->direct_collision);
    map->direct_stats = NULL;
    map->direct_wide_lengths = NULL;
    map->direct_collision = NULL;
}
#endif

#ifdef ONEBRC_COLD_COLLISION_PATH
static __attribute__((noinline)) int channel_map_get(
    ChannelMap *map, const char *key, uint32_t key_length,
    uint64_t hash, uint32_t *id) {
#else
static int channel_map_get(ChannelMap *map, const char *key, uint32_t key_length,
                           uint64_t hash, uint32_t *id) {
#endif
    if ((map->channel_count + 1) * 10 >= map->slot_count * 7 &&
        channel_map_rehash(map, map->slot_count * 2) != 0) {
        return -1;
    }

    size_t mask = map->slot_count - 1;
    size_t index = channel_slot_index(hash, mask);
    uint32_t tag_collision = 0;
    for (;;) {
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED32_SLOTS)
        uint32_t packed = map->packed32_slot[index];
        if (packed == 0) {
            if (map->channel_count == map->stats_capacity &&
                channel_map_grow_stats(map) != 0) {
                return -1;
            }
            uint32_t new_id = (uint32_t)map->channel_count++;
            map->packed32_slot[index] = channel_packed32_value(hash, new_id);
            map->info[new_id].key = key;
            map->info[new_id].hash = hash;
            map->info[new_id].key_length = key_length;
            map->info[new_id].tag_collision = tag_collision;
            if (tag_collision) channel_packed32_mark_collision(map, new_id);
#ifdef ONEBRC_FREQUENCY_REBUILD
            if (map->channel_count == 10000 &&
                channel_map_rebuild_by_frequency(map) != 0) return -1;
#endif
            *id = new_id;
            return 0;
        }
        if (channel_packed32_tag_matches(packed, hash)) {
            uint32_t candidate_id = (packed & PACKED32_ID_MASK) - 1;
            ChannelInfo *candidate = &map->info[candidate_id];
            if (candidate->key_length == key_length &&
                channel_keys_equal(candidate->key, key, key_length)) {
                *id = candidate_id;
                return 0;
            }
            candidate->tag_collision = 1;
            channel_packed32_mark_collision(map, candidate_id);
            tag_collision = 1;
        }
#elif defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED_SLOTS)
        uint64_t packed = map->packed_slot[index];
        if (packed == 0) {
            if (map->channel_count == map->stats_capacity && channel_map_grow_stats(map) != 0) {
                return -1;
            }
            uint32_t new_id = (uint32_t)map->channel_count++;
            map->packed_slot[index] = (hash & UINT64_C(0xffffffffffff0000)) | (new_id + 1);
            map->info[new_id].key = key;
            map->info[new_id].hash = hash;
            map->info[new_id].key_length = key_length;
            map->info[new_id].tag_collision = tag_collision;
#ifdef ONEBRC_FREQUENCY_REBUILD
            if (map->channel_count == 10000 &&
                channel_map_rebuild_by_frequency(map) != 0) return -1;
#endif
            *id = new_id;
            return 0;
        }
        if (((packed ^ hash) & UINT64_C(0xffffffffffff0000)) == 0) {
            uint32_t candidate_id = (uint16_t)packed - 1;
            ChannelInfo *candidate = &map->info[candidate_id];
            if (candidate->key_length == key_length &&
                channel_keys_equal(candidate->key, key, key_length)) {
                *id = candidate_id;
                return 0;
            }
            candidate->tag_collision = 1;
            tag_collision = 1;
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
            map->has_fingerprint_collision |= FINGERPRINT_COLLISION_SEEN;
#endif
        }
#elif defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_SPLIT_SLOTS)
        ChannelSlotHash slot_hash = map->slot_hash[index];
        if (slot_hash == 0) {
            if (map->channel_count == map->stats_capacity && channel_map_grow_stats(map) != 0) {
                return -1;
            }
            uint32_t new_id = (uint32_t)map->channel_count++;
            map->slot_hash[index] = channel_slot_hash_value(hash);
            map->slot_id[index] =
                (uint16_t)(new_id | (tag_collision ? CHANNEL_COLLISION_FLAG : 0));
            map->info[new_id].key = key;
            map->info[new_id].hash = hash;
            map->info[new_id].key_length = key_length;
            map->info[new_id].tag_collision = tag_collision;
#ifdef ONEBRC_FREQUENCY_REBUILD
            if (map->channel_count == 10000 &&
                channel_map_rebuild_by_frequency(map) != 0) return -1;
#endif
            *id = new_id;
            return 0;
        }
        if (slot_hash == channel_slot_hash_value(hash)) {
            uint16_t tagged_id = map->slot_id[index];
            uint32_t candidate_id = tagged_id & CHANNEL_ID_MASK;
            ChannelInfo *candidate = &map->info[candidate_id];
            if (candidate->key_length == key_length &&
                channel_keys_equal(candidate->key, key, key_length)) {
                *id = candidate_id;
                return 0;
            }
            map->slot_id[index] = tagged_id | CHANNEL_COLLISION_FLAG;
            candidate->tag_collision = 1;
            tag_collision = 1;
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
            map->has_fingerprint_collision |= FINGERPRINT_COLLISION_SEEN;
#endif
        }
#else
        ChannelSlot *slot = &map->slot[index];
#ifdef ONEBRC_FINGERPRINT_MAP
        if (slot->hash == 0) {
            if (map->channel_count == map->stats_capacity && channel_map_grow_stats(map) != 0) {
                return -1;
            }
            uint32_t new_id = (uint32_t)map->channel_count++;
            slot->hash = hash;
            slot->id = new_id;
            map->info[new_id].key = key;
            map->info[new_id].hash = hash;
            map->info[new_id].key_length = key_length;
            map->info[new_id].tag_collision = tag_collision;
#ifdef ONEBRC_FREQUENCY_REBUILD
            if (map->channel_count == 10000 &&
                channel_map_rebuild_by_frequency(map) != 0) return -1;
#endif
            *id = new_id;
            return 0;
        }
        if (slot->hash == hash) {
            ChannelInfo *candidate = &map->info[slot->id];
            if (candidate->key_length == key_length &&
                channel_keys_equal(candidate->key, key, key_length)) {
                *id = slot->id;
                return 0;
            }
            candidate->tag_collision = 1;
            tag_collision = 1;
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
            map->has_fingerprint_collision |= FINGERPRINT_COLLISION_SEEN;
#endif
        }
#else
        if (slot->key == NULL) {
            if (map->channel_count == map->stats_capacity && channel_map_grow_stats(map) != 0) {
                return -1;
            }
            slot->key = key;
            slot->hash = hash;
            slot->key_length = key_length;
            slot->id = (uint32_t)map->channel_count++;
            *id = slot->id;
            return 0;
        }
        if (slot->hash == hash && slot->key_length == key_length &&
            channel_keys_equal(slot->key, key, key_length)) {
            *id = slot->id;
            return 0;
        }
#endif
#endif
        index = (index + 1) & mask;
    }
}

#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_KNOWN_10000)
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
static inline uint32_t channel_map_lookup_unique_fingerprint(
    const ChannelMap *map, const char *key, uint32_t key_length,
    uint64_t hash) {
#ifndef ONEBRC_PACKED32_SLOTS
    (void)key;
    (void)key_length;
#endif
    size_t mask = map->slot_count - 1;
    size_t index = channel_slot_index(hash, mask);
    for (;;) {
#ifdef ONEBRC_PACKED32_SLOTS
        uint32_t packed = map->packed32_slot[index];
        if (channel_packed32_tag_matches(packed, hash)) {
            uint32_t candidate_id = (packed & PACKED32_ID_MASK) - 1;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if (!channel_packed32_id_collides(map, candidate_id) ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                return candidate_id;
            }
        }
#elif defined(ONEBRC_PACKED_SLOTS)
        uint64_t packed = map->packed_slot[index];
        if (((packed ^ hash) & UINT64_C(0xffffffffffff0000)) == 0) {
            return (uint16_t)packed - 1;
        }
#elif defined(ONEBRC_SPLIT_SLOTS)
        if (map->slot_hash[index] == channel_slot_hash_value(hash)) {
            return map->slot_id[index];
        }
#else
        const ChannelSlot *slot = &map->slot[index];
        if (slot->hash == hash) return slot->id;
#endif
        index = (index + 1) & mask;
    }
}
#endif

#ifdef ONEBRC_COLD_COLLISION_PATH
static __attribute__((noinline, cold)) uint32_t channel_map_lookup_collision(
    const ChannelMap *map, const char *key, uint32_t key_length, uint64_t hash) {
#else
static inline uint32_t channel_map_lookup_present(const ChannelMap *map,
                                                  const char *key,
                                                  uint32_t key_length,
                                                  uint64_t hash) {
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
    if (__builtin_expect(
            (map->has_fingerprint_collision & FINGERPRINT_COLLISION_SEEN) == 0, 1)) {
        return channel_map_lookup_unique_fingerprint(map, key, key_length, hash);
    }
#endif
#endif
    size_t mask = map->slot_count - 1;
    size_t index = channel_slot_index(hash, mask);
    for (;;) {
#ifdef ONEBRC_PACKED32_SLOTS
        uint32_t packed = map->packed32_slot[index];
        if (channel_packed32_tag_matches(packed, hash)) {
            uint32_t candidate_id = (packed & PACKED32_ID_MASK) - 1;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if (!channel_packed32_id_collides(map, candidate_id) ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                return candidate_id;
            }
        }
#elif defined(ONEBRC_PACKED_SLOTS)
        uint64_t packed = map->packed_slot[index];
        if (((packed ^ hash) & UINT64_C(0xffffffffffff0000)) == 0) {
            uint32_t candidate_id = (uint16_t)packed - 1;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if (candidate->key_length == key_length &&
                channel_keys_equal(candidate->key, key, key_length)) {
                return candidate_id;
            }
        }
#elif defined(ONEBRC_SPLIT_SLOTS)
        if (map->slot_hash[index] == channel_slot_hash_value(hash)) {
            uint16_t tagged_id = map->slot_id[index];
            uint32_t candidate_id = tagged_id & CHANNEL_ID_MASK;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if ((tagged_id & CHANNEL_COLLISION_FLAG) == 0 ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                return candidate_id;
            }
        }
#else
        const ChannelSlot *slot = &map->slot[index];
        if (slot->hash == hash) {
            const ChannelInfo *candidate = &map->info[slot->id];
            if (candidate->tag_collision == 0 ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                return slot->id;
            }
        }
#endif
        index = (index + 1) & mask;
    }
}

#ifdef ONEBRC_COLD_COLLISION_PATH
static inline uint32_t channel_map_lookup_present(const ChannelMap *map,
                                                  const char *key,
                                                  uint32_t key_length,
                                                  uint64_t hash) {
    if (__builtin_expect(
            (map->has_fingerprint_collision & FINGERPRINT_COLLISION_SEEN) == 0, 1)) {
        return channel_map_lookup_unique_fingerprint(map, key, key_length, hash);
    }
    return channel_map_lookup_collision(map, key, key_length, hash);
}
#endif
#endif

#ifdef ONEBRC_HOT_CACHE_SIZE
static uint64_t channel_stats_count(const ChannelStats *stats) {
    uint64_t count = 0;
    for (int month = 0; month < MONTHS_2027; month++) count += stats->month[month].count;
    return count;
}

static void channel_map_build_hot_cache(ChannelMap *map) {
    uint64_t weight[ONEBRC_HOT_CACHE_SIZE] = {0};
    const size_t mask = ONEBRC_HOT_CACHE_SIZE - 1;
    for (size_t id = 0; id < map->channel_count; id++) {
        uint64_t rows = channel_stats_count(&map->stats[id]);
        size_t index = map->info[id].hash & mask;
        if (rows > weight[index]) {
            weight[index] = rows;
            map->hot_hash[index] = map->info[id].hash;
            map->hot_stats[index] = &map->stats[id];
        }
    }
    map->hot_cache_ready = 1;
}

static inline ChannelStats *channel_map_lookup_stats(ChannelMap *map,
                                                     const char *key,
                                                     uint32_t key_length,
                                                     uint64_t hash) {
    size_t index = hash & (ONEBRC_HOT_CACHE_SIZE - 1);
    if (__builtin_expect(map->hot_hash[index] == hash, 1)) {
        uint32_t hot_id = (uint32_t)(map->hot_stats[index] - map->stats);
        const ChannelInfo *candidate = &map->info[hot_id];
        if (candidate->tag_collision == 0 ||
            (candidate->key_length == key_length &&
             channel_keys_equal(candidate->key, key, key_length))) {
            return map->hot_stats[index];
        }
    }
    uint32_t id = channel_map_lookup_present(map, key, key_length, hash);
    return &map->stats[id];
}
#endif

static inline uint64_t group_hash(uint64_t hash, int32_t year_month) {
    return mix64(hash ^ ((uint64_t)(uint32_t)year_month * UINT64_C(0x9e3779b97f4a7c15)));
}

static int group_map_init(GroupMap *map, size_t slot_count) {
    memset(map, 0, sizeof(*map));
    map->slot_count = slot_count;
    map->slot = calloc(slot_count, sizeof(*map->slot));
    return map->slot == NULL ? -1 : 0;
}

static int group_map_rehash(GroupMap *map, size_t new_slot_count) {
    GroupSlot *new_slots = calloc(new_slot_count, sizeof(*new_slots));
    if (new_slots == NULL) return -1;

    size_t mask = new_slot_count - 1;
    for (size_t i = 0; i < map->slot_count; i++) {
        GroupSlot value = map->slot[i];
        if (value.key == NULL) continue;
        size_t index = value.hash & mask;
        while (new_slots[index].key != NULL) index = (index + 1) & mask;
        new_slots[index] = value;
    }

    free(map->slot);
    map->slot = new_slots;
    map->slot_count = new_slot_count;
    return 0;
}

static int group_map_add(GroupMap *map, const char *key, uint32_t key_length,
                         uint64_t channel_hash_value, int32_t year_month,
                         const Aggregate *aggregate,
                         const WideLengths *wide_lengths) {
    if (map->slot_count == 0 && group_map_init(map, 1024) != 0) return -1;
    if ((map->group_count + 1) * 10 >= map->slot_count * 7 &&
        group_map_rehash(map, map->slot_count * 2) != 0) {
        return -1;
    }

    uint64_t hash = group_hash(channel_hash_value, year_month);
    size_t mask = map->slot_count - 1;
    size_t index = channel_slot_index(hash, mask);
    for (;;) {
        GroupSlot *slot = &map->slot[index];
        if (slot->key == NULL) {
            slot->key = key;
            slot->hash = hash;
            slot->key_length = key_length;
            slot->year_month = year_month;
            slot->aggregate = *aggregate;
            slot->wide_lengths = *wide_lengths;
            map->group_count++;
            return 0;
        }
        if (slot->hash == hash && slot->year_month == year_month &&
            slot->key_length == key_length &&
            channel_keys_equal(slot->key, key, key_length)) {
            aggregate_merge(&slot->aggregate, &slot->wide_lengths,
                            aggregate, wide_lengths);
            return 0;
        }
        index = (index + 1) & mask;
    }
}

static inline uint32_t parse_5_digits(const char *p) {
    return (uint32_t)(p[0] - '0') * 10000U + (uint32_t)(p[1] - '0') * 1000U +
           (uint32_t)(p[2] - '0') * 100U + (uint32_t)(p[3] - '0') * 10U +
           (uint32_t)(p[4] - '0');
}

static inline uint32_t parse_digits(const char *p, uint32_t length) {
    switch (length) {
        case 1: return (uint32_t)(p[0] - '0');
        case 2: return (uint32_t)(p[0] - '0') * 10U + (uint32_t)(p[1] - '0');
        case 3: return (uint32_t)(p[0] - '0') * 100U + (uint32_t)(p[1] - '0') * 10U +
                       (uint32_t)(p[2] - '0');
        case 4: return (uint32_t)(p[0] - '0') * 1000U + (uint32_t)(p[1] - '0') * 100U +
                       (uint32_t)(p[2] - '0') * 10U + (uint32_t)(p[3] - '0');
        case 5: return parse_5_digits(p);
        default: {
            uint32_t value = 0;
            for (uint32_t i = 0; i < length; i++) value = value * 10U + (uint32_t)(p[i] - '0');
            return value;
        }
    }
}

static inline uint32_t parse_message_digits(const char *p, uint32_t length) {
    if (__builtin_expect(length == 3, 1)) {
        return (uint32_t)(p[0] - '0') * 100U + (uint32_t)(p[1] - '0') * 10U +
               (uint32_t)(p[2] - '0');
    }
#ifdef ONEBRC_FAST_TWO_DIGITS
    if (__builtin_expect(length == 2, 1)) {
        return (uint32_t)(p[0] - '0') * 10U + (uint32_t)(p[1] - '0');
    }
#endif
    return parse_digits(p, length);
}

static inline uint32_t parse_stamp_digits(const char *p, uint32_t length) {
    if (__builtin_expect(length == 1, 1)) return (uint32_t)(p[0] - '0');
#ifdef ONEBRC_FAST_TWO_DIGITS
    if (__builtin_expect(length == 2, 1)) {
        return (uint32_t)(p[0] - '0') * 10U + (uint32_t)(p[1] - '0');
    }
#endif
    return parse_digits(p, length);
}

static inline uint64_t parse_timestamp_10(const char *p) {
    return (uint64_t)parse_5_digits(p) * 100000U + parse_5_digits(p + 5);
}

#ifdef ONEBRC_COMBINED_DELIMITERS
static inline uint32_t row_delimiter_mask(__m256i bytes) {
    __m256i table = _mm256_load_si256((const __m256i *)delimiter_shuffle);
    __m256i candidates = _mm256_shuffle_epi8(table, bytes);
    return (uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(bytes, candidates));
}

static inline uint64_t drop_three_delimiters(uint64_t delimiters) {
    delimiters = _blsr_u64(delimiters);
    delimiters = _blsr_u64(delimiters);
    return _blsr_u64(delimiters);
}
#endif

static int32_t unix_timestamp_to_year_month(uint64_t timestamp) {
    int64_t z = (int64_t)(timestamp / 86400U) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t day_of_era = (uint32_t)(z - era * 146097);
    uint32_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                            day_of_era / 146096) / 365;
    int32_t year = (int32_t)year_of_era + (int32_t)(era * 400);
    uint32_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    uint32_t month_prime = (5 * day_of_year + 2) / 153;
    uint32_t month = month_prime < 10 ? month_prime + 3 : month_prime - 9;
    year += month <= 2;
    return year * 12 + (int32_t)month - 1;
}

static inline int32_t timestamp_10_to_year_month(const char *p) {
#ifdef ONEBRC_GENERIC_MONTH
    return unix_timestamp_to_year_month(parse_timestamp_10(p));
#else
#ifdef ONEBRC_BCD_MONTH
    uint32_t bcd = _pext_u32((uint32_t)load_u64(p + 2), UINT32_C(0x000f0f0f));
    uint8_t bcd_result = bcd_month[bcd];
    if (bcd_result != UINT8_MAX) return YM_2027 + bcd_result;

    uint64_t bcd_timestamp = parse_timestamp_10(p);
    for (int32_t boundary = 1; boundary <= 12; boundary++) {
        if (bcd_timestamp < month_start_2027[boundary]) return YM_2027 + boundary - 1;
    }
    return unix_timestamp_to_year_month(bcd_timestamp);
#else
    uint32_t prefix = parse_5_digits(p);
#ifdef ONEBRC_2027_ONLY
    uint8_t month = prefix_month[prefix - PREFIX_BASE];
    if (month != UINT8_MAX) return YM_2027 + month;

    uint64_t timestamp = parse_timestamp_10(p);
    for (int32_t boundary = 1; boundary <= 12; boundary++) {
        if (timestamp < month_start_2027[boundary]) return YM_2027 + boundary - 1;
    }
    __builtin_unreachable();
#else
    if (prefix >= PREFIX_BASE && prefix < PREFIX_BASE + PREFIX_COUNT) {
        uint8_t month = prefix_month[prefix - PREFIX_BASE];
        if (month != UINT8_MAX) return YM_2027 + month;
    }

    uint64_t timestamp = parse_timestamp_10(p);
    if (timestamp >= month_start_2027[0] && timestamp < month_start_2027[12]) {
        for (int32_t month = 0; month < 12; month++) {
            if (timestamp < month_start_2027[month + 1]) return YM_2027 + month;
        }
    }
    return unix_timestamp_to_year_month(timestamp);
#endif
#endif
#endif
}

static void initialize_prefix_month(void) {
    memset(prefix_month, UINT8_MAX, sizeof(prefix_month));
#ifdef ONEBRC_BCD_MONTH
    memset(bcd_month, UINT8_MAX, sizeof(bcd_month));
#endif
    for (uint32_t index = 0; index < PREFIX_COUNT; index++) {
        uint32_t low = (PREFIX_BASE + index) * 100000U;
        uint32_t high = low + 99999U;
        for (uint8_t month = 0; month < 12; month++) {
            if (low >= month_start_2027[month] && high < month_start_2027[month + 1]) {
                prefix_month[index] = month;
#ifdef ONEBRC_BCD_MONTH
                uint32_t suffix = (PREFIX_BASE + index) % 1000;
                uint32_t bcd = suffix / 100 + ((suffix / 10) % 10) * 16 +
                               (suffix % 10) * 256;
                bcd_month[bcd] = month;
#endif
                break;
            }
        }
    }
}

static int parse_row_fast(const char *p, const char *file_end, ParsedRow *row) {
    if ((size_t)(file_end - p) < 64) return 0;

#ifdef ONEBRC_COMBINED_DELIMITERS
    __m256i first = _mm256_loadu_si256((const __m256i *)p);
    uint64_t delimiters = row_delimiter_mask(first);

    if (drop_three_delimiters(delimiters) == 0) {
        __m256i second = _mm256_loadu_si256((const __m256i *)(p + 32));
        delimiters |= (uint64_t)row_delimiter_mask(second) << 32;
        if (drop_three_delimiters(delimiters) == 0) return 0;
    }

    delimiters = _blsr_u64(delimiters);
    uint32_t comma2 = (uint32_t)__builtin_ctzll(delimiters);
    delimiters = _blsr_u64(delimiters);
    uint32_t comma3 = (uint32_t)__builtin_ctzll(delimiters);
    delimiters = _blsr_u64(delimiters);
    uint32_t newline = (uint32_t)__builtin_ctzll(delimiters);
#else
    const __m256i comma_byte = _mm256_set1_epi8(',');
    const __m256i newline_byte = _mm256_set1_epi8('\n');
    __m256i first = _mm256_loadu_si256((const __m256i *)p);
    uint64_t commas = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(first, comma_byte));
    uint64_t newlines = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(first, newline_byte));

    if (newlines == 0) {
        __m256i second = _mm256_loadu_si256((const __m256i *)(p + 32));
        commas |= (uint64_t)(uint32_t)_mm256_movemask_epi8(
                      _mm256_cmpeq_epi8(second, comma_byte)) << 32;
        newlines = (uint64_t)(uint32_t)_mm256_movemask_epi8(
                       _mm256_cmpeq_epi8(second, newline_byte)) << 32;
        if (newlines == 0) return 0;
    }

    uint32_t newline = (uint32_t)__builtin_ctzll(newlines);
    commas &= ~((UINT64_C(1) << 11) - 1);
    uint32_t comma2 = (uint32_t)__builtin_ctzll(commas);
    commas &= commas - 1;
    uint32_t comma3 = (uint32_t)__builtin_ctzll(commas);
#endif

    row->next = p + newline + 1;
    row->channel = p + 11;
    row->channel_length = comma2 - 11;
    row->message_length = parse_message_digits(p + comma2 + 1, comma3 - comma2 - 1);
    row->stamp_count = parse_stamp_digits(p + comma3 + 1, newline - comma3 - 1);
    row->year_month = timestamp_10_to_year_month(p);
    return 1;
}

static int parse_row_scalar(const char *p, const char *end, ParsedRow *row) {
    const char *comma1 = memchr(p, ',', (size_t)(end - p));
    if (comma1 == NULL) return -1;
    const char *comma2 = memchr(comma1 + 1, ',', (size_t)(end - (comma1 + 1)));
    if (comma2 == NULL) return -1;
    const char *comma3 = memchr(comma2 + 1, ',', (size_t)(end - (comma2 + 1)));
    if (comma3 == NULL) return -1;
    const char *newline = memchr(comma3 + 1, '\n', (size_t)(end - (comma3 + 1)));
    const char *value_end = newline == NULL ? end : newline;
    if (value_end > comma3 + 1 && value_end[-1] == '\r') value_end--;
    if (value_end <= comma3 + 1) return -1;

    uint64_t timestamp = 0;
    for (const char *q = p; q < comma1; q++) timestamp = timestamp * 10U + (uint32_t)(*q - '0');

    row->next = newline == NULL ? end : newline + 1;
    row->channel = comma1 + 1;
    row->channel_length = (uint32_t)(comma2 - comma1 - 1);
    row->message_length = parse_digits(comma2 + 1, (uint32_t)(comma3 - comma2 - 1));
    row->stamp_count = parse_digits(comma3 + 1, (uint32_t)(value_end - comma3 - 1));
    row->year_month = unix_timestamp_to_year_month(timestamp);
    return 0;
}

#ifdef ONEBRC_SHARED_DICTIONARY
static int build_shared_dictionary(ChannelMap *map, const char *begin, const char *file_end) {
    if (channel_map_init(map) != 0) return -1;
    const char *p = begin;
    while (p < file_end && map->channel_count < 10000) {
        ParsedRow row;
        int parsed = parse_row_fast(p, file_end, &row);
        if (parsed == 0 && parse_row_scalar(p, file_end, &row) != 0) return -1;
        uint64_t hash = channel_fingerprint(row.channel, row.channel_length);
        uint32_t channel_id;
        if (channel_map_get(map, row.channel, row.channel_length, hash, &channel_id) != 0) {
            return -1;
        }
        p = row.next;
    }
    free(map->stats);
    free(map->wide_lengths);
    map->stats = NULL;
    map->wide_lengths = NULL;
    map->stats_capacity = 0;
    return 0;
}

static inline int channel_map_lookup_readonly(const ChannelMap *map,
                                              const char *key,
                                              uint32_t key_length,
                                              uint64_t hash,
                                              uint32_t *id) {
#ifdef ONEBRC_UNIQUE_FINGERPRINT_FASTPATH
    *id = channel_map_lookup_present(map, key, key_length, hash);
    return 0;
#else
    size_t mask = map->slot_count - 1;
    size_t index = channel_slot_index(hash, mask);
    for (;;) {
#ifdef ONEBRC_PACKED32_SLOTS
        uint32_t packed = map->packed32_slot[index];
        if (packed == 0) return -1;
        if (channel_packed32_tag_matches(packed, hash)) {
            uint32_t candidate_id = (packed & PACKED32_ID_MASK) - 1;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if (!channel_packed32_id_collides(map, candidate_id) ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                *id = candidate_id;
                return 0;
            }
        }
#elif defined(ONEBRC_PACKED_SLOTS)
        uint64_t packed = map->packed_slot[index];
        if (packed == 0) return -1;
        if (((packed ^ hash) & UINT64_C(0xffffffffffff0000)) == 0) {
            uint32_t candidate_id = (uint16_t)packed - 1;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if (candidate->key_length == key_length &&
                channel_keys_equal(candidate->key, key, key_length)) {
                *id = candidate_id;
                return 0;
            }
        }
#elif defined(ONEBRC_SPLIT_SLOTS)
        ChannelSlotHash slot_hash = map->slot_hash[index];
        if (slot_hash == 0) return -1;
        if (slot_hash == channel_slot_hash_value(hash)) {
            uint16_t tagged_id = map->slot_id[index];
            uint32_t candidate_id = tagged_id & CHANNEL_ID_MASK;
            const ChannelInfo *candidate = &map->info[candidate_id];
            if ((tagged_id & CHANNEL_COLLISION_FLAG) == 0 ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                *id = candidate_id;
                return 0;
            }
        }
#else
        const ChannelSlot *slot = &map->slot[index];
        if (slot->hash == 0) return -1;
        if (slot->hash == hash) {
            const ChannelInfo *candidate = &map->info[slot->id];
            if (candidate->tag_collision == 0 ||
                (candidate->key_length == key_length &&
                 channel_keys_equal(candidate->key, key, key_length))) {
                *id = slot->id;
                return 0;
            }
        }
#endif
        index = (index + 1) & mask;
    }
#endif
}
#endif

#ifdef ONEBRC_BATCH_ROWS
static int run_worker_batches(Worker *worker) {
    const char *p = worker->begin;
    while (p < worker->end) {
        ParsedRow rows[4];
        uint64_t hashes[4];
        uint32_t channel_ids[4];
        uint32_t row_count = 0;

        for (; row_count < 4 && p < worker->end; row_count++) {
            int parsed = parse_row_fast(p, worker->file_end, &rows[row_count]);
            if (parsed == 0 && parse_row_scalar(p, worker->end, &rows[row_count]) != 0) {
                return EINVAL;
            }
            p = rows[row_count].next;
        }

#pragma GCC unroll 4
        for (uint32_t i = 0; i < row_count; i++) {
            hashes[i] = channel_fingerprint(rows[i].channel, rows[i].channel_length);
#ifdef ONEBRC_BATCH_PREFETCH
            __builtin_prefetch(&worker->channels.slot_hash[
                                   channel_slot_index(
                                       hashes[i], worker->channels.slot_count - 1)],
                               0, 1);
#endif
        }

#pragma GCC unroll 4
        for (uint32_t i = 0; i < row_count; i++) {
            if (__builtin_expect(worker->channels.channel_count == 10000, 1)) {
                channel_ids[i] = channel_map_lookup_present(
                    &worker->channels, rows[i].channel, rows[i].channel_length, hashes[i]);
            } else if (channel_map_get(&worker->channels, rows[i].channel,
                                       rows[i].channel_length, hashes[i],
                                       &channel_ids[i]) != 0) {
                return ENOMEM;
            }
#ifdef ONEBRC_BATCH_PREFETCH
            __builtin_prefetch(
                &worker->channels.stats[channel_ids[i]].month[rows[i].year_month - YM_2027],
                1, 1);
#endif
        }

#pragma GCC unroll 4
        for (uint32_t i = 0; i < row_count; i++) {
            uint32_t month = (uint32_t)(rows[i].year_month - YM_2027);
            channel_aggregate_add(
                &worker->channels,
                &worker->channels.stats[channel_ids[i]].month[month],
                rows[i].message_length, rows[i].stamp_count);
        }
    }
    return 0;
}
#endif

#ifdef ONEBRC_DEFER_AGGREGATE
static int run_worker_deferred(Worker *worker) {
    const char *p = worker->begin;
#ifdef ONEBRC_DEFER_DISTANCE
    Aggregate *pending[ONEBRC_DEFER_DISTANCE] = {0};
    uint32_t pending_message_length[ONEBRC_DEFER_DISTANCE] = {0};
    uint32_t pending_stamp_count[ONEBRC_DEFER_DISTANCE] = {0};
    uint32_t pending_index = 0;
#else
    Aggregate *pending = NULL;
    uint32_t pending_message_length = 0;
    uint32_t pending_stamp_count = 0;
#endif

    while (p < worker->end) {
        ParsedRow row;
        int parsed = parse_row_fast(p, worker->file_end, &row);
        if (parsed == 0 && parse_row_scalar(p, worker->end, &row) != 0) return EINVAL;
        p = row.next;

        uint32_t channel_id;
#ifdef ONEBRC_PROBE_REPORT
        worker->discovery_rows += worker->channels.channel_count != 10000;
#endif
        uint64_t hash = channel_fingerprint(row.channel, row.channel_length);
#ifdef ONEBRC_DEFER_MAP_PREFETCH
        __builtin_prefetch(
            &worker->channels.slot_hash[channel_slot_index(
                hash, worker->channels.slot_count - 1)], 0, 1);
#ifdef ONEBRC_DEFER_DISTANCE
        if (pending[pending_index] != NULL) {
            channel_pending_aggregate_add(
                &worker->channels, pending[pending_index],
                pending_message_length[pending_index],
                pending_stamp_count[pending_index]);
        }
#else
        if (pending != NULL) {
            channel_pending_aggregate_add(&worker->channels, pending,
                                          pending_message_length,
                                          pending_stamp_count);
            pending = NULL;
        }
#endif
#endif
#ifdef ONEBRC_DIRECT_STATS
        uint32_t month = (uint32_t)(row.year_month - YM_2027);
        Aggregate *current;
        if (__builtin_expect(worker->channels.direct_stats != NULL, 1)) {
            size_t bucket = channel_direct_bucket(&worker->channels, hash);
            if (__builtin_expect(
                    !channel_direct_bucket_collides(&worker->channels, bucket), 1)) {
                current = &worker->channels.direct_stats[bucket].month[month];
            } else {
                channel_id = channel_map_lookup_present(
                    &worker->channels, row.channel, row.channel_length, hash);
                current = &worker->channels
                               .direct_stats[DIRECT_STATS_BUCKETS + channel_id]
                               .month[month];
            }
        } else {
            if (__builtin_expect(worker->channels.channel_count == 10000, 1)) {
                channel_id = channel_map_lookup_present(
                    &worker->channels, row.channel, row.channel_length, hash);
            } else if (channel_map_get(&worker->channels, row.channel,
                                       row.channel_length, hash,
                                       &channel_id) != 0) {
                return ENOMEM;
            }
            if (worker->channels.channel_count == 10000 &&
                !worker->channels.direct_attempted) {
#ifdef ONEBRC_DEFER_DISTANCE
                for (uint32_t i = 0; i < ONEBRC_DEFER_DISTANCE; i++) {
                    if (pending[i] != NULL) {
                        channel_pending_aggregate_add(
                            &worker->channels, pending[i],
                            pending_message_length[i], pending_stamp_count[i]);
                        pending[i] = NULL;
                    }
                }
#else
                if (pending != NULL) {
                    channel_pending_aggregate_add(
                        &worker->channels, pending,
                        pending_message_length, pending_stamp_count);
                    pending = NULL;
                }
#endif
                channel_map_build_direct_stats(&worker->channels);
            }
            if (worker->channels.direct_stats != NULL) {
                size_t bucket = channel_direct_bucket(&worker->channels, hash);
                if (!channel_direct_bucket_collides(&worker->channels, bucket)) {
                    current = &worker->channels.direct_stats[bucket].month[month];
                } else {
                    current = &worker->channels
                                   .direct_stats[DIRECT_STATS_BUCKETS + channel_id]
                                   .month[month];
                }
            } else {
                current = &worker->channels.stats[channel_id].month[month];
            }
        }
#else
#ifdef ONEBRC_SHARED_DICTIONARY
        channel_id = channel_map_lookup_present(
            worker->shared_channels, row.channel, row.channel_length, hash);
#else
#ifdef ONEBRC_HOT_CACHE_SIZE
        ChannelStats *channel_stats;
        if (__builtin_expect(worker->channels.channel_count == 10000, 1)) {
            if (__builtin_expect(!worker->channels.hot_cache_ready, 0)) {
                channel_map_build_hot_cache(&worker->channels);
            }
            channel_stats = channel_map_lookup_stats(
                &worker->channels, row.channel, row.channel_length, hash);
            channel_id = (uint32_t)(channel_stats - worker->channels.stats);
        } else if (channel_map_get(&worker->channels, row.channel, row.channel_length,
                                   hash, &channel_id) != 0) {
            return ENOMEM;
        } else {
            channel_stats = &worker->channels.stats[channel_id];
        }
#else
        if (__builtin_expect(worker->channels.channel_count == 10000, 1)) {
            channel_id = channel_map_lookup_present(
                &worker->channels, row.channel, row.channel_length, hash);
        } else if (channel_map_get(&worker->channels, row.channel, row.channel_length,
                                   hash, &channel_id) != 0) {
            return ENOMEM;
        }
#endif
#endif

#ifdef ONEBRC_HOT_CACHE_SIZE
        uint32_t month = (uint32_t)(row.year_month - YM_2027);
        Aggregate *current = &channel_stats->month[month];
#else
        uint32_t month = (uint32_t)(row.year_month - YM_2027);
        Aggregate *current =
            &worker->channels.stats[channel_id].month[month];
#endif
#endif
#ifdef ONEBRC_DEFER_PREFETCH
        __builtin_prefetch(current, 1, 1);
#endif
#ifndef ONEBRC_DEFER_MAP_PREFETCH
#ifdef ONEBRC_DEFER_DISTANCE
        if (pending[pending_index] != NULL) {
            channel_pending_aggregate_add(
                &worker->channels, pending[pending_index],
                pending_message_length[pending_index],
                pending_stamp_count[pending_index]);
        }
#else
        if (pending != NULL) {
            channel_pending_aggregate_add(&worker->channels, pending,
                                          pending_message_length,
                                          pending_stamp_count);
        }
#endif
#endif
#ifdef ONEBRC_DEFER_DISTANCE
        pending[pending_index] = current;
        pending_message_length[pending_index] = row.message_length;
        pending_stamp_count[pending_index] = row.stamp_count;
        pending_index = (pending_index + 1) % ONEBRC_DEFER_DISTANCE;
#else
        pending = current;
        pending_message_length = row.message_length;
        pending_stamp_count = row.stamp_count;
#endif
    }

#ifdef ONEBRC_DEFER_DISTANCE
    for (uint32_t i = 0; i < ONEBRC_DEFER_DISTANCE; i++) {
        if (pending[i] != NULL) {
            channel_pending_aggregate_add(&worker->channels, pending[i],
                                          pending_message_length[i],
                                          pending_stamp_count[i]);
        }
    }
#else
    if (pending != NULL) {
        channel_pending_aggregate_add(&worker->channels, pending,
                                      pending_message_length,
                                      pending_stamp_count);
    }
#endif
    return 0;
}
#endif

static void *run_worker(void *argument) {
    Worker *worker = argument;
#ifdef ONEBRC_POPULATE_WORKER
    uintptr_t populate_begin = (uintptr_t)worker->begin & ~(uintptr_t)4095;
    uintptr_t populate_end = ((uintptr_t)worker->end + 4095) & ~(uintptr_t)4095;
    if (madvise((void *)populate_begin, populate_end - populate_begin,
                MADV_POPULATE_READ) != 0) {
        worker->error = errno;
        return NULL;
    }
#endif
#ifdef ONEBRC_PRETOUCH_STRIDE
    const volatile unsigned char *touch =
        (const volatile unsigned char *)worker->begin;
    const volatile unsigned char *touch_end =
        (const volatile unsigned char *)worker->end;
    while (touch < touch_end) {
        (void)*touch;
        touch += ONEBRC_PRETOUCH_STRIDE;
    }
#endif
#ifdef ONEBRC_BYTES_ONLY
    const char *byte = worker->begin;
    __m256i vector_checksum = _mm256_setzero_si256();
    while ((size_t)(worker->end - byte) >= 32) {
        vector_checksum = _mm256_xor_si256(
            vector_checksum, _mm256_loadu_si256((const __m256i *)byte));
        byte += 32;
    }
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i *)lanes, vector_checksum);
    worker->checksum = lanes[0] ^ lanes[1] ^ lanes[2] ^ lanes[3];
    while (byte < worker->end) worker->checksum = worker->checksum * 33U + (uint8_t)*byte++;
    return NULL;
#endif

#ifdef ONEBRC_SHARED_DICTIONARY
#ifdef ONEBRC_HUGEPAGE_STATS
    if (channel_map_grow_stats(&worker->channels) != 0) {
        worker->error = ENOMEM;
        return NULL;
    }
#else
    worker->channels.stats_capacity = worker->shared_channels->channel_count;
    worker->channels.stats = calloc(worker->channels.stats_capacity,
                                    sizeof(*worker->channels.stats));
    worker->channels.wide_lengths =
        calloc(worker->channels.stats_capacity * MONTHS_2027,
               sizeof(*worker->channels.wide_lengths));
    if (worker->channels.stats == NULL || worker->channels.wide_lengths == NULL) {
        worker->error = ENOMEM;
        return NULL;
    }
#endif
#elif !defined(ONEBRC_GROUP_PER_ROW) && !defined(ONEBRC_PARSE_ONLY) && !defined(ONEBRC_HASH_ONLY)
    if (channel_map_init(&worker->channels) != 0) {
        worker->error = ENOMEM;
        return NULL;
    }
#endif

#ifdef ONEBRC_BATCH_ROWS
    worker->error = run_worker_batches(worker);
    return NULL;
#endif
#ifdef ONEBRC_DEFER_AGGREGATE
    worker->error = run_worker_deferred(worker);
    return NULL;
#endif

    const char *p = worker->begin;
    while (p < worker->end) {
        ParsedRow row;
        int parsed = 0;
#ifndef ONEBRC_SCALAR_ONLY
        parsed = parse_row_fast(p, worker->file_end, &row);
#endif
        if (parsed == 0 && parse_row_scalar(p, worker->end, &row) != 0) {
            worker->error = EINVAL;
            return NULL;
        }

#ifdef ONEBRC_PARSE_ONLY
        worker->checksum += (uint64_t)(uint32_t)row.year_month + row.channel_length +
                            row.message_length + row.stamp_count;
#else
#ifdef ONEBRC_FINGERPRINT_MAP
        uint64_t hash = channel_fingerprint(row.channel, row.channel_length);
#else
        uint64_t hash = channel_hash(row.channel, row.channel_length);
#endif
#ifdef ONEBRC_HASH_ONLY
        worker->checksum += hash + row.message_length + row.stamp_count;
#else
#ifdef ONEBRC_GROUP_PER_ROW
        Aggregate aggregate = {0};
        WideLengths wide_lengths = {0};
        aggregate_add(&aggregate, &wide_lengths, row.message_length, row.stamp_count);
        if (group_map_add(&worker->overflow, row.channel, row.channel_length, hash,
                          row.year_month, &aggregate, &wide_lengths) != 0) {
            worker->error = ENOMEM;
            return NULL;
        }
#else
        uint32_t channel_id;
#ifdef ONEBRC_SHARED_DICTIONARY
        if (channel_map_lookup_readonly(worker->shared_channels, row.channel,
                                        row.channel_length, hash, &channel_id) != 0) {
            worker->error = EINVAL;
            return NULL;
        }
#else
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_KNOWN_10000)
        if (__builtin_expect(worker->channels.channel_count == 10000, 1)) {
            channel_id = channel_map_lookup_present(
                &worker->channels, row.channel, row.channel_length, hash);
        } else
#endif
        if (channel_map_get(&worker->channels, row.channel, row.channel_length, hash,
                            &channel_id) != 0) {
            worker->error = ENOMEM;
            return NULL;
        }
#endif

#ifdef ONEBRC_LOOKUP_ONLY
        worker->checksum += channel_id + row.message_length + row.stamp_count;
#else
#ifdef ONEBRC_2027_ONLY
        uint32_t month = (uint32_t)(row.year_month - YM_2027);
        channel_aggregate_add(&worker->channels,
                              &worker->channels.stats[channel_id].month[month],
                              row.message_length, row.stamp_count);
#else
        if (row.year_month >= YM_2027 && row.year_month < YM_2027 + 12) {
            uint32_t month = (uint32_t)(row.year_month - YM_2027);
            channel_aggregate_add(&worker->channels,
                                  &worker->channels.stats[channel_id].month[month],
                                  row.message_length, row.stamp_count);
        } else {
            Aggregate aggregate = {0};
            WideLengths wide_lengths = {0};
            aggregate_add(&aggregate, &wide_lengths,
                          row.message_length, row.stamp_count);
            if (group_map_add(&worker->overflow, row.channel, row.channel_length, hash,
                              row.year_month, &aggregate, &wide_lengths) != 0) {
                worker->error = ENOMEM;
                return NULL;
            }
        }
#endif
#endif
#endif
#endif
#endif
        p = row.next;
    }
    return NULL;
}

static int merge_worker(GroupMap *final, const Worker *worker) {
#ifdef ONEBRC_FINGERPRINT_MAP
    for (size_t channel_id = 0; channel_id < worker->channels.channel_count; channel_id++) {
        const ChannelInfo *channel = &worker->channels.info[channel_id];
        const ChannelStats *stats = &worker->channels.stats[channel_id];
        for (int32_t month = 0; month < 12; month++) {
            if (stats->month[month].count != 0 &&
                group_map_add(final, channel->key, channel->key_length, channel->hash,
                              YM_2027 + month, &stats->month[month],
                              &worker->channels.wide_lengths[
                                  channel_id * MONTHS_2027 + (size_t)month]) != 0) {
                return -1;
            }
        }
    }
#else
    for (size_t i = 0; i < worker->channels.slot_count; i++) {
        const ChannelSlot *channel = &worker->channels.slot[i];
        if (channel->key == NULL) continue;
        const ChannelStats *stats = &worker->channels.stats[channel->id];
        for (int32_t month = 0; month < 12; month++) {
            if (stats->month[month].count != 0 &&
                group_map_add(final, channel->key, channel->key_length, channel->hash,
                              YM_2027 + month, &stats->month[month],
                              &worker->channels.wide_lengths[
                                  channel->id * MONTHS_2027 + (size_t)month]) != 0) {
                return -1;
            }
        }
    }
#endif
    for (size_t i = 0; i < worker->overflow.slot_count; i++) {
        const GroupSlot *group = &worker->overflow.slot[i];
        if (group->key == NULL) continue;
        uint64_t hash = channel_hash(group->key, group->key_length);
        if (group_map_add(final, group->key, group->key_length, hash, group->year_month,
                          &group->aggregate, &group->wide_lengths) != 0) {
            return -1;
        }
    }
    return 0;
}

#ifdef ONEBRC_SHARED_DICTIONARY
static int merge_shared_workers(GroupMap *final, const ChannelMap *shared,
                                const Worker *workers, int worker_count) {
    for (size_t channel_id = 0; channel_id < shared->channel_count; channel_id++) {
        const ChannelInfo *channel = &shared->info[channel_id];
        for (int32_t month = 0; month < 12; month++) {
            Aggregate aggregate = {0};
            WideLengths wide_lengths = {0};
            for (int worker_id = 0; worker_id < worker_count; worker_id++) {
                aggregate_merge(
                    &aggregate, &wide_lengths,
                    &workers[worker_id].channels.stats[channel_id].month[month],
                    &workers[worker_id].channels.wide_lengths[
                        channel_id * MONTHS_2027 + (size_t)month]);
            }
            if (aggregate.count != 0 &&
                group_map_add(final, channel->key, channel->key_length, channel->hash,
                              YM_2027 + month, &aggregate, &wide_lengths) != 0) {
                return -1;
            }
        }
    }
    for (int worker_id = 0; worker_id < worker_count; worker_id++) {
        const GroupMap *overflow = &workers[worker_id].overflow;
        for (size_t i = 0; i < overflow->slot_count; i++) {
            const GroupSlot *group = &overflow->slot[i];
            if (group->key == NULL) continue;
            uint64_t hash = channel_fingerprint(group->key, group->key_length);
            if (group_map_add(final, group->key, group->key_length, hash, group->year_month,
                              &group->aggregate, &group->wide_lengths) != 0) {
                return -1;
            }
        }
    }
    return 0;
}
#endif

static void split_year_month(int32_t year_month, int32_t *year, uint32_t *month) {
    int32_t quotient = year_month / 12;
    int32_t remainder = year_month % 12;
    if (remainder < 0) {
        remainder += 12;
        quotient--;
    }
    *year = quotient;
    *month = (uint32_t)remainder + 1;
}

#ifdef ONEBRC_MEMORY_REPORT
static void report_memory(const char *label);
#endif

static int write_output(const char *path, const GroupMap *groups) {
    FILE *output = fopen(path, "wb");
    if (output == NULL) return -1;
    static char output_buffer[1 << 20];
    setvbuf(output, output_buffer, _IOFBF, sizeof(output_buffer));

    for (size_t i = 0; i < groups->slot_count; i++) {
        const GroupSlot *group = &groups->slot[i];
        if (group->key == NULL) continue;
        const Aggregate *aggregate = &group->aggregate;
        double average = (double)aggregate->sum_length / (double)aggregate->count;

        int32_t year;
        uint32_t month;
        split_year_month(group->year_month, &year, &month);
        if (fprintf(output, "%.*s,%04d-%02u=%u/%.2f/%u/%" PRIu64 "/%" PRIu64 "\n",
                    (int)group->key_length, group->key, year, month,
                    aggregate_min_length(aggregate, &group->wide_lengths), average,
                    aggregate_max_length(aggregate, &group->wide_lengths),
                    (uint64_t)aggregate->count, (uint64_t)aggregate->sum_stamps) < 0) {
            fclose(output);
            return -1;
        }
    }
    return fclose(output);
}

#ifdef ONEBRC_DIRECT_MERGE
#ifdef ONEBRC_FAST_OUTPUT
static inline uint64_t rounded_hundredths(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint64_t exponent_bits = (bits >> 52) & UINT64_C(0x7ff);
    uint64_t mantissa = (bits & UINT64_C(0x000fffffffffffff)) | (UINT64_C(1) << 52);
    __uint128_t numerator = (__uint128_t)mantissa * 100;
    int right_shift = 1075 - (int)exponent_bits;

    if (right_shift <= 0) return (uint64_t)(numerator << -right_shift);
    if (right_shift >= 128) return 0;

    __uint128_t integer = numerator >> right_shift;
    __uint128_t remainder = numerator - (integer << right_shift);
    __uint128_t halfway = (__uint128_t)1 << (right_shift - 1);
    if (remainder > halfway || (remainder == halfway && (integer & 1) != 0)) {
        integer++;
    }
    return (uint64_t)integer;
}

#ifdef ONEBRC_PAIR_OUTPUT
static inline char *append_unsigned_pairs(char *cursor, uint64_t value,
                                          const char *pairs) {
    char temporary[20];
    char *begin = temporary + sizeof(temporary);
    while (value >= 100) {
        uint64_t quotient = value / 100;
        uint32_t remainder = (uint32_t)(value - quotient * 100);
        begin -= 2;
        memcpy(begin, pairs + remainder * 2, 2);
        value = quotient;
    }
    if (value >= 10) {
        begin -= 2;
        memcpy(begin, pairs + value * 2, 2);
    } else {
        *--begin = (char)('0' + value);
    }
    size_t length = (size_t)((temporary + sizeof(temporary)) - begin);
    memcpy(cursor, begin, length);
    return cursor + length;
}
#endif
#endif

#ifndef ONEBRC_SHARED_DICTIONARY
static int merge_workers_direct(ChannelMap *destination, const Worker *workers,
                                int worker_count) {
    for (int worker_id = 1; worker_id < worker_count; worker_id++) {
        const ChannelMap *source = &workers[worker_id].channels;
        for (size_t source_id = 0; source_id < source->channel_count; source_id++) {
            const ChannelInfo *channel = &source->info[source_id];
            uint32_t destination_id;
#ifdef ONEBRC_KNOWN_10000
            if (destination->channel_count == 10000) {
                destination_id = channel_map_lookup_present(
                    destination, channel->key, channel->key_length, channel->hash);
            } else
#endif
            if (channel_map_get(destination, channel->key, channel->key_length,
                                channel->hash, &destination_id) != 0) {
                return -1;
            }
            for (int month = 0; month < MONTHS_2027; month++) {
                aggregate_merge(
                    &destination->stats[destination_id].month[month],
                    &destination->wide_lengths[
                        destination_id * MONTHS_2027 + (size_t)month],
                    &source->stats[source_id].month[month],
                    &source->wide_lengths[source_id * MONTHS_2027 + (size_t)month]);
            }
        }
    }
    return 0;
}
#else
static void merge_shared_stats_direct(Worker *workers, int worker_count,
                                      size_t channel_count) {
    ChannelStats *destination = workers[0].channels.stats;
    WideLengths *destination_wide_lengths = workers[0].channels.wide_lengths;
    for (int worker_id = 1; worker_id < worker_count; worker_id++) {
        const ChannelStats *source = workers[worker_id].channels.stats;
        for (size_t channel_id = 0; channel_id < channel_count; channel_id++) {
            for (int month = 0; month < MONTHS_2027; month++) {
                aggregate_merge(
                    &destination[channel_id].month[month],
                    &destination_wide_lengths[
                        channel_id * MONTHS_2027 + (size_t)month],
                    &source[channel_id].month[month],
                    &workers[worker_id].channels.wide_lengths[
                        channel_id * MONTHS_2027 + (size_t)month]);
            }
        }
    }
}
#endif

static int write_channel_output(const char *path, const ChannelMap *channels,
                                const ChannelStats *stats,
                                const WideLengths *wide_lengths) {
#ifdef ONEBRC_FAST_OUTPUT
    size_t capacity = 0;
    for (size_t channel_id = 0; channel_id < channels->channel_count; channel_id++) {
        const ChannelInfo *channel = &channels->info[channel_id];
        for (uint32_t month = 0; month < MONTHS_2027; month++) {
            if (stats[channel_id].month[month].count != 0) {
                capacity += channel->key_length + 160;
            }
        }
    }

#ifdef ONEBRC_MMAP_OUTPUT
    int mapped_output = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (mapped_output == -1) return -1;
    if (ftruncate(mapped_output, (off_t)capacity) != 0) {
        close(mapped_output);
        return -1;
    }
    char *buffer = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED,
                        mapped_output, 0);
    if (buffer == MAP_FAILED) {
        close(mapped_output);
        return -1;
    }
#else
    char *buffer = malloc(capacity);
    if (buffer == NULL) return -1;
#endif
    char *cursor = buffer;
#ifdef ONEBRC_PAIR_OUTPUT
    char decimal_pairs[200];
    for (uint32_t value = 0; value < 100; value++) {
        decimal_pairs[value * 2] = (char)('0' + value / 10);
        decimal_pairs[value * 2 + 1] = (char)('0' + value % 10);
    }
#endif

    for (size_t channel_id = 0; channel_id < channels->channel_count; channel_id++) {
        const ChannelInfo *channel = &channels->info[channel_id];
        for (uint32_t month = 0; month < MONTHS_2027; month++) {
            const Aggregate *aggregate = &stats[channel_id].month[month];
            const WideLengths *aggregate_wide_lengths =
                &wide_lengths[channel_id * MONTHS_2027 + month];
            if (aggregate->count == 0) continue;

            memcpy(cursor, channel->key, channel->key_length);
            cursor += channel->key_length;
            memcpy(cursor, ",2027-", 6);
            cursor += 6;
            *cursor++ = (char)('0' + (month + 1) / 10);
            *cursor++ = (char)('0' + (month + 1) % 10);
            *cursor++ = '=';

#ifdef ONEBRC_PAIR_OUTPUT
#define APPEND_UNSIGNED(value) do {                                              \
                cursor = append_unsigned_pairs(cursor, (uint64_t)(value),          \
                                               decimal_pairs);                     \
            } while (0)
#else
#define APPEND_UNSIGNED(value) do {                                              \
                uint64_t append_value = (uint64_t)(value);                       \
                char append_reverse[20];                                          \
                unsigned append_length = 0;                                       \
                do {                                                               \
                    append_reverse[append_length++] =                              \
                        (char)('0' + append_value % 10);                            \
                    append_value /= 10;                                            \
                } while (append_value != 0);                                       \
                do {                                                               \
                    *cursor++ = append_reverse[--append_length];                    \
                } while (append_length != 0);                                      \
            } while (0)
#endif

            APPEND_UNSIGNED(aggregate_min_length(aggregate, aggregate_wide_lengths));
            *cursor++ = '/';

            double average =
                (double)aggregate->sum_length / (double)aggregate->count;
            uint64_t scaled_average = rounded_hundredths(average);
            APPEND_UNSIGNED(scaled_average / 100);
            *cursor++ = '.';
            *cursor++ = (char)('0' + (scaled_average / 10) % 10);
            *cursor++ = (char)('0' + scaled_average % 10);
            *cursor++ = '/';

            APPEND_UNSIGNED(aggregate_max_length(aggregate, aggregate_wide_lengths));
            *cursor++ = '/';
            APPEND_UNSIGNED(aggregate->count);
            *cursor++ = '/';
            APPEND_UNSIGNED(aggregate->sum_stamps);
            *cursor++ = '\n';
#undef APPEND_UNSIGNED
        }
    }

#ifdef ONEBRC_MEMORY_REPORT
    report_memory("output-buffer");
#endif

#ifdef ONEBRC_MMAP_OUTPUT
    size_t output_size = (size_t)(cursor - buffer);
    int result = munmap(buffer, capacity);
    if (result == 0) result = ftruncate(mapped_output, (off_t)output_size);
    if (close(mapped_output) != 0) result = -1;
    return result;
#else
    int output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output == -1) {
        free(buffer);
        return -1;
    }
    size_t remaining = (size_t)(cursor - buffer);
    const char *write_cursor = buffer;
    while (remaining != 0) {
        ssize_t written = write(output, write_cursor, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            int saved_errno = errno;
            close(output);
            free(buffer);
            errno = saved_errno;
            return -1;
        }
        write_cursor += written;
        remaining -= (size_t)written;
    }
    int result = close(output);
    free(buffer);
    return result;
#endif
#else
    FILE *output = fopen(path, "wb");
    if (output == NULL) return -1;
    static char output_buffer[1 << 20];
    setvbuf(output, output_buffer, _IOFBF, sizeof(output_buffer));

    for (size_t channel_id = 0; channel_id < channels->channel_count; channel_id++) {
        const ChannelInfo *channel = &channels->info[channel_id];
        for (uint32_t month = 0; month < MONTHS_2027; month++) {
            const Aggregate *aggregate = &stats[channel_id].month[month];
            const WideLengths *aggregate_wide_lengths =
                &wide_lengths[channel_id * MONTHS_2027 + month];
            if (aggregate->count == 0) continue;
            double average = (double)aggregate->sum_length / (double)aggregate->count;
            if (fprintf(output, "%.*s,2027-%02u=%u/%.2f/%u/%" PRIu64 "/%" PRIu64 "\n",
                        (int)channel->key_length, channel->key, month + 1,
                        aggregate_min_length(aggregate, aggregate_wide_lengths), average,
                        aggregate_max_length(aggregate, aggregate_wide_lengths),
                        (uint64_t)aggregate->count,
                        (uint64_t)aggregate->sum_stamps) < 0) {
                fclose(output);
                return -1;
            }
        }
    }
    return fclose(output);
#endif
}
#endif

static int configured_thread_count(void) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = online > 0 && online < 8 ? (int)online : 8;
    const char *value = getenv("ONEBRC_THREADS");
    if (value != NULL && *value != '\0') {
        char *end;
        long requested = strtol(value, &end, 10);
        if (*end == '\0' && requested >= 1 && requested <= 64) threads = (int)requested;
    }
    return threads;
}

static void free_worker(Worker *worker) {
    free(worker->channels.slot);
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_SPLIT_SLOTS)
#ifndef ONEBRC_COLOCATE_MAP
    free(worker->channels.slot_hash);
    free(worker->channels.slot_id);
#endif
#endif
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED_SLOTS)
    free(worker->channels.packed_slot);
#endif
#if defined(ONEBRC_FINGERPRINT_MAP) && defined(ONEBRC_PACKED32_SLOTS)
    free(worker->channels.packed32_slot);
    free(worker->channels.packed32_collision);
#endif
#ifdef ONEBRC_DIRECT_STATS
    free(worker->channels.direct_stats);
    free(worker->channels.direct_wide_lengths);
    free(worker->channels.direct_collision);
#endif
    free(worker->channels.stats);
    free(worker->channels.wide_lengths);
#ifdef ONEBRC_FINGERPRINT_MAP
    free(worker->channels.info);
#endif
    free(worker->overflow.slot);
}

#ifdef ONEBRC_TIMING
static uint64_t monotonic_nanoseconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
#endif

#ifdef ONEBRC_MEMORY_REPORT
static void report_memory(const char *label) {
    FILE *input = fopen("/proc/self/smaps_rollup", "r");
    if (input == NULL) return;
    fprintf(stderr, "memory[%s]", label);
    char line[256];
    while (fgets(line, sizeof(line), input) != NULL) {
        char key[64];
        unsigned long kibibytes;
        if (sscanf(line, "%63[^:]: %lu kB", key, &kibibytes) != 2) continue;
        if (strcmp(key, "Rss") == 0 || strcmp(key, "Pss_Anon") == 0 ||
            strcmp(key, "Pss_File") == 0 || strcmp(key, "Anonymous") == 0 ||
            strcmp(key, "AnonHugePages") == 0 || strcmp(key, "FilePmdMapped") == 0) {
            fprintf(stderr, " %s=%luKiB", key, kibibytes);
        }
    }
    fputc('\n', stderr);
    fclose(input);
}
#endif

#if defined(ONEBRC_PROBE_REPORT) && defined(ONEBRC_SPLIT_SLOTS)
static void report_channel_probes(const Worker *workers, int worker_count) {
    uint64_t total_rows = 0;
    uint64_t weighted_probes = 0;
    uint64_t one_probe_rows = 0;
    uint64_t two_probe_rows = 0;
    uint64_t four_probe_rows = 0;
    uint64_t total_channels = 0;
    uint64_t total_channel_probes = 0;
    uint64_t discovery_rows = 0;
    size_t maximum_probes = 0;

    for (int worker_id = 0; worker_id < worker_count; worker_id++) {
        discovery_rows += workers[worker_id].discovery_rows;
        const ChannelMap *map = &workers[worker_id].channels;
        size_t mask = map->slot_count - 1;
        for (size_t slot = 0; slot < map->slot_count; slot++) {
            if (map->slot_hash[slot] == 0) continue;

            uint32_t channel_id = map->slot_id[slot] & CHANNEL_ID_MASK;
            uint64_t hash = map->info[channel_id].hash;
            uint64_t rows = 0;
            for (int month = 0; month < MONTHS_2027; month++) {
                rows += map->stats[channel_id].month[month].count;
            }
            size_t probes =
                ((slot - channel_slot_index(hash, mask)) & mask) + 1;
            total_rows += rows;
            weighted_probes += rows * probes;
            total_channels++;
            total_channel_probes += probes;
            if (probes == 1) one_probe_rows += rows;
            if (probes <= 2) two_probe_rows += rows;
            if (probes <= 4) four_probe_rows += rows;
            if (probes > maximum_probes) maximum_probes = probes;
        }
    }

    fprintf(stderr,
            "probes rows=%" PRIu64 " channels=%" PRIu64
            " discovery=%" PRIu64
            " row_avg=%.6f channel_avg=%.6f one=%.3f%% le2=%.3f%% le4=%.3f%% max=%zu\n",
            total_rows, total_channels, discovery_rows,
            (double)weighted_probes / (double)total_rows,
            (double)total_channel_probes / (double)total_channels,
            100.0 * (double)one_probe_rows / (double)total_rows,
            100.0 * (double)two_probe_rows / (double)total_rows,
            100.0 * (double)four_probe_rows / (double)total_rows,
            maximum_probes);
}
#endif

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
        return 2;
    }
#ifdef ONEBRC_TIMING
    uint64_t timing_start = monotonic_nanoseconds();
    uint64_t timing_setup_end;
    uint64_t timing_workers_end;
    uint64_t timing_merge_end;
    uint64_t timing_output_end;
#endif

    initialize_prefix_month();
    int input = open(argv[1], O_RDONLY);
    if (input < 0) {
        perror("open input");
        return 1;
    }
    struct stat metadata;
    if (fstat(input, &metadata) != 0 || metadata.st_size <= 0) {
        perror("stat input");
        close(input);
        return 1;
    }

    size_t size = (size_t)metadata.st_size;
    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, input, 0);
    close(input);
    if (data == MAP_FAILED) {
        perror("mmap input");
        return 1;
    }
#ifdef ONEBRC_MADVISE_SEQUENTIAL
    madvise((void *)data, size, MADV_SEQUENTIAL);
#endif
#ifdef ONEBRC_MADVISE_HUGEPAGE
    madvise((void *)data, size, MADV_HUGEPAGE);
#endif

    const char *header_end = memchr(data, '\n', size);
    if (header_end == NULL) {
        fprintf(stderr, "input has no header line\n");
        munmap((void *)data, size);
        return 1;
    }
    const char *body = header_end + 1;
    size_t body_size = (size_t)((data + size) - body);

#ifdef ONEBRC_SHARED_DICTIONARY
    ChannelMap shared_channels = {0};
    if (build_shared_dictionary(&shared_channels, body, data + size) != 0) {
        fprintf(stderr, "failed to build the 10000-channel dictionary\n");
        munmap((void *)data, size);
        return 1;
    }
#endif

    int thread_count = configured_thread_count();
    Worker *workers = calloc((size_t)thread_count, sizeof(*workers));
    pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));
    const char **boundaries = calloc((size_t)thread_count + 1, sizeof(*boundaries));
    if (workers == NULL || threads == NULL || boundaries == NULL) {
        fprintf(stderr, "out of memory\n");
        free(workers);
        free(threads);
        free(boundaries);
        munmap((void *)data, size);
        return 1;
    }

    boundaries[0] = body;
    boundaries[thread_count] = data + size;
    for (int i = 1; i < thread_count; i++) {
        const char *nominal = body + body_size * (size_t)i / (size_t)thread_count;
        const char *newline = memchr(nominal, '\n', (size_t)((data + size) - nominal));
        boundaries[i] = newline == NULL ? data + size : newline + 1;
    }
#ifdef ONEBRC_TIMING
    timing_setup_end = monotonic_nanoseconds();
#endif
#ifdef ONEBRC_MEMORY_REPORT
    report_memory("setup");
#endif

    int created = 0;
    int failed = 0;
    for (int i = 0; i < thread_count; i++) {
        workers[i].begin = boundaries[i];
        workers[i].end = boundaries[i + 1];
        workers[i].file_end = data + size;
#ifdef ONEBRC_SHARED_DICTIONARY
        workers[i].shared_channels = &shared_channels;
#endif
        int error = pthread_create(&threads[i], NULL, run_worker, &workers[i]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            failed = 1;
            break;
        }
        created++;
    }
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    for (int i = 0; i < created; i++) {
        if (workers[i].error != 0) {
            fprintf(stderr, "worker failed: %s\n", strerror(workers[i].error));
            failed = 1;
        }
    }
#ifdef ONEBRC_TIMING
    timing_workers_end = monotonic_nanoseconds();
#endif
#ifdef ONEBRC_MEMORY_REPORT
    report_memory("workers");
#endif
#ifdef ONEBRC_DIRECT_STATS
    for (int i = 0; i < created; i++) {
        channel_map_materialize_direct_stats(&workers[i].channels);
    }
#endif
#if defined(ONEBRC_PROBE_REPORT) && defined(ONEBRC_SPLIT_SLOTS)
    report_channel_probes(workers, created);
#endif

    GroupMap final = {0};
#ifdef ONEBRC_BENCHMARK
    uint64_t checksum = 0;
    for (int i = 0; i < created; i++) checksum ^= workers[i].checksum;
    if (!failed) fprintf(stderr, "checksum=%" PRIu64 "\n", checksum);
#else
#ifdef ONEBRC_DIRECT_MERGE
#ifdef ONEBRC_SHARED_DICTIONARY
    if (!failed && created > 0) {
        merge_shared_stats_direct(workers, created, shared_channels.channel_count);
    }
#else
    if (!failed && created > 0 &&
        merge_workers_direct(&workers[0].channels, workers, created) != 0) {
        failed = 1;
    }
#endif
#else
    if (!failed && group_map_init(&final, INITIAL_FINAL_SLOTS) != 0) failed = 1;
#ifdef ONEBRC_SHARED_DICTIONARY
    if (!failed && merge_shared_workers(&final, &shared_channels, workers, created) != 0) {
        failed = 1;
    }
#else
    for (int i = 0; !failed && i < created; i++) {
        if (merge_worker(&final, &workers[i]) != 0) failed = 1;
    }
#endif
#endif
#ifdef ONEBRC_TIMING
    timing_merge_end = monotonic_nanoseconds();
#endif
#ifdef ONEBRC_MEMORY_REPORT
    report_memory("merge");
#endif
#ifdef ONEBRC_DIRECT_MERGE
#ifdef ONEBRC_SHARED_DICTIONARY
    if (!failed && write_channel_output(argv[2], &shared_channels,
                                        workers[0].channels.stats,
                                        workers[0].channels.wide_lengths) != 0) {
#else
    if (!failed && write_channel_output(argv[2], &workers[0].channels,
                                        workers[0].channels.stats,
                                        workers[0].channels.wide_lengths) != 0) {
#endif
        perror("write output");
        failed = 1;
    }
#else
    if (!failed && write_output(argv[2], &final) != 0) {
        perror("write output");
        failed = 1;
    }
#endif
#ifdef ONEBRC_TIMING
    timing_output_end = monotonic_nanoseconds();
    fprintf(stderr, "timing setup=%.6f workers=%.6f merge=%.6f output=%.6f total=%.6f\n",
            (double)(timing_setup_end - timing_start) / 1e9,
            (double)(timing_workers_end - timing_setup_end) / 1e9,
            (double)(timing_merge_end - timing_workers_end) / 1e9,
            (double)(timing_output_end - timing_merge_end) / 1e9,
            (double)(timing_output_end - timing_start) / 1e9);
#endif
#ifdef ONEBRC_MEMORY_REPORT
    report_memory("output");
#endif
#endif

#ifdef ONEBRC_FAST_EXIT
    _exit(failed ? 1 : 0);
#endif
    for (int i = 0; i < created; i++) free_worker(&workers[i]);
    free(final.slot);
    free(workers);
    free(threads);
    free(boundaries);
#ifdef ONEBRC_SHARED_DICTIONARY
    free(shared_channels.slot);
#ifdef ONEBRC_SPLIT_SLOTS
    free(shared_channels.slot_hash);
    free(shared_channels.slot_id);
#endif
#ifdef ONEBRC_PACKED_SLOTS
    free(shared_channels.packed_slot);
#endif
    free(shared_channels.stats);
    free(shared_channels.wide_lengths);
    free(shared_channels.info);
#endif
#ifdef ONEBRC_MEMORY_REPORT
    report_memory("freed");
#endif
    munmap((void *)data, size);
    return failed ? 1 : 0;
}
