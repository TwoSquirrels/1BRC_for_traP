#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__SSE4_2__) || defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {

constexpr std::string_view kHeader =
    "unix_timestamp,channel_path,message_length,stamp_count";
constexpr unsigned kContestThreads = 8;
constexpr std::size_t kMaxChannels = 10000;
constexpr std::size_t kMaxChannelLength = 20 * 5 + 4;
constexpr std::size_t kExactCacheSize = 1U << 12;
constexpr std::size_t kLongCacheSize = 1U << 12;
constexpr unsigned kChannelOffsetBits = 20;
constexpr std::uint32_t kChannelOffsetMask =
    (std::uint32_t{1} << kChannelOffsetBits) - 1;
constexpr std::size_t kChannelCapacity = 1U << 14;
constexpr std::uint32_t kChannelIdMask =
    static_cast<std::uint32_t>(kChannelCapacity - 1);
constexpr std::uint32_t kChannelTagMask = ~kChannelIdMask;
constexpr std::array<std::uint32_t, 11> kMonthStartPrefixes = {
    18014400, 18038592, 18065376, 18091296, 18118080, 18144000,
    18170784, 18197568, 18223488, 18250272, 18276192};
constexpr std::uint32_t kMonthHashMultiplier = 0x153713b9U;

static_assert(kMaxChannels * kMaxChannelLength <=
              std::size_t{1} << kChannelOffsetBits);
static_assert(kMaxChannelLength <=
              (std::numeric_limits<std::uint32_t>::max()
               >> kChannelOffsetBits));
static_assert(kMaxChannels <= kChannelIdMask);

constexpr std::uint32_t suffix_as_big_endian_ascii(
    std::uint32_t value) noexcept {
    return (static_cast<std::uint32_t>('0' + value / 1000) << 24) |
           (static_cast<std::uint32_t>('0' + value / 100 % 10) << 16) |
           (static_cast<std::uint32_t>('0' + value / 10 % 10) << 8) |
           static_cast<std::uint32_t>('0' + value % 10);
}

constexpr std::uint32_t prefix_as_little_endian_ascii(
    std::uint32_t value) noexcept {
    return static_cast<std::uint32_t>('0' + value / 1000) |
           (static_cast<std::uint32_t>('0' + value / 100 % 10) << 8) |
           (static_cast<std::uint32_t>('0' + value / 10 % 10) << 16) |
           (static_cast<std::uint32_t>('0' + value % 10) << 24);
}

constexpr std::array<std::uint64_t, 64> make_month_buckets() {
    std::array<std::uint64_t, 64> result {};
    for (std::uint32_t offset = 0; offset < 33; ++offset) {
        const std::uint32_t four_digit_prefix = 1798 + offset;
        std::uint32_t month = 0;
        std::uint32_t cutoff = std::numeric_limits<std::uint32_t>::max();
        for (const std::uint32_t month_start : kMonthStartPrefixes) {
            const std::uint32_t start_prefix = month_start / 10000;
            if (start_prefix < four_digit_prefix) {
                ++month;
            } else if (start_prefix == four_digit_prefix) {
                cutoff = suffix_as_big_endian_ascii(month_start % 10000);
                break;
            } else {
                break;
            }
        }
        const std::uint32_t key =
            prefix_as_little_endian_ascii(four_digit_prefix);
        const std::uint32_t index =
            static_cast<std::uint32_t>(key * kMonthHashMultiplier) >> 26;
        result[index] = (static_cast<std::uint64_t>(month) << 32) | cutoff;
    }
    return result;
}

constexpr auto kMonthBuckets = make_month_buckets();

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string system_error(const char* action) {
    return std::string(action) + ": " + std::strerror(errno);
}

class MappedFile {
public:
    explicit MappedFile(const char* path) {
        const int fd = ::open(path, O_RDONLY);
        if (fd < 0) {
            fail(system_error("input open failed"));
        }

        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            const std::string error = system_error("input fstat failed");
            ::close(fd);
            fail(error);
        }
        if (st.st_size < 0 ||
            static_cast<std::uintmax_t>(st.st_size) >
                std::numeric_limits<std::size_t>::max()) {
            ::close(fd);
            fail("input is too large for this process");
        }

        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) {
            ::close(fd);
            fail("input is empty");
        }

        void* mapping =
            ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        const int mmap_errno = errno;
        ::close(fd);
        if (mapping == MAP_FAILED) {
            errno = mmap_errno;
            fail(system_error("input mmap failed"));
        }
        data_ = static_cast<const char*>(mapping);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() {
        if (data_ != nullptr) {
            ::munmap(const_cast<char*>(data_), size_);
        }
    }

    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

