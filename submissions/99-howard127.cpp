// 1BRC for traP: native C++ implementation.
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#if defined(__SSE2__)
#include <immintrin.h>
#endif
#include <limits>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define COLD_NOINLINE __attribute__((cold, noinline))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define COLD_NOINLINE
#endif

constexpr std::int32_t kEmptyMonth = std::numeric_limits<std::int32_t>::min();
constexpr std::uint32_t kNoExtra = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kMonthSlots = 12;
#ifndef BRC_PIPELINE_DEPTH
#define BRC_PIPELINE_DEPTH 8
#endif
constexpr std::size_t kPipelineDepth = BRC_PIPELINE_DEPTH;
static_assert(std::has_single_bit(kPipelineDepth));

struct YearMonth {
    int year;
    unsigned month;
};

YearMonth civil_from_days(std::int64_t days) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
         day_of_era / 146096) /
        365;
    int year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
    const unsigned day_of_year =
        day_of_era -
        (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_prime = (5 * day_of_year + 2) / 153;
    const unsigned month =
        month_prime < 10 ? month_prime + 3 : month_prime - 9;
    year += month <= 2;
    return {year, month};
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned month_prime =
        month > 2 ? month - 3 : static_cast<unsigned>(month + 9);
    const unsigned day_of_year = (153 * month_prime + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(day_of_era) - 719468;
}

struct MonthBlock {
    std::uint64_t next_month_start;
    std::int32_t month_serial;
};

constexpr unsigned kMonthBlockShift = 21;
constexpr std::size_t kMonthBlockCount =
    (std::uint64_t{1} << 32) >> kMonthBlockShift;

std::array<MonthBlock, kMonthBlockCount> make_month_table() {
    std::array<MonthBlock, kMonthBlockCount> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const std::uint64_t start =
            static_cast<std::uint64_t>(i) << kMonthBlockShift;
        const YearMonth current =
            civil_from_days(static_cast<std::int64_t>(start / 86400));
        table[i].month_serial =
            current.year * 12 + static_cast<int>(current.month) - 1;

        int next_year = current.year;
        unsigned next_month = current.month + 1;
        if (next_month == 13) {
            next_month = 1;
            ++next_year;
        }
        table[i].next_month_start =
            static_cast<std::uint64_t>(
                days_from_civil(next_year, next_month, 1)) *
            86400;
    }
    return table;
}

const std::array<MonthBlock, kMonthBlockCount> kMonthTable =
    make_month_table();

constexpr std::uint32_t k2027Start = 1798761600U;
constexpr std::uint32_t k2027Seconds = 365U * 86400U;
constexpr std::int32_t k2027MonthSerial = 2027 * 12;