struct Stats {
    std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum = 0;
    std::uint32_t sum = 0;
    std::uint32_t count = 0;
    std::uint32_t stamp_sum = 0;

    void add(std::uint32_t length, std::uint32_t stamps) noexcept {
        minimum = std::min(minimum, length);
        maximum = std::max(maximum, length);
        sum += length;
        ++count;
        stamp_sum += stamps;
    }

    void merge(const Stats& other) noexcept {
        minimum = std::min(minimum, other.minimum);
        maximum = std::max(maximum, other.maximum);
        sum += other.sum;
        count += other.count;
        stamp_sum += other.stamp_sum;
    }
};

static_assert(sizeof(Stats) == 20);

constexpr std::uint16_t kCompactLengthSentinel =
    std::numeric_limits<std::uint16_t>::max();

struct alignas(16) CompactStats {
    std::uint16_t minimum = kCompactLengthSentinel;
    std::uint16_t maximum = 0;
    std::uint32_t sum = 0;
    std::uint32_t count = 0;
    std::uint32_t stamp_sum = 0;
};

static_assert(sizeof(CompactStats) == 16);

struct WideMinMax {
    std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum = 0;
};

using WideMonthTable = std::array<WideMinMax, 12>;

class MonthTable {
public:
    void prefetch(std::uint8_t month) noexcept {
        __builtin_prefetch(&stats_[month], 1, 1);
    }

    CompactStats& operator[](std::uint8_t month) noexcept {
        return stats_[month];
    }

    const CompactStats& operator[](std::uint8_t month) const noexcept {
        return stats_[month];
    }

    template <class Function>
    void for_each(Function&& function) const {
        for (std::uint8_t month = 0; month < 12; ++month) {
            if (stats_[month].count != 0) {
                function(month, stats_[month]);
            }
        }
    }

    std::size_t size() const noexcept {
        std::size_t result = 0;
        for (const CompactStats& stats : stats_) {
            result += stats.count != 0;
        }
        return result;
    }

private:
    std::array<CompactStats, 12> stats_ {};
};

static_assert(sizeof(MonthTable) == 192);

#if !defined(__SSE4_2__)
std::uint64_t avalanche(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}
#endif

struct ChannelScan {
    const char* end;
    std::uint64_t first;
    std::uint64_t second;
};

constexpr std::uint64_t lower_bytes(std::uint64_t word,
                                    unsigned count) noexcept {
    return count == 0
        ? 0
        : count == 8
            ? word
            : word & ((std::uint64_t{1} << (count * 8)) - 1);
}

ChannelScan finish_channel_scan(const char* begin, const char* end,
                                std::uint64_t first,
                                std::uint64_t second) noexcept {
    const unsigned length = static_cast<unsigned>(end - begin);
    if (length < 8) {
        first = lower_bytes(first, length);
        second = 0;
    } else if (length < 16) {
        second = lower_bytes(second, length - 8);
    }
    return ChannelScan{end, first, second};
}

ChannelScan scan_channel(const char* begin, const char* range_end) noexcept {
    static_assert(std::endian::native == std::endian::little);
    constexpr std::uint64_t kCommas = 0x2c2c2c2c2c2c2c2cULL;
    constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
    constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
    std::uint64_t first = 0;
    std::uint64_t second = 0;

#if defined(__AVX2__)
    if (range_end - begin >= 128) {
        const __m256i commas = _mm256_set1_epi8(',');
        const __m256i block0 =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(begin));
        const __m128i low = _mm256_castsi256_si128(block0);
        first = static_cast<std::uint64_t>(_mm_cvtsi128_si64(low));
        second = static_cast<std::uint64_t>(_mm_extract_epi64(low, 1));

        std::uint32_t matches = static_cast<std::uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(block0, commas)));
        if (matches != 0) {
            return finish_channel_scan(
                begin, begin + std::countr_zero(matches), first, second);
        }

        const __m256i block1 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(begin + 32));
        matches = static_cast<std::uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(block1, commas)));
        if (matches != 0) {
            return finish_channel_scan(
                begin, begin + 32 + std::countr_zero(matches),
                first, second);
        }

        const __m256i block2 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(begin + 64));
        matches = static_cast<std::uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(block2, commas)));
        if (matches != 0) {
            return finish_channel_scan(
                begin, begin + 64 + std::countr_zero(matches),
                first, second);
        }

        const __m256i block3 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(begin + 96));
        matches = static_cast<std::uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(block3, commas)));
        return finish_channel_scan(
            begin, begin + 96 + std::countr_zero(matches), first, second);
    }
#endif

    const bool unchecked = range_end - begin >= 112;
    const char* cursor = begin;
    while (unchecked || range_end - cursor >=
                            static_cast<std::ptrdiff_t>(sizeof(std::uint64_t))) {
        std::uint64_t word;
        std::memcpy(&word, cursor, sizeof(word));
        if (cursor == begin) {
            first = word;
        } else if (cursor == begin + 8) {
            second = word;
        }
        const std::uint64_t difference = word ^ kCommas;
        const std::uint64_t matches =
            (difference - kOnes) & ~difference & kHighBits;
        if (matches != 0) {
            const unsigned remaining =
                static_cast<unsigned>(std::countr_zero(matches)) / 8;
            return finish_channel_scan(
                begin, cursor + remaining, first, second);
        }
        cursor += sizeof(word);
    }

    while (*cursor != ',') {
        const unsigned offset = static_cast<unsigned>(cursor - begin);
        if (offset < 8) {
            first |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(*cursor))
                     << (offset * 8);
        } else if (offset < 16) {
            second |= static_cast<std::uint64_t>(
                          static_cast<unsigned char>(*cursor))
                      << ((offset - 8) * 8);
        }
        ++cursor;
    }
    return finish_channel_scan(begin, cursor, first, second);
}

bool channel_equal(const char* left, const char* right,
                   std::size_t length) noexcept {
    while (length > 2 * sizeof(std::uint64_t)) {
        std::uint64_t left_word;
        std::uint64_t right_word;
        std::memcpy(&left_word, left, sizeof(left_word));
        std::memcpy(&right_word, right, sizeof(right_word));
        if (left_word != right_word) {
            return false;
        }
        left += sizeof(std::uint64_t);
        right += sizeof(std::uint64_t);
        length -= sizeof(std::uint64_t);
    }
    if (length >= sizeof(std::uint64_t)) {
        std::uint64_t left_first;
        std::uint64_t right_first;
        std::memcpy(&left_first, left, sizeof(left_first));
        std::memcpy(&right_first, right, sizeof(right_first));
        if (left_first != right_first) {
            return false;
        }
        if (length == sizeof(std::uint64_t)) {
            return true;
        }
        std::uint64_t left_last;
        std::uint64_t right_last;
        std::memcpy(&left_last, left + length - sizeof(left_last),
                    sizeof(left_last));
        std::memcpy(&right_last, right + length - sizeof(right_last),
                    sizeof(right_last));
        return left_last == right_last;
    }
    if (length >= sizeof(std::uint32_t)) {
        std::uint32_t left_word;
        std::uint32_t right_word;
        std::memcpy(&left_word, left, sizeof(left_word));
        std::memcpy(&right_word, right, sizeof(right_word));
        if (left_word != right_word) {
            return false;
        }
        left += sizeof(std::uint32_t);
        right += sizeof(std::uint32_t);
        length -= sizeof(std::uint32_t);
    }
    if (length >= sizeof(std::uint16_t)) {
        std::uint16_t left_word;
        std::uint16_t right_word;
        std::memcpy(&left_word, left, sizeof(left_word));
        std::memcpy(&right_word, right, sizeof(right_word));
        if (left_word != right_word) {
            return false;
        }
        left += sizeof(std::uint16_t);
        right += sizeof(std::uint16_t);
        length -= sizeof(std::uint16_t);
    }
    return length == 0 || *left == *right;
}

std::uint32_t exact_channel_hash(std::uint64_t first,
                                 std::uint64_t second) noexcept {
#if defined(__SSE4_2__)
    const std::uint32_t first_hash = static_cast<std::uint32_t>(
        _mm_crc32_u64(0x9e3779b9U, first));
    const std::uint32_t second_hash = static_cast<std::uint32_t>(
        _mm_crc32_u64(0x85ebca6bU, second));
    return first_hash ^ std::rotl(second_hash, 16);
#else
    return static_cast<std::uint32_t>(
        avalanche(first ^ std::rotl(second, 23)));
#endif
}