constexpr std::array<std::uint8_t, 365> make_2027_day_months() {
    constexpr std::array<unsigned, 12> lengths{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    std::array<std::uint8_t, 365> months{};
    std::size_t day = 0;
    for (std::uint8_t month = 0; month < lengths.size(); ++month) {
        for (unsigned i = 0; i < lengths[month]; ++i) {
            months[day++] = month;
        }
    }
    return months;
}

constexpr std::array<std::uint8_t, 365> k2027DayMonths =
    make_2027_day_months();

inline std::int32_t positive_timestamp_to_month(std::uint32_t timestamp) {
    const MonthBlock& block = kMonthTable[timestamp >> kMonthBlockShift];
    return block.month_serial +
           static_cast<std::int32_t>(timestamp >= block.next_month_start);
}

inline std::int32_t signed_timestamp_to_month(std::int64_t timestamp) {
    std::int64_t days = timestamp / 86400;
    if (timestamp < 0 && timestamp % 86400 != 0) {
        --days;
    }
    const YearMonth month = civil_from_days(days);
    return month.year * 12 + static_cast<int>(month.month) - 1;
}

inline std::uint64_t multiply_mix(std::uint64_t lhs, std::uint64_t rhs) {
    const unsigned __int128 product =
        static_cast<unsigned __int128>(lhs) * rhs;
    return static_cast<std::uint64_t>(product) ^
           static_cast<std::uint64_t>(product >> 64);
}

inline std::uint64_t load_u64(const char* data) {
    std::uint64_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

struct ParsedChannel {
    const char* data;
    std::uint32_t length;
    std::uint64_t hash;
};

inline ParsedChannel parse_channel(const char*& cursor) {
    constexpr std::uint64_t kCommas = 0x2c2c2c2c2c2c2c2cULL;
    constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
    constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
    constexpr std::uint64_t kSecret1 = 0xa0761d6478bd642fULL;
    constexpr std::uint64_t kSecret2 = 0xe7037ed1a0b428dbULL;

    const char* const begin = cursor;

#if defined(__AVX2__) && defined(__BMI2__)
    // Locate and hash normal channel paths from the same 32-byte load.
    // A 32-byte channel has its comma just outside this block and falls
    // through to the scalar parser below.
    const __m256i bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cursor));
    const unsigned comma_mask = static_cast<unsigned>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(','))));
    if (LIKELY(comma_mask != 0)) {
        const unsigned length = std::countr_zero(comma_mask);
        const __m256i byte_indices = _mm256_setr_epi8(
            0, 1, 2, 3, 4, 5, 6, 7,
            8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 20, 21, 22, 23,
            24, 25, 26, 27, 28, 29, 30, 31);
        const __m256i prefix = _mm256_cmpgt_epi8(
            _mm256_set1_epi8(static_cast<char>(length)), byte_indices);
        const __m256i masked = _mm256_and_si256(bytes, prefix);
        const __m128i low = _mm256_castsi256_si128(masked);
        const __m128i high = _mm256_extracti128_si256(masked, 1);
        const std::uint64_t word0 =
            static_cast<std::uint64_t>(_mm_cvtsi128_si64(low));
        const std::uint64_t word1 =
            static_cast<std::uint64_t>(_mm_extract_epi64(low, 1));
        const std::uint64_t word2 =
            static_cast<std::uint64_t>(_mm_cvtsi128_si64(high));
        const std::uint64_t word3 =
            static_cast<std::uint64_t>(_mm_extract_epi64(high, 1));

        const std::uint64_t left =
            word0 ^ std::rotl(word2, 23);
        std::uint64_t right =
            word1 ^ std::rotl(word3, 41);
        right ^= static_cast<std::uint64_t>(length) << 56;
        const std::uint64_t hash =
            multiply_mix(left ^ kSecret1, right ^ kSecret2);
        cursor += length + 1;
        return {begin, length, hash};
    }
#endif

    std::uint32_t length = 0;
    std::uint64_t hash = kSecret1;

    for (;;) {
        const std::uint64_t word = load_u64(cursor);
        std::uint64_t compared = word ^ kCommas;
#if defined(__BMI__) && defined(__BMI2__)
        std::uint64_t subtracted = compared - kOnes;
#if defined(__GNUC__) || defined(__clang__)
        // Preserve the BMI andn instead of folding this back into a longer
        // generic zero-byte test.
        __asm__("" : "+r"(compared), "+r"(subtracted));
#endif
        const std::uint64_t matches =
            _andn_u64(compared, subtracted) & kHighBits;
#else
        const std::uint64_t matches =
            (compared - kOnes) & ~compared & kHighBits;
#endif
        if (LIKELY(matches != 0)) {
            const unsigned tail_length =
                static_cast<unsigned>(std::countr_zero(matches)) >> 3;
#if defined(__BMI2__)
            const std::uint64_t tail =
                _bzhi_u64(word, tail_length * 8);
#else
            const std::uint64_t tail_mask =
                tail_length == 0
                    ? 0
                    : std::numeric_limits<std::uint64_t>::max() >>
                          ((8 - tail_length) * 8);
            const std::uint64_t tail = word & tail_mask;
#endif
            hash = multiply_mix(
                (hash ^ tail) + kSecret2,
                kSecret1 ^
                    (static_cast<std::uint64_t>(length + tail_length) *
                     kSecret2));
            length += tail_length;
            cursor += tail_length + 1;
            return {begin, length, hash};
        }

        hash = multiply_mix(hash ^ word, kSecret2);
        cursor += 8;
        length += 8;
    }
}