std::uint32_t quick_channel_hash(std::uint64_t first,
                                 std::uint64_t second,
                                 std::uint64_t last,
                                 std::size_t length) noexcept {
#if defined(__SSE4_2__)
    const std::uint32_t prefix_hash = static_cast<std::uint32_t>(
        _mm_crc32_u64(static_cast<std::uint32_t>(length),
                      first ^ std::rotl(second, 17)));
    const std::uint32_t suffix_hash = static_cast<std::uint32_t>(
        _mm_crc32_u64(0x27d4eb2fU, last));
    return prefix_hash ^ std::rotl(suffix_hash, 16);
#else
    return static_cast<std::uint32_t>(avalanche(
        first ^ std::rotl(second, 17) ^ std::rotl(last, 41) ^
        (static_cast<std::uint64_t>(length) << 56)));
#endif
}

std::uint32_t full_channel_hash(const char* key,
                                std::size_t length) noexcept {
#if defined(__SSE4_2__)
    std::uint32_t hash = 0x9e3779b9U;
    while (length >= sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, key, sizeof(word));
        hash = static_cast<std::uint32_t>(_mm_crc32_u64(hash, word));
        key += sizeof(word);
        length -= sizeof(word);
    }
    if (length != 0) {
        std::uint64_t tail = 0;
        std::memcpy(&tail, key, length);
        hash = static_cast<std::uint32_t>(_mm_crc32_u64(hash, tail));
    }
    return hash;
#else
    std::uint64_t hash = 0xa0761d6478bd642fULL;
    while (length >= sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, key, sizeof(word));
        hash ^= avalanche(word + 0xe7037ed1a0b428dbULL);
        hash = std::rotl(hash, 27) * 0x9e3779b185ebca87ULL;
        key += sizeof(word);
        length -= sizeof(word);
    }
    while (length != 0) {
        hash = (hash ^ static_cast<unsigned char>(*key++)) *
               0x100000001b3ULL;
        --length;
    }
    return static_cast<std::uint32_t>(avalanche(hash));
#endif
}

class ThreadResult {
public:
    ThreadResult() : channel_slots_(kChannelCapacity) {
        channel_meta_.reserve(kMaxChannels);
        channel_hashes_.reserve(kMaxChannels);
        aggregates_.reserve(kMaxChannels);
        channel_keys_.reserve(kMaxChannels * 16);
    }

    ThreadResult(const ThreadResult&) = delete;
    ThreadResult& operator=(const ThreadResult&) = delete;
    ThreadResult(ThreadResult&&) noexcept = default;
    ThreadResult& operator=(ThreadResult&&) noexcept = default;

    std::uint32_t resolve(const char* channel, std::size_t channel_length,
                          std::uint64_t first, std::uint64_t second,
                          std::uint8_t month) {
        std::uint32_t id;
        if (channel_length <= 16) {
            const std::uint32_t hash = exact_channel_hash(first, second);
            const std::size_t cache_index = hash & (kExactCacheSize - 1);
            const std::uint16_t encoded_id = exact_ids_[cache_index];
            if (encoded_id != 0 &&
                exact_first_[cache_index] == first &&
                exact_second_[cache_index] == second) {
                id = encoded_id - 1;
                aggregates_[id].prefetch(month);
                return id;
            }
            id = intern(channel, channel_length, hash);
            exact_first_[cache_index] = first;
            exact_second_[cache_index] = second;
            exact_ids_[cache_index] = static_cast<std::uint16_t>(id + 1);
        } else {
            std::uint64_t last;
            std::memcpy(&last, channel + channel_length - sizeof(last),
                        sizeof(last));
            const std::uint32_t quick_hash = quick_channel_hash(
                first, second, last, channel_length);
            const std::size_t cache_index =
                quick_hash & (kLongCacheSize - 1);
            const std::uint64_t entry = long_cache_[cache_index];
            const std::uint16_t encoded_id =
                static_cast<std::uint16_t>(entry);
            if (encoded_id != 0 && entry >> 16 == quick_hash) {
                const std::uint32_t candidate_id = encoded_id - 1;
                const std::uint32_t meta = channel_meta_[candidate_id];
                if ((meta >> kChannelOffsetBits) == channel_length) {
                    aggregates_[candidate_id].prefetch(month);
                    if (channel_equal(
                            channel_keys_.data() +
                                (meta & kChannelOffsetMask),
                            channel, channel_length)) {
                        return candidate_id;
                    }
                }
            }
            const std::uint32_t hash =
                full_channel_hash(channel, channel_length);
            id = intern(channel, channel_length, hash);
            long_cache_[cache_index] =
                (static_cast<std::uint64_t>(quick_hash) << 16) | (id + 1);
        }
        aggregates_[id].prefetch(month);
        return id;
    }

    void add(std::uint32_t id, std::uint8_t month,
             std::uint32_t message_length,
             std::uint32_t stamps) {
        CompactStats& stats = aggregates_[id][month];
        if (__builtin_expect(message_length < kCompactLengthSentinel, 1)) {
#if defined(__SSE4_2__)
            const __m128i current = _mm_load_si128(
                reinterpret_cast<const __m128i*>(&stats));
            const __m128i delta = _mm_set_epi32(
                std::bit_cast<std::int32_t>(stamps), 1,
                static_cast<std::int32_t>(message_length), 0);
            __m128i updated = _mm_add_epi32(current, delta);
            const __m128i length = _mm_cvtsi32_si128(
                static_cast<std::int32_t>(message_length));
            const __m128i minimum_mask = _mm_or_si128(
                length, _mm_set_epi16(-1, -1, -1, -1, -1, -1, -1, 0));
            const __m128i maximum_mask = _mm_slli_epi32(length, 16);
            updated = _mm_min_epu16(updated, minimum_mask);
            updated = _mm_max_epu16(updated, maximum_mask);
            _mm_store_si128(reinterpret_cast<__m128i*>(&stats), updated);
#else
            const auto compact_length =
                static_cast<std::uint16_t>(message_length);
            stats.minimum = std::min(stats.minimum, compact_length);
            stats.maximum = std::max(stats.maximum, compact_length);
            stats.sum += message_length;
            ++stats.count;
            stats.stamp_sum += stamps;
#endif
        } else {
            stats.sum += message_length;
            ++stats.count;
            stats.stamp_sum += stamps;
            add_wide(id, month, message_length);
        }
    }

    void merge_from(const ThreadResult& other) {
        for (std::uint32_t old_id = 0;
             old_id < other.channel_meta_.size(); ++old_id) {
            const std::uint32_t meta = other.channel_meta_[old_id];
            const std::uint32_t length = meta >> kChannelOffsetBits;
            const std::uint32_t key_offset = meta & kChannelOffsetMask;
            const char* const key =
                other.channel_keys_.data() + key_offset;
            const std::uint32_t id =
                intern(key, length, other.channel_hashes_[old_id]);
            for (std::uint8_t month = 0; month < 12; ++month) {
                if (other.aggregates_[old_id][month].count != 0) {
                    merge_stats(id, month,
                                other.decode_stats(old_id, month));
                }
            }
        }
    }

    template <class Function>
    void for_each_group(Function&& function) const {
        for (std::uint32_t id = 0; id < channel_meta_.size(); ++id) {
            const std::uint32_t meta = channel_meta_[id];
            const std::uint32_t length = meta >> kChannelOffsetBits;
            const std::uint32_t key_offset = meta & kChannelOffsetMask;
            const char* const key =
                channel_keys_.data() + key_offset;
            for (std::uint8_t month = 0; month < 12; ++month) {
                if (aggregates_[id][month].count != 0) {
                    const Stats stats = decode_stats(id, month);
                    function(key, length, month, stats);
                }
            }
        }
    }

    std::size_t group_count() const noexcept {
        std::size_t count = 0;
        for (const MonthTable& table : aggregates_) {
            count += table.size();
        }
        return count;
    }

private:
    void ensure_wide() {
        if (!wide_min_max_) {
            wide_min_max_ =
                std::make_unique<WideMonthTable[]>(kMaxChannels);
        }
    }