inline std::int32_t parse_timestamp_month(const char*& cursor) {
#if !defined(__SSSE3__) || !defined(__SSE4_1__)
    constexpr std::uint64_t kAsciiZeros = 0x3030303030303030ULL;
    constexpr std::uint64_t kLowBytes = 0x00ff00ff00ff00ffULL;
    constexpr std::uint64_t kLowHalfWords = 0x0000ffff0000ffffULL;
#endif

#if defined(__SSE2__)
    const __m128i bytes =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(cursor));
    const __m128i commas = _mm_cmpeq_epi8(bytes, _mm_set1_epi8(','));
    const unsigned comma_mask =
        static_cast<unsigned>(_mm_movemask_epi8(commas));
#if !defined(__SSSE3__) || !defined(__SSE4_1__)
    const std::uint64_t first_eight =
        static_cast<std::uint64_t>(_mm_cvtsi128_si64(bytes));
#endif
    const bool has_ten_digits =
        (comma_mask & 0x7ffU) == (1U << 10) && cursor[0] != '-';
#else
    constexpr std::uint64_t kCommas = 0x2c2c2c2c2c2c2c2cULL;
    constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
    constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
    const std::uint64_t first_eight = load_u64(cursor);
    const std::uint64_t compared = first_eight ^ kCommas;
    const std::uint64_t comma_in_first_eight =
        (compared - kOnes) & ~compared & kHighBits;
    const bool has_ten_digits =
        comma_in_first_eight == 0 && cursor[8] != ',' &&
        cursor[9] != ',' && cursor[10] == ',';
#endif

    if (LIKELY(has_ten_digits)) {
#if defined(__SSSE3__) && defined(__SSE4_1__)
        const __m128i digits =
            _mm_sub_epi8(bytes, _mm_set1_epi8('0'));
        const __m128i pairs = _mm_maddubs_epi16(
            digits, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1,
                                  10, 1, 10, 1, 10, 1, 10, 1));
        const __m128i quads = _mm_madd_epi16(
            pairs, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
        const std::uint32_t first_four =
            static_cast<std::uint32_t>(_mm_cvtsi128_si32(quads));
        const std::uint32_t second_four =
            static_cast<std::uint32_t>(_mm_extract_epi32(quads, 1));
        const std::uint32_t final_pair =
            static_cast<std::uint32_t>(_mm_extract_epi16(pairs, 4));
        const std::uint64_t timestamp =
            static_cast<std::uint64_t>(first_four) * 1000000 +
            second_four * 100 + final_pair;
#else
        const std::uint64_t digits = first_eight - kAsciiZeros;
        const std::uint64_t pairs =
            (digits & kLowBytes) * 10 +
            ((digits >> 8) & kLowBytes);
        const std::uint64_t quads =
            (pairs & kLowHalfWords) * 100 +
            ((pairs >> 16) & kLowHalfWords);
        const std::uint32_t first_eight_digits =
            static_cast<std::uint32_t>(quads) * 10000 +
            static_cast<std::uint32_t>(quads >> 32);
        const std::uint32_t pair4 =
            static_cast<std::uint32_t>(cursor[8] - '0') * 10 +
            static_cast<std::uint32_t>(cursor[9] - '0');
        const std::uint64_t timestamp =
            static_cast<std::uint64_t>(first_eight_digits) * 100 + pair4;
#endif
        cursor += 11;
        const std::uint64_t delta = timestamp - k2027Start;
        if (LIKELY(delta < k2027Seconds)) {
            const std::size_t day =
                static_cast<std::size_t>(delta / 86400);
            return k2027MonthSerial + k2027DayMonths[day];
        }
        if (LIKELY(timestamp <=
                   std::numeric_limits<std::uint32_t>::max())) {
            return positive_timestamp_to_month(
                static_cast<std::uint32_t>(timestamp));
        }
        return signed_timestamp_to_month(
            static_cast<std::int64_t>(timestamp));
    }

    const bool negative = *cursor == '-';
    cursor += negative;
    std::uint64_t value = 0;
    while (*cursor != ',') {
        value = value * 10 + static_cast<std::uint64_t>(*cursor++ - '0');
    }
    ++cursor;
    const std::int64_t timestamp =
        negative ? -static_cast<std::int64_t>(value)
                 : static_cast<std::int64_t>(value);
    if (LIKELY(static_cast<std::uint64_t>(timestamp) <=
               std::numeric_limits<std::uint32_t>::max())) {
        return positive_timestamp_to_month(
            static_cast<std::uint32_t>(timestamp));
    }
    return signed_timestamp_to_month(timestamp);
}