    [[gnu::cold, gnu::noinline]]
    void add_wide(std::uint32_t id, std::uint8_t month,
                  std::uint32_t length) {
        ensure_wide();
        CompactStats& compact = aggregates_[id][month];
        compact.maximum = kCompactLengthSentinel;
        WideMinMax& wide = wide_min_max_[id][month];
        wide.minimum = std::min(wide.minimum, length);
        wide.maximum = std::max(wide.maximum, length);
    }

    Stats decode_stats(std::uint32_t id, std::uint8_t month) const noexcept {
        const CompactStats& compact = aggregates_[id][month];
        Stats result;
        result.minimum = compact.minimum;
        result.maximum = compact.maximum;
        result.sum = compact.sum;
        result.count = compact.count;
        result.stamp_sum = compact.stamp_sum;
        if (compact.minimum == kCompactLengthSentinel) {
            result.minimum = wide_min_max_[id][month].minimum;
        }
        if (compact.maximum == kCompactLengthSentinel) {
            result.maximum = wide_min_max_[id][month].maximum;
        }
        return result;
    }

    void merge_stats(std::uint32_t id, std::uint8_t month,
                     const Stats& source) {
        CompactStats& destination = aggregates_[id][month];
        destination.sum += source.sum;
        destination.count += source.count;
        destination.stamp_sum += source.stamp_sum;

        if (source.minimum < kCompactLengthSentinel) {
            destination.minimum = std::min(
                destination.minimum,
                static_cast<std::uint16_t>(source.minimum));
        }
        if (source.maximum < kCompactLengthSentinel) {
            if (destination.maximum != kCompactLengthSentinel) {
                destination.maximum = std::max(
                    destination.maximum,
                    static_cast<std::uint16_t>(source.maximum));
            }
        } else {
            destination.maximum = kCompactLengthSentinel;
        }

        if (source.minimum >= kCompactLengthSentinel ||
            source.maximum >= kCompactLengthSentinel) {
            ensure_wide();
            WideMinMax& wide = wide_min_max_[id][month];
            if (source.minimum >= kCompactLengthSentinel) {
                wide.minimum = std::min(wide.minimum, source.minimum);
            }
            if (source.maximum >= kCompactLengthSentinel) {
                wide.maximum = std::max(wide.maximum, source.maximum);
            }
        }
    }

    std::uint32_t intern(const char* key, std::size_t length,
                         std::uint32_t hash) {
        if (length > kMaxChannelLength) {
            fail("channel path is too long");
        }

        std::size_t index = hash & kChannelIdMask;
        for (;;) {
            std::uint32_t& slot = channel_slots_[index];
            const std::uint32_t encoded_id = slot & kChannelIdMask;
            if (encoded_id == 0) {
                if (channel_meta_.size() == kMaxChannels) {
                    fail("too many channel paths");
                }
                if (channel_keys_.size() > kChannelOffsetMask) {
                    fail("total channel path size is too large");
                }
                const auto key_offset =
                    static_cast<std::uint32_t>(channel_keys_.size());
                channel_keys_.insert(channel_keys_.end(), key, key + length);
                const auto id =
                    static_cast<std::uint32_t>(channel_meta_.size());
                channel_meta_.push_back(
                    (static_cast<std::uint32_t>(length)
                         << kChannelOffsetBits) |
                    key_offset);
                channel_hashes_.push_back(hash);
                aggregates_.emplace_back();
                slot = (hash & kChannelTagMask) | (id + 1);
                return id;
            }
            if (((slot ^ hash) & kChannelTagMask) == 0) {
                const std::uint32_t id = encoded_id - 1;
                const std::uint32_t meta = channel_meta_[id];
                if ((meta >> kChannelOffsetBits) == length &&
                    channel_equal(
                        channel_keys_.data() +
                            (meta & kChannelOffsetMask),
                        key,
                        length)) {
                    return id;
                }
            }
            index = (index + 1) & kChannelIdMask;
        }
    }

    std::vector<std::uint32_t> channel_slots_;
    std::vector<std::uint32_t> channel_meta_;
    std::vector<std::uint32_t> channel_hashes_;
    std::vector<MonthTable> aggregates_;
    std::vector<char> channel_keys_;
    std::array<std::uint64_t, kExactCacheSize> exact_first_ {};
    std::array<std::uint64_t, kExactCacheSize> exact_second_ {};
    std::array<std::uint16_t, kExactCacheSize> exact_ids_ {};
    std::array<std::uint64_t, kLongCacheSize> long_cache_ {};
    std::unique_ptr<WideMonthTable[]> wide_min_max_;
};