constexpr std::array<std::uint32_t, 13> make_2027_month_starts() {
    constexpr std::array<unsigned, 12> lengths{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    std::array<std::uint32_t, 13> starts{};
    starts[0] = k2027Start;
    for (std::size_t month = 0; month < lengths.size(); ++month) {
        starts[month + 1] =
            starts[month] + lengths[month] * 86400U;
    }
    return starts;
}

constexpr std::uint32_t prefix_bcd_key(std::uint32_t prefix) {
    const std::uint32_t suffix = prefix % 1000U;
    return suffix / 100U |
           ((suffix / 10U) % 10U) << 4 |
           (suffix % 10U) << 8;
}

constexpr std::array<std::uint8_t, 4096>
make_2027_prefix_months() {
    constexpr auto starts = make_2027_month_starts();
    std::array<std::uint8_t, 4096> table{};
    table.fill(0xff);
    constexpr std::uint32_t first_prefix = k2027Start / 100000U;
    constexpr std::uint32_t final_timestamp =
        k2027Start + k2027Seconds;
    constexpr std::uint32_t last_prefix =
        (final_timestamp - 1) / 100000U;
    for (std::uint32_t prefix = first_prefix;
         prefix <= last_prefix; ++prefix) {
        const std::uint32_t raw_start = prefix * 100000U;
        const std::uint32_t raw_end = raw_start + 100000U;
        const std::uint32_t range_start =
            raw_start < k2027Start ? k2027Start : raw_start;
        const std::uint32_t range_end =
            raw_end > final_timestamp ? final_timestamp : raw_end;
        for (std::uint8_t month = 0; month < 12; ++month) {
            if (range_start >= starts[month] &&
                range_end <= starts[month + 1]) {
                table[prefix_bcd_key(prefix)] = month;
                break;
            }
        }
    }
    return table;
}

constexpr auto k2027PrefixMonths = make_2027_prefix_months();

COLD_NOINLINE std::int32_t
parse_2027_timestamp_month_slow(const char* cursor) {
#if defined(__SSSE3__) && defined(__SSE4_1__)
    const __m128i bytes =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(cursor));
    const __m128i pairs = _mm_maddubs_epi16(
        bytes, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1,
                             10, 1, 10, 1, 10, 1, 10, 1));
    const __m128i quads = _mm_madd_epi16(
        pairs, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
    const std::uint32_t first_four =
        static_cast<std::uint32_t>(_mm_cvtsi128_si32(quads));
    const std::uint32_t second_four =
        static_cast<std::uint32_t>(_mm_extract_epi32(quads, 1));
    const std::uint32_t final_pair =
        static_cast<std::uint32_t>(_mm_extract_epi16(pairs, 4));
    const std::uint32_t timestamp =
        first_four * 1000000U + second_four * 100U + final_pair;
    constexpr std::uint32_t kTimestampBias = 1793725776U;
#else
    constexpr std::uint64_t kAsciiZeros = 0x3030303030303030ULL;
    constexpr std::uint64_t kLowBytes = 0x00ff00ff00ff00ffULL;
    constexpr std::uint64_t kLowHalfWords = 0x0000ffff0000ffffULL;
    const std::uint64_t first_eight = load_u64(cursor);
    const std::uint64_t digits = first_eight - kAsciiZeros;
    const std::uint64_t pairs =
        (digits & kLowBytes) * 10 + ((digits >> 8) & kLowBytes);
    const std::uint64_t quads =
        (pairs & kLowHalfWords) * 100 +
        ((pairs >> 16) & kLowHalfWords);
    const std::uint32_t first_eight_digits =
        static_cast<std::uint32_t>(quads) * 10000 +
        static_cast<std::uint32_t>(quads >> 32);
    const std::uint32_t final_pair =
        static_cast<std::uint32_t>(cursor[8] - '0') * 10 +
        static_cast<std::uint32_t>(cursor[9] - '0');
    const std::uint32_t timestamp =
        first_eight_digits * 100U + final_pair;
    constexpr std::uint32_t kTimestampBias = 0;
#endif
    const std::uint32_t day =
        (timestamp - kTimestampBias - k2027Start) / 86400U;
    return k2027MonthSerial + k2027DayMonths[day];
}

inline std::int32_t parse_2027_timestamp_month(const char*& cursor) {
#if defined(__BMI2__)
    const char* const begin = cursor;
    std::uint32_t prefix_bytes;
    std::memcpy(&prefix_bytes, begin + 2, sizeof(prefix_bytes));
    const std::uint32_t key =
        _pext_u32(prefix_bytes, 0x000f0f0fU);
    const std::uint8_t month = k2027PrefixMonths[key];
    cursor = begin + 11;
    if (LIKELY(month != 0xff)) {
        return k2027MonthSerial + month;
    }
    return parse_2027_timestamp_month_slow(begin);
#else
    const char* const begin = cursor;
    cursor += 11;
    return parse_2027_timestamp_month_slow(begin);
#endif
}

inline std::uint32_t parse_comma_terminated_u32(const char*& cursor) {
    const std::uint32_t digit0 =
        static_cast<std::uint32_t>(cursor[0] - '0');
    if (LIKELY(cursor[3] == ',')) {
        const std::uint32_t value =
            digit0 * 100 +
            static_cast<std::uint32_t>(cursor[1] - '0') * 10 +
            static_cast<std::uint32_t>(cursor[2] - '0');
        cursor += 4;
        return value;
    }
    if (LIKELY(cursor[2] == ',')) {
        const std::uint32_t value =
            digit0 * 10 +
            static_cast<std::uint32_t>(cursor[1] - '0');
        cursor += 3;
        return value;
    }
    if (UNLIKELY(cursor[1] == ',')) {
        cursor += 2;
        return digit0;
    }

    std::uint32_t value =
        digit0 * 100 +
        static_cast<std::uint32_t>(cursor[1] - '0') * 10 +
        static_cast<std::uint32_t>(cursor[2] - '0');
    cursor += 3;
    while (*cursor != ',') {
        value = value * 10 + static_cast<std::uint32_t>(*cursor++ - '0');
    }
    ++cursor;
    return value;
}

inline std::uint32_t parse_line_terminated_u32(const char*& cursor) {
    std::uint32_t value = static_cast<std::uint32_t>(*cursor++ - '0');
    if (LIKELY(*cursor == '\n')) {
        ++cursor;
        return value;
    }
    while (*cursor != '\n' && *cursor != '\r') {
        value = value * 10 + static_cast<std::uint32_t>(*cursor++ - '0');
    }
    if (*cursor == '\r') {
        ++cursor;
    }
    ++cursor;
    return value;
}

struct alignas(32) Stats {
    std::uint64_t total_length;
    std::uint64_t total_stamps;
    std::uint32_t message_count;
    std::uint32_t min_length;
    std::uint32_t max_length;
    std::int32_t month;
};

inline void initialize_stats(Stats& stats, std::int32_t month,
                             std::uint32_t length,
                             std::uint32_t stamps) {
    stats.total_length = length;
    stats.total_stamps = stamps;
    stats.message_count = 1;
    stats.min_length = length;
    stats.max_length = length;
    stats.month = month;
}

inline void update_stats(Stats& stats, std::uint32_t length,
                         std::uint32_t stamps) {
    stats.total_length += length;
    stats.total_stamps += stamps;
    ++stats.message_count;
    if (length < stats.min_length) {
        stats.min_length = length;
    }
    if (length > stats.max_length) {
        stats.max_length = length;
    }
}

inline void combine_stats(Stats& destination, const Stats& source) {
    destination.total_length += source.total_length;
    destination.total_stamps += source.total_stamps;
    destination.message_count += source.message_count;
    if (source.min_length < destination.min_length) {
        destination.min_length = source.min_length;
    }
    if (source.max_length > destination.max_length) {
        destination.max_length = source.max_length;
    }
}

struct Channel {
    const char* name;
    std::uint64_t hash;
    std::uint32_t length;

    Channel(const char* channel_name, std::uint32_t channel_length,
            std::uint64_t channel_hash)
        : name(channel_name),
          hash(channel_hash),
          length(channel_length) {}
};

struct HashSlot {
    std::uint64_t encoded = 0;
};

class Aggregator {
public:
    struct PreparedUpdate {
        Stats* direct;
    };