std::uint8_t parse_month(const char*& cursor) noexcept {
    static_assert(std::endian::native == std::endian::little);
    std::uint64_t first_eight;
    std::memcpy(&first_eight, cursor, sizeof(first_eight));
    const std::uint32_t bucket_index = static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(first_eight) *
        kMonthHashMultiplier) >> 26;
    const std::uint64_t bucket = kMonthBuckets[bucket_index];
    const std::uint32_t suffix =
        __builtin_bswap32(static_cast<std::uint32_t>(first_eight >> 32));
    cursor += 11;
    return static_cast<std::uint8_t>(bucket >> 32) +
           static_cast<std::uint8_t>(
               suffix >= static_cast<std::uint32_t>(bucket));
}

template <char Delimiter>
std::uint32_t parse_unsigned(const char*& cursor) noexcept {
    std::uint32_t value = 0;
    for (;;) {
        const unsigned character =
            static_cast<unsigned char>(*cursor++);
        if (character == static_cast<unsigned char>(Delimiter)) {
            return value;
        }
        value = value * 10 + character - '0';
    }
}

struct ParsedNumbers {
    std::uint32_t message_length;
    std::uint32_t stamps;
};

ParsedNumbers parse_numbers(const char*& cursor) noexcept {
    const char* const begin = cursor;
    std::uint32_t word;
    std::memcpy(&word, begin, sizeof(word));
    const auto two_digits = static_cast<std::uint32_t>(
        ((word >> 16) & 0xff) == ',');
    const auto three_digits = static_cast<std::uint32_t>(
        ((word >> 24) & 0xff) == ',');
    if (__builtin_expect((two_digits | three_digits) != 0, 1)) {
        const std::uint32_t first = word & 15;
        const std::uint32_t second = (word >> 8) & 15;
        const std::uint32_t third = (word >> 16) & 15;
        const std::uint32_t value2 = first * 10 + second;
        const std::uint32_t value3 = value2 * 10 + third;
        const std::uint32_t selection = 0U - three_digits;
        const std::uint32_t message_length =
            value2 ^ ((value2 ^ value3) & selection);
        const unsigned message_bytes = 3 + three_digits;
        const auto* stamps = reinterpret_cast<const unsigned char*>(
            begin + message_bytes);
        if (__builtin_expect(stamps[1] == '\n', 1)) {
            cursor = reinterpret_cast<const char*>(stamps + 2);
            return ParsedNumbers{
                message_length,
                static_cast<std::uint32_t>(stamps[0] - '0')};
        }
        if (__builtin_expect(stamps[2] == '\n', 1)) {
            cursor = reinterpret_cast<const char*>(stamps + 3);
            return ParsedNumbers{
                message_length,
                static_cast<std::uint32_t>(
                    (stamps[0] - '0') * 10 + stamps[1] - '0')};
        }
    }
    const std::uint32_t message_length = parse_unsigned<','>(cursor);
    return ParsedNumbers{message_length, parse_unsigned<'\n'>(cursor)};
}

std::unique_ptr<ThreadResult> parse_range(const char* begin,
                                           const char* end) {
    auto result = std::make_unique<ThreadResult>();
    const char* cursor = begin;

    while (cursor != end) {
        const std::uint8_t month = parse_month(cursor);

        const char* const channel = cursor;
        const ChannelScan scan = scan_channel(channel, end);
        cursor = scan.end;
        const std::size_t channel_length =
            static_cast<std::size_t>(cursor - channel);
        ++cursor;

        const std::uint32_t channel_id = result->resolve(
            channel, channel_length, scan.first, scan.second, month);
        const ParsedNumbers numbers = parse_numbers(cursor);
        result->add(channel_id, month,
                    numbers.message_length, numbers.stamps);
    }
    return result;
}

void append_unsigned(std::string& output, std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    output.append(buffer, result.ptr);
}

void append_month(std::string& output, std::uint8_t month_index) {
    const unsigned month = static_cast<unsigned>(month_index) + 1;
    output.append("2027-");
    output.push_back(static_cast<char>('0' + month / 10));
    output.push_back(static_cast<char>('0' + month % 10));
}