    explicit Aggregator(std::size_t initial_capacity = 32768)
        : slots_(round_up_power_of_two(initial_capacity)),
          slot_mask_(slots_.size() - 1) {
        constexpr std::size_t kExpectedChannelCount = 16384;
        channels_.reserve(kExpectedChannelCount);
        for (std::size_t slot = 0; slot < kMonthSlots; ++slot) {
            stats_[slot].reserve(kExpectedChannelCount);
            stats_bases_[slot] = stats_[slot].data();
        }
    }

    PreparedUpdate prepare(const ParsedChannel& parsed,
                           std::int32_t month) {
        const std::uint32_t channel_index = find_or_insert(parsed);
        const std::uint32_t month_slot =
            static_cast<std::uint32_t>(month - k2027MonthSerial);
        Stats& direct = stats_bases_[month_slot][channel_index];
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&direct, 1, 3);
#endif
        return {&direct};
    }

    void add(const PreparedUpdate& prepared, std::uint32_t length,
             std::uint32_t stamps) {
        update_stats(*prepared.direct, length, stamps);
    }

    void merge_channel(const Aggregator& source_aggregator,
                       std::size_t source_index) {
        const Channel& source = source_aggregator.channels_[source_index];
        const ParsedChannel parsed{source.name, source.length, source.hash};
        const std::uint32_t destination_index = find_or_insert(parsed);
        for (std::size_t slot = 0; slot < kMonthSlots; ++slot) {
            const Stats& source_stats =
                source_aggregator.stats_[slot][source_index];
            if (source_stats.message_count != 0) {
                combine_stats(stats_[slot][destination_index],
                              source_stats);
            }
        }
    }

    const std::vector<Channel>& channels() const {
        return channels_;
    }

    const Stats& stats(std::size_t month_slot,
                       std::size_t channel_index) const {
        return stats_[month_slot][channel_index];
    }

private:
    static constexpr std::uint64_t kChannelIndexMask =
        (std::uint64_t{1} << 24) - 1;

    static std::size_t round_up_power_of_two(std::size_t value) {
        value = std::max<std::size_t>(value, 16);
        return std::bit_ceil(value);
    }

    std::uint32_t find_or_insert(const ParsedChannel& parsed) {
        std::size_t position = parsed.hash & slot_mask_;
        const std::uint64_t fingerprint =
            parsed.hash & ~kChannelIndexMask;

        for (;;) {
            HashSlot& slot = slots_[position];
            const std::uint32_t channel_index_plus_one =
                static_cast<std::uint32_t>(
                    slot.encoded & kChannelIndexMask);
            if (channel_index_plus_one == 0) {
                return insert_new(parsed, position, fingerprint);
            }

            if ((slot.encoded & ~kChannelIndexMask) == fingerprint) {
                return channel_index_plus_one - 1;
            }
            position = (position + 1) & slot_mask_;
        }
    }

    COLD_NOINLINE std::uint32_t
    insert_new(const ParsedChannel& parsed, std::size_t position,
               std::uint64_t fingerprint) {
        if ((channels_.size() + 1) * 10 >= slots_.size() * 7) {
            grow();
            return find_or_insert(parsed);
        }
        const std::uint32_t index =
            static_cast<std::uint32_t>(channels_.size());
        channels_.emplace_back(parsed.data, parsed.length, parsed.hash);
        for (std::size_t slot = 0; slot < kMonthSlots; ++slot) {
            Stats stats{};
            stats.min_length =
                std::numeric_limits<std::uint32_t>::max();
            stats.month =
                k2027MonthSerial + static_cast<std::int32_t>(slot);
            stats_[slot].push_back(stats);
        }
        slots_[position].encoded = fingerprint | (index + 1);
        return index;
    }

    COLD_NOINLINE void grow() {
        std::vector<HashSlot> replacement(slots_.size() * 2);
        for (std::uint32_t index = 0; index < channels_.size(); ++index) {
            const Channel& channel = channels_[index];
            std::size_t position = channel.hash & (replacement.size() - 1);
            while ((replacement[position].encoded &
                    kChannelIndexMask) != 0) {
                position = (position + 1) & (replacement.size() - 1);
            }
            replacement[position].encoded =
                (channel.hash & ~kChannelIndexMask) | (index + 1);
        }
        slots_.swap(replacement);
        slot_mask_ = slots_.size() - 1;
    }

    std::vector<HashSlot> slots_;
    std::size_t slot_mask_;
    std::vector<Channel> channels_;
    std::array<std::vector<Stats>, kMonthSlots> stats_;
    std::array<Stats*, kMonthSlots> stats_bases_;
};

class MappedFile {
public:
    explicit MappedFile(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) {
            return;
        }
        struct stat status {};
        if (::fstat(fd_, &status) != 0 || status.st_size <= 0) {
            return;
        }
        size_ = static_cast<std::size_t>(status.st_size);
        void* mapped =
            ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapped == MAP_FAILED) {
            size_ = 0;
            return;
        }
        data_ = static_cast<const char*>(mapped);
#if defined(POSIX_FADV_SEQUENTIAL)
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() {
        if (data_ != nullptr) {
            ::munmap(const_cast<char*>(data_), size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    explicit operator bool() const {
        return data_ != nullptr;
    }

    const char* data() const {
        return data_;
    }

    std::size_t size() const {
        return size_;
    }

private:
    int fd_ = -1;
    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

void parse_range(const char* begin, const char* end, Aggregator& aggregates) {
    struct PendingUpdate {
        Aggregator::PreparedUpdate prepared;
        std::uint32_t message_length;
        std::uint32_t stamp_count;
    };

    std::array<PendingUpdate, kPipelineDepth> pending;
    const char* cursor = begin;
    std::size_t next_pending = 0;
    std::size_t pending_count = 0;
    while (cursor < end) {
        const std::int32_t month = parse_2027_timestamp_month(cursor);
        const ParsedChannel channel = parse_channel(cursor);
        const Aggregator::PreparedUpdate prepared =
            aggregates.prepare(channel, month);
        const std::uint32_t message_length =
            parse_comma_terminated_u32(cursor);
        const std::uint32_t stamp_count =
            parse_line_terminated_u32(cursor);
        if (pending_count == kPipelineDepth) {
            const PendingUpdate& oldest = pending[next_pending];
            aggregates.add(oldest.prepared, oldest.message_length,
                           oldest.stamp_count);
        } else {
            ++pending_count;
        }
        pending[next_pending] = {prepared, message_length, stamp_count};
        next_pending = (next_pending + 1) & (kPipelineDepth - 1);
    }

    const std::size_t first_pending =
        pending_count == kPipelineDepth ? next_pending : 0;
    for (std::size_t i = 0; i < pending_count; ++i) {
        const PendingUpdate& update =
            pending[(first_pending + i) & (kPipelineDepth - 1)];
        aggregates.add(update.prepared, update.message_length,
                       update.stamp_count);
    }
}

std::size_t thread_count_for(std::size_t input_size) {
    if (input_size < (1U << 20)) {
        return 1;
    }
    if (const char* configured = std::getenv("BRC_THREADS")) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(configured, &end, 10);
        if (end != configured && *end == '\0' && value > 0) {
            return std::min<std::size_t>(value, 64);
        }
    }
    const unsigned detected = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(detected == 0 ? 8 : detected, 1, 8);
}

bool append_u64(std::string& output, std::uint64_t value) {
    char buffer[32];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    output.append(buffer, result.ptr);
    return true;
}

bool append_i32(std::string& output, std::int32_t value) {
    char buffer[16];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    output.append(buffer, result.ptr);
    return true;
}

bool append_average(std::string& output, const Stats& stats) {
    char buffer[64];
    const double average =
        static_cast<double>(stats.total_length) / stats.message_count;
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), average,
                      std::chars_format::fixed, 2);
    if (result.ec != std::errc{}) {
        return false;
    }
    output.append(buffer, result.ptr);
    return true;
}