std::string build_output(const ThreadResult& result) {
    std::string output;
    output.reserve(result.group_count() * 64);

    result.for_each_group(
        [&](const char* channel, std::size_t channel_length,
            std::uint8_t month, const Stats& stats) {
            output.append(channel, channel_length);
            output.push_back(',');
            append_month(output, month);
            output.push_back('=');
            append_unsigned(output, stats.minimum);
            output.push_back('/');

            char average[32];
            const auto average_result = std::to_chars(
                average, average + sizeof(average),
                static_cast<double>(stats.sum) /
                    static_cast<double>(stats.count),
                std::chars_format::fixed, 2);
            if (average_result.ec != std::errc{}) {
                fail("average formatting failed");
            }
            output.append(average, average_result.ptr);

            output.push_back('/');
            append_unsigned(output, stats.maximum);
            output.push_back('/');
            append_unsigned(output, stats.count);
            output.push_back('/');
            append_unsigned(output, stats.stamp_sum);
            output.push_back('\n');
        });
    return output;
}

void write_output(const char* path, std::string_view output) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail(system_error("output open failed"));
    }

    const char* cursor = output.data();
    std::size_t remaining = output.size();
    while (remaining != 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string error = system_error("output write failed");
            ::close(fd);
            fail(error);
        }
        if (written == 0) {
            ::close(fd);
            fail("output write made no progress");
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0) {
        fail(system_error("output close failed"));
    }
}

std::size_t find_data_start(const char* data, std::size_t size) {
    const void* newline = std::memchr(data, '\n', size);
    if (newline == nullptr) {
        fail("input header has no newline");
    }
    const char* header_end = static_cast<const char*>(newline);
    std::size_t header_length =
        static_cast<std::size_t>(header_end - data);
    if (header_length != 0 && data[header_length - 1] == '\r') {
        --header_length;
    }
    if (header_length != kHeader.size() ||
        std::memcmp(data, kHeader.data(), kHeader.size()) != 0) {
        fail("unexpected input header");
    }
    return static_cast<std::size_t>(header_end - data) + 1;
}

std::vector<std::size_t> make_boundaries(const char* data,
                                         std::size_t size,
                                         std::size_t data_start,
                                         unsigned thread_count) {
    std::vector<std::size_t> boundaries(thread_count + 1);
    boundaries[0] = data_start;
    const std::size_t payload_size = size - data_start;
    const std::size_t base_chunk = payload_size / thread_count;
    const std::size_t remainder = payload_size % thread_count;
    for (unsigned i = 1; i < thread_count; ++i) {
        std::size_t position = data_start + base_chunk * i +
                               remainder * i / thread_count;
        while (position < size && data[position - 1] != '\n') {
            ++position;
        }
        boundaries[i] = position;
    }
    boundaries[thread_count] = size;
    return boundaries;
}

int run(const char* input_path, const char* output_path) {
    MappedFile input(input_path);
    const std::size_t data_start =
        find_data_start(input.data(), input.size());
    const std::size_t payload_size = input.size() - data_start;

    unsigned hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        hardware_threads = kContestThreads;
    }
    unsigned thread_count =
        std::min(kContestThreads, hardware_threads);
    if (payload_size < std::size_t{64} * 1024) {
        thread_count = 1;
    }

    const std::vector<std::size_t> boundaries =
        make_boundaries(input.data(), input.size(), data_start, thread_count);
    std::vector<std::unique_ptr<ThreadResult>> partials(thread_count);
    std::vector<std::exception_ptr> errors(thread_count);
    std::vector<std::thread> workers;
    workers.reserve(thread_count > 0 ? thread_count - 1 : 0);

    auto work = [&](unsigned index) {
        try {
            partials[index] =
                parse_range(input.data() + boundaries[index],
                            input.data() + boundaries[index + 1]);
        } catch (...) {
            errors[index] = std::current_exception();
        }
    };

    try {
        for (unsigned i = 1; i < thread_count; ++i) {
            workers.emplace_back(work, i);
        }
    } catch (...) {
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }
    work(0);
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::exception_ptr& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    ThreadResult merged = std::move(*partials[0]);
    for (unsigned i = 1; i < thread_count; ++i) {
        merged.merge_from(*partials[i]);
    }
    const std::string output = build_output(merged);
    write_output(output_path, output);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
        return 2;
    }
    try {
        return run(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