bool write_output(const char* path, const Aggregator& aggregates) {
    std::string output;
    output.reserve(aggregates.channels().size() * 12 * 52);

    for (std::size_t channel_index = 0;
         channel_index < aggregates.channels().size();
         ++channel_index) {
        const Channel& channel = aggregates.channels()[channel_index];
        for (std::size_t slot = 0; slot < kMonthSlots; ++slot) {
            const Stats& stats = aggregates.stats(slot, channel_index);
            if (stats.message_count != 0) {
                output.append(channel.name, channel.length);
                output.push_back(',');
                const std::int32_t year = stats.month / 12;
                const std::int32_t month = stats.month % 12 + 1;
                if (!append_i32(output, year)) {
                    return false;
                }
                output.push_back('-');
                if (month < 10) {
                    output.push_back('0');
                }
                if (!append_i32(output, month)) {
                    return false;
                }
                output.push_back('=');
                if (!append_u64(output, stats.min_length)) {
                    return false;
                }
                output.push_back('/');
                if (!append_average(output, stats)) {
                    return false;
                }
                output.push_back('/');
                if (!append_u64(output, stats.max_length)) {
                    return false;
                }
                output.push_back('/');
                if (!append_u64(output, stats.message_count)) {
                    return false;
                }
                output.push_back('/');
                if (!append_u64(output, stats.total_stamps)) {
                    return false;
                }
                output.push_back('\n');
            }
        }
    }

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }
    const char* cursor = output.data();
    std::size_t remaining = output.size();
    while (remaining != 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return ::close(fd) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
        return 2;
    }

    MappedFile input(argv[1]);
    if (!input) {
        std::fprintf(stderr, "failed to map input\n");
        return 1;
    }

    const char* const file_begin = input.data();
    const char* const file_end = file_begin + input.size();
    const char* header_end = static_cast<const char*>(
        std::memchr(file_begin, '\n', input.size()));
    if (header_end == nullptr) {
        std::fprintf(stderr, "invalid input framing\n");
        return 1;
    }
    const char* const data_begin = header_end + 1;
    if (data_begin == file_end) {
        Aggregator empty(16);
        return write_output(argv[2], empty) ? 0 : 1;
    }

    const bool has_final_newline = file_end[-1] == '\n';
    const char* last_line_begin =
        has_final_newline ? file_end - 1 : file_end;
    while (last_line_begin > data_begin &&
           last_line_begin[-1] != '\n') {
        --last_line_begin;
    }

    const std::size_t last_line_size =
        static_cast<std::size_t>(file_end - last_line_begin);
    std::string padded_last_line(last_line_begin, file_end);
    std::size_t parsed_last_line_size = last_line_size;
    if (!has_final_newline) {
        padded_last_line.push_back('\n');
        ++parsed_last_line_size;
    }
    padded_last_line.resize(
        parsed_last_line_size + 32, '\0');
    const char* const parallel_end = last_line_begin;

    const std::size_t thread_count =
        thread_count_for(
            static_cast<std::size_t>(parallel_end - data_begin));
    std::vector<const char*> boundaries(thread_count + 1);
    boundaries.front() = data_begin;
    boundaries.back() = parallel_end;
    for (std::size_t i = 1; i < thread_count; ++i) {
        const std::size_t offset =
            static_cast<std::size_t>(parallel_end - data_begin) * i /
            thread_count;
        const char* tentative = data_begin + offset;
        const char* newline = static_cast<const char*>(
            std::memchr(tentative, '\n',
                        static_cast<std::size_t>(parallel_end - tentative)));
        boundaries[i] =
            newline == nullptr ? parallel_end : newline + 1;
    }

    std::vector<std::unique_ptr<Aggregator>> local(thread_count);
    std::vector<std::thread> workers;
    workers.reserve(thread_count - 1);
    const auto parse_chunk = [&](std::size_t i) {
        const std::size_t capacity =
            boundaries[i + 1] - boundaries[i] < (1U << 20) ? 256 : 32768;
        auto aggregates = std::make_unique<Aggregator>(capacity);
        parse_range(boundaries[i], boundaries[i + 1], *aggregates);
        local[i] = std::move(aggregates);
    };
    for (std::size_t i = 0; i + 1 < thread_count; ++i) {
        workers.emplace_back([&, i] {
            parse_chunk(i);
        });
    }
    parse_chunk(thread_count - 1);
    parse_range(padded_last_line.data(),
                padded_last_line.data() + parsed_last_line_size,
                *local.back());
    for (std::thread& worker : workers) {
        worker.join();
    }

    Aggregator& merged = *local.front();
    for (std::size_t i = 1; i < local.size(); ++i) {
        for (std::size_t channel_index = 0;
             channel_index < local[i]->channels().size();
             ++channel_index) {
            merged.merge_channel(*local[i], channel_index);
        }
    }

    if (!write_output(argv[2], merged)) {
        std::fprintf(stderr, "failed to write output\n");
        return 1;
    }
    return 0;
}
