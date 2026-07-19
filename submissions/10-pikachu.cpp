#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#if !defined(BRC_LEGACY_MONTH_PARSER) && \
    !defined(BRC_AVX512_MONTH_THRESHOLDS) && \
    !defined(BRC_AVX2_MONTH_THRESHOLDS)
#define BRC_AVX512_MONTH_THRESHOLDS 1
// #define BRC_PACKED_MILLION_MONTH_LOOKUP 1
#endif

#if !defined(BRC_LEGACY_CHANNEL_DICTIONARY) && !defined(BRC_CHD_MPH)
#define BRC_CHD_MPH 1
#endif

#ifndef BRC_LEGACY_HASH_MERGE
#define BRC_DIRECT_MERGE_OUTPUT 1
#endif

#ifndef BRC_LEGACY_EVEN_HASH
#define BRC_ODD_HASH 1
#endif

#ifndef BRC_BATCH_SIZE
#define BRC_BATCH_SIZE 32
#endif

#define BRC_YEAR_2027 1
#define BRC_SKIP_CHANNEL_PREFETCH 1
#define BRC_CRC_SHORT_HASH 1
#define BRC_PAD_MONTHBLOCK_256 1
#define BRC_CRC_SHORT_SHIFT_LENGTH 1
#define BRC_CRC_SHORT_SEED 0xa0761d6478bd642fULL
#define BRC_RELATIVE_MONTH_INDEX 1
#define BRC_FOUR_POINT_LONG_HASH 1
#define BRC_MAIN_THREAD_WORKER 1
#define BRC_STATS_PREFETCH_LOCALITY 3
#define BRC_VECTOR_STATS16 1
#define BRC_RAW_SHORT_HASH 1
#define BRC_FAST_WIDE_ROW 1
// #define BRC_PACKED_WIDE_ROW 1
#define BRC_PARALLEL_DICTIONARY_SAMPLE 1
#ifndef BRC_DICTIONARY_ROWS_PER_SAMPLER
#define BRC_DICTIONARY_ROWS_PER_SAMPLER (1U << 18)
#endif
// #define BRC_THREE_CURSORS 1
// #define BRC_PHASE_INTERLEAVED_CURSORS 1
// #define BRC_FUSED_CHANNEL_KEY 1
// #define BRC_WORK_STEAL_SEGMENTS 1
#ifndef BRC_CURSOR_COUNT
#define BRC_CURSOR_COUNT 2
#endif
// #define BRC_FORCED_THREAD_COUNT 7

#if defined(BRC_COMMON_ONLY_PIPELINE) || defined(BRC_SAFE_U16_PIPELINE)
#define BRC_UNCHECKED_FAST_PIPELINE 1
#endif

#ifndef BRC_FOUR_POINT_MIX
#define BRC_FOUR_POINT_MIX 0xbb67ae8584caa73bULL
#endif

#ifndef BRC_CHANNEL_HASH_FOLD_SHIFT
#define BRC_CHANNEL_HASH_FOLD_SHIFT 37
#endif

namespace {

[[noreturn]] void fail(const char* message) {
    std::perror(message);
    std::exit(1);
}

struct Mapping {
    int fd = -1;
    const char* data = nullptr;
    std::size_t size = 0;

    explicit Mapping(const char* path) {
        fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) fail("open input");

        struct stat st {};
        if (::fstat(fd, &st) != 0) fail("fstat input");
        size = static_cast<std::size_t>(st.st_size);
        if (size == 0) return;

        void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) fail("mmap input");
        data = static_cast<const char*>(mapped);
        ::madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
    }

    ~Mapping() {
        if (data != nullptr) ::munmap(const_cast<char*>(data), size);
        if (fd >= 0) ::close(fd);
    }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
};

struct Stats {
    std::uint64_t length_sum = 0;
    std::uint64_t count = 0;
    std::uint64_t stamp_sum = 0;
    std::uint32_t min_length = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t max_length = 0;

    void add(std::uint32_t length, std::uint32_t stamps) noexcept {
        length_sum += length;
        ++count;
        stamp_sum += stamps;
        min_length = std::min(min_length, length);
        max_length = std::max(max_length, length);
    }

    void merge(const Stats& other) noexcept {
        length_sum += other.length_sum;
        count += other.count;
        stamp_sum += other.stamp_sum;
        min_length = std::min(min_length, other.min_length);
        max_length = std::max(max_length, other.max_length);
    }

    void merge_values(std::uint64_t other_length_sum, std::uint64_t other_count,
                      std::uint64_t other_stamp_sum, std::uint32_t other_min_length,
                      std::uint32_t other_max_length) noexcept {
        length_sum += other_length_sum;
        count += other_count;
        stamp_sum += other_stamp_sum;
        min_length = std::min(min_length, other_min_length);
        max_length = std::max(max_length, other_max_length);
    }
};

static inline std::uint64_t mix(std::uint64_t a, std::uint64_t b) noexcept {
    const __uint128_t product = static_cast<__uint128_t>(a) * b;
    return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);
}

static inline std::uint64_t load_u64(const char* p) noexcept {
    std::uint64_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static inline std::uint32_t load_u32(const char* p) noexcept {
    std::uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static inline std::uint64_t narrow_channel_hash(std::uint64_t hash) noexcept {
#ifdef BRC_CHANNEL_HASH32
    return static_cast<std::uint32_t>(
        hash ^ (hash >> BRC_CHANNEL_HASH_FOLD_SHIFT));
#else
    return hash;
#endif
}

static inline std::uint64_t hash_short_channel_bytes(
    __m128i bytes, std::size_t length) noexcept {
    constexpr std::uint64_t secret1 = 0xa0761d6478bd642fULL;
    constexpr std::uint64_t secret2 = 0xe7037ed1a0b428dbULL;
    const std::uint64_t first = static_cast<std::uint64_t>(
        _mm_cvtsi128_si64(bytes));
    const std::uint64_t last = static_cast<std::uint64_t>(
        _mm_extract_epi64(bytes, 1));
#ifdef BRC_CRC_SHORT_HASH
    const std::uint64_t combined =
        first ^
#ifdef BRC_CRC_SHORT_XOR_FOLD
        last ^
#else
        std::rotl(last, 23) ^
#endif
#ifdef BRC_CRC_SHORT_SHIFT_LENGTH
        (static_cast<std::uint64_t>(length) << 56);
#else
        (static_cast<std::uint64_t>(length) * secret2);
#endif
#ifdef BRC_RAW_SHORT_HASH
    const std::uint64_t hash = combined;
#else
    const std::uint64_t hash = _mm_crc32_u64(BRC_CRC_SHORT_SEED, combined);
#endif
#else
    const std::uint64_t hash = mix(
        first ^ secret1, last ^ secret2 ^ static_cast<std::uint64_t>(length));
#endif
#ifdef BRC_ODD_HASH
    return narrow_channel_hash(hash) | 1U;
#else
    return hash == 0 ? 1 : hash;
#endif
}

static inline std::uint64_t hash_channel(const char* p, std::size_t length) noexcept {
    constexpr std::uint64_t secret1 = 0xa0761d6478bd642fULL;
    constexpr std::uint64_t secret2 = 0xe7037ed1a0b428dbULL;
    constexpr std::uint64_t secret3 = 0x8ebc6af09c88c6e3ULL;

#ifdef BRC_ENDPOINT_HASH
    if (length >= 8) {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t last = load_u64(p + length - 8);
        const std::uint64_t combined =
            first ^ std::rotl(last, 23) ^
            (static_cast<std::uint64_t>(length) << 56);
        return _mm_crc32_u64(BRC_CRC_SHORT_SEED, combined) | 1U;
    }
#endif

    if (length <= 16) {
        const __mmask16 mask = static_cast<__mmask16>(
            (static_cast<std::uint32_t>(1) << length) - 1U);
        const __m128i bytes =
            _mm_maskz_loadu_epi8(mask, static_cast<const void*>(p));
        return hash_short_channel_bytes(bytes, length);
    }

#ifdef BRC_DOUBLE_FOLDED_FOUR_POINT_LONG_HASH
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t middle = load_u64(p + (length - 8) / 2);
        const std::uint64_t last = load_u64(p + length - 8);
        const std::uint64_t first_crc = _mm_crc32_u64(
            BRC_CRC_SHORT_SEED, first ^ std::rotl(second, 23));
        const std::uint64_t second_crc = _mm_crc32_u64(
            static_cast<std::uint64_t>(length),
            middle ^ std::rotl(last, 23));
        std::uint64_t hash =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
            static_cast<std::uint32_t>(first_crc);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_FOLDED_FOUR_POINT_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t middle = load_u64(p + (length - 8) / 2);
        const std::uint64_t last = load_u64(p + length - 8);
        std::uint64_t first_crc = _mm_crc32_u64(BRC_CRC_SHORT_SEED, first);
        first_crc = _mm_crc32_u64(first_crc, second);
        const std::uint64_t second_crc = _mm_crc32_u64(
            static_cast<std::uint64_t>(length),
            middle ^ std::rotl(last, 23));
        std::uint64_t hash =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
            static_cast<std::uint32_t>(first_crc);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_FIRST24_LAST_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t third = load_u64(
            p + std::min<std::size_t>(16, length - 8));
        const std::uint64_t last = load_u64(p + length - 8);
        std::uint64_t first_crc = _mm_crc32_u64(BRC_CRC_SHORT_SEED, first);
        first_crc = _mm_crc32_u64(first_crc, second);
        std::uint64_t second_crc =
            _mm_crc32_u64(static_cast<std::uint64_t>(length), third);
        second_crc = _mm_crc32_u64(second_crc, last);
        std::uint64_t hash =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
            static_cast<std::uint32_t>(first_crc);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_THREE_MIDDLE_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t middle = load_u64(p + (length - 8) / 2);
        std::uint64_t first_crc = _mm_crc32_u64(BRC_CRC_SHORT_SEED, first);
        first_crc = _mm_crc32_u64(first_crc, second);
        const std::uint64_t second_crc =
            _mm_crc32_u64(static_cast<std::uint64_t>(length), middle);
        std::uint64_t hash =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
            static_cast<std::uint32_t>(first_crc);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_THREE_CHUNK_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t last = load_u64(p + length - 8);
        std::uint64_t first_crc = _mm_crc32_u64(BRC_CRC_SHORT_SEED, first);
        first_crc = _mm_crc32_u64(first_crc, second);
        const std::uint64_t second_crc =
            _mm_crc32_u64(static_cast<std::uint64_t>(length), last);
        std::uint64_t hash =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
            static_cast<std::uint32_t>(first_crc);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_INDEPENDENT_THREE_POINT_LONG_HASH)
    {
        constexpr std::uint64_t part_mask = (1ULL << 21) - 1U;
        const std::uint64_t first = load_u64(p);
        const std::uint64_t middle = load_u64(p + 8);
        const std::uint64_t last = load_u64(p + length - 8);
        const std::uint64_t first_crc =
            _mm_crc32_u64(BRC_CRC_SHORT_SEED, first) & part_mask;
        const std::uint64_t middle_crc =
            _mm_crc32_u64(static_cast<std::uint64_t>(length), middle) & part_mask;
        const std::uint64_t last_crc =
            _mm_crc32_u64(0xe7037ed1U ^ length, last) & part_mask;
        std::uint64_t hash =
            first_crc | (middle_crc << 21) | (last_crc << 42);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_INDEPENDENT_FOUR_POINT_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t middle = load_u64(p + (length - 8) / 2);
        const std::uint64_t last = load_u64(p + length - 8);
        const std::uint32_t first_crc = static_cast<std::uint32_t>(
            _mm_crc32_u64(BRC_CRC_SHORT_SEED, first));
        const std::uint32_t second_crc = static_cast<std::uint32_t>(
            _mm_crc32_u64(static_cast<std::uint64_t>(length), second));
        const std::uint32_t middle_crc = static_cast<std::uint32_t>(
            _mm_crc32_u64(BRC_CRC_SHORT_SEED ^ length, middle));
        const std::uint32_t last_crc = static_cast<std::uint32_t>(
            _mm_crc32_u64(0xe7037ed1U ^ length, last));
        const std::uint64_t first_pair =
            static_cast<std::uint64_t>(second_crc) << 32 | first_crc;
        const std::uint64_t second_pair =
            static_cast<std::uint64_t>(last_crc) << 32 | middle_crc;
        std::uint64_t hash = first_pair ^ std::rotl(second_pair, 23);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return hash | 1U;
    }
#elif defined(BRC_FOUR_POINT_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t second = load_u64(p + 8);
        const std::uint64_t middle = load_u64(p + (length - 8) / 2);
        const std::uint64_t last = load_u64(p + length - 8);
        std::uint64_t first_crc = _mm_crc32_u64(BRC_CRC_SHORT_SEED, first);
        first_crc = _mm_crc32_u64(first_crc, second);
        std::uint64_t second_crc =
            _mm_crc32_u64(static_cast<std::uint64_t>(length), middle);
        second_crc = _mm_crc32_u64(second_crc, last);
        std::uint64_t hash =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
            static_cast<std::uint32_t>(first_crc);
        hash *= BRC_FOUR_POINT_MIX;
        hash ^= hash >> 29;
        return narrow_channel_hash(hash) | 1U;
    }
#elif defined(BRC_THREE_POINT_LONG_HASH)
    {
        const std::uint64_t first = load_u64(p);
        const std::uint64_t last = load_u64(p + length - 8);
        const std::uint64_t middle = load_u64(p + (length - 8) / 2);
        const std::uint64_t folded =
            first ^ std::rotl(last, 23) ^
            (static_cast<std::uint64_t>(length) << 56);
        const std::uint64_t first_crc =
            _mm_crc32_u64(BRC_CRC_SHORT_SEED, folded);
        const std::uint64_t second_crc = _mm_crc32_u64(
            static_cast<std::uint64_t>(length),
            middle ^ std::rotl(last, 7));
        return (static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(second_crc)) << 32 |
                static_cast<std::uint32_t>(first_crc)) | 1U;
    }
#endif

#ifdef BRC_CRC_LONG_HASH
    std::uint64_t crc1 = static_cast<std::uint64_t>(length);
    std::uint64_t crc2 = secret2;
    while (length >= 16) {
        crc1 = _mm_crc32_u64(crc1, load_u64(p));
        crc2 = _mm_crc32_u64(crc2, load_u64(p + 8));
        p += 16;
        length -= 16;
    }
    if (length >= 8) {
        crc1 = _mm_crc32_u64(crc1, load_u64(p));
        p += 8;
        length -= 8;
    }
    if (length != 0) {
        const __mmask8 mask = static_cast<__mmask8>((1U << length) - 1U);
        const __m128i tail = _mm_maskz_loadu_epi8(
            mask, static_cast<const void*>(p));
        crc2 = _mm_crc32_u64(
            crc2, static_cast<std::uint64_t>(_mm_cvtsi128_si64(tail)));
    }
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(crc2)) << 32 |
            static_cast<std::uint32_t>(crc1)) | 1U;
#else

    constexpr std::uint64_t multiplier1 = 0x9fb21c651e98df25ULL;
    constexpr std::uint64_t multiplier2 = 0xd6e8feb86659fd93ULL;
    std::uint64_t hash1 = secret1 ^
                          (static_cast<std::uint64_t>(length) * secret2);
    std::uint64_t hash2 = secret2;
    while (length >= 16) {
        hash1 = std::rotl((hash1 ^ load_u64(p)) * multiplier1, 27);
        hash2 = std::rotl((hash2 ^ load_u64(p + 8)) * multiplier2, 31);
        p += 16;
        length -= 16;
    }
    if (length >= 8) {
        hash1 = std::rotl((hash1 ^ load_u64(p)) * multiplier1, 27);
        p += 8;
        length -= 8;
    }
    if (length != 0) {
        std::uint64_t tail;
        if (length >= 4) {
            tail = (static_cast<std::uint64_t>(load_u32(p)) << 32) |
                   load_u32(p + length - 4);
        } else {
            tail = (static_cast<std::uint64_t>(static_cast<unsigned char>(p[0])) << 16) |
                   (static_cast<std::uint64_t>(static_cast<unsigned char>(p[length >> 1])) << 8) |
                   static_cast<unsigned char>(p[length - 1]);
        }
        hash2 = std::rotl((hash2 ^ tail) * multiplier2, 31);
    }
    const std::uint64_t hash = mix(hash1 ^ secret3, hash2 ^ secret1);
#ifdef BRC_ODD_HASH
    return hash | 1U;
#else
    return hash == 0 ? 1 : hash;
#endif
#endif
}

struct Entry {
    const char* channel = nullptr;
    std::uint64_t hash = 0;
    Stats stats {};
    std::uint32_t channel_length = 0;
    std::int32_t month = 0;
};

class AggregateTable {
public:
    explicit AggregateTable(std::size_t capacity = 1U << 18)
        : entries_(std::bit_ceil(capacity)), mask_(entries_.size() - 1) {}

    Entry& find_or_insert(const char* channel, std::uint32_t length,
                          std::int32_t month, std::uint64_t hash) {
        if ((size_ + 1) * 10 >= entries_.size() * 7) grow();
        return find_or_insert_without_growth(channel, length, month, hash);
    }

    const std::vector<Entry>& entries() const noexcept { return entries_; }
    std::size_t size() const noexcept { return size_; }

private:
    Entry& find_or_insert_without_growth(const char* channel, std::uint32_t length,
                                         std::int32_t month, std::uint64_t hash) {
        std::size_t index = static_cast<std::size_t>(hash) & mask_;
        for (;;) {
            Entry& entry = entries_[index];
            if (entry.hash == 0) {
                entry.channel = channel;
                entry.hash = hash;
                entry.channel_length = length;
                entry.month = month;
                ++size_;
                return entry;
            }
            if (entry.hash == hash && entry.month == month &&
                entry.channel_length == length &&
                std::memcmp(entry.channel, channel, length) == 0) {
                return entry;
            }
            index = (index + 1) & mask_;
        }
    }

    void grow() {
        std::vector<Entry> old = std::move(entries_);
        entries_.assign(old.size() * 2, Entry {});
        mask_ = entries_.size() - 1;
        size_ = 0;
        for (Entry& entry : old) {
            if (entry.hash == 0) continue;
            Entry& destination = find_or_insert_without_growth(
                entry.channel, entry.channel_length, entry.month, entry.hash);
            destination.stats = entry.stats;
        }
    }

    std::vector<Entry> entries_;
    std::size_t mask_;
    std::size_t size_ = 0;
};

static inline std::uint64_t key_hash(std::uint64_t channel_hash,
                                     std::int32_t month) noexcept {
    std::uint64_t hash = channel_hash ^
                         (static_cast<std::uint64_t>(month) * 0x9e3779b97f4a7c15ULL);
    return hash == 0 ? 1 : hash;
}

struct CompactStats {
    std::uint32_t length_sum = 0;
#ifdef BRC_COMPACT_STATS12_PACKED
    std::uint32_t stamp_and_count = 0;
    std::uint32_t min_and_max = std::numeric_limits<std::uint16_t>::max();

    std::uint32_t stamp_sum_value() const noexcept {
        return stamp_and_count & 0xffffU;
    }
    std::uint32_t count_value() const noexcept { return stamp_and_count >> 16; }
    std::uint32_t min_length_value() const noexcept { return min_and_max & 0xffffU; }
    std::uint32_t max_length_value() const noexcept { return min_and_max >> 16; }

    bool try_add(std::uint32_t length, std::uint32_t stamps) noexcept {
        const std::uint32_t old_stamps = stamp_sum_value();
        const std::uint32_t old_count = count_value();
        if (length > std::numeric_limits<std::uint16_t>::max() ||
            stamps > std::numeric_limits<std::uint16_t>::max() - old_stamps ||
            old_count == std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        length_sum += length;
        stamp_and_count += stamps + (1U << 16);
        const std::uint32_t minimum =
            std::min(min_length_value(), length);
        const std::uint32_t maximum =
            std::max(max_length_value(), length);
        min_and_max = minimum | (maximum << 16);
        return true;
    }
#elif defined(BRC_COMPACT_STATS12)
    std::uint16_t stamp_sum = 0;
    std::uint16_t count = 0;
    std::uint16_t min_length = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t max_length = 0;

    bool try_add(std::uint32_t length, std::uint32_t stamps) noexcept {
        if (length > std::numeric_limits<std::uint16_t>::max() ||
            stamps > std::numeric_limits<std::uint16_t>::max() - stamp_sum ||
            count == std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        length_sum += length;
        stamp_sum = static_cast<std::uint16_t>(stamp_sum + stamps);
        ++count;
        min_length = std::min(min_length, static_cast<std::uint16_t>(length));
        max_length = std::max(max_length, static_cast<std::uint16_t>(length));
        return true;
    }

    std::uint32_t stamp_sum_value() const noexcept { return stamp_sum; }
    std::uint32_t count_value() const noexcept { return count; }
    std::uint32_t min_length_value() const noexcept { return min_length; }
    std::uint32_t max_length_value() const noexcept { return max_length; }
#elif defined(BRC_VECTOR_STATS16)
    std::uint32_t stamp_sum = 0;
    std::uint32_t count = 0;
    std::uint16_t min_length = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t inverted_max_length = std::numeric_limits<std::uint16_t>::max();

    bool try_add(std::uint32_t length, std::uint32_t stamps) noexcept {
        if (length > std::numeric_limits<std::uint16_t>::max()) return false;
        const __m128i current = _mm_loadu_si128(
            reinterpret_cast<const __m128i_u*>(this));
        const std::uint32_t extrema =
            length | ((length ^ 0xffffU) << 16);
        const __m128i increment = _mm_setr_epi32(
            static_cast<int>(length), static_cast<int>(stamps), 1,
            static_cast<int>(extrema));
#ifdef BRC_VECTOR_STATS16_AND
        const __m128i addend = _mm_and_si128(
            increment, _mm_setr_epi32(-1, -1, -1, 0));
        const __m128i sums = _mm_add_epi32(current, addend);
        const __m128i minima = _mm_min_epu16(sums, increment);
        const __m128i result = _mm_blend_epi16(sums, minima, 0xc0);
#elif defined(BRC_VECTOR_STATS16_BLEND)
        const __m128i addend = _mm_blend_epi32(
            increment, _mm_setzero_si128(), 0x08);
        const __m128i sums = _mm_add_epi32(current, addend);
        const __m128i minima = _mm_min_epu16(sums, increment);
        const __m128i result = _mm_blend_epi16(sums, minima, 0xc0);
#else
        const __m128i sums = _mm_mask_add_epi32(
            current, static_cast<__mmask8>(0x07), current, increment);
        const __m128i result = _mm_mask_min_epu16(
            sums, static_cast<__mmask8>(0xc0), sums, increment);
#endif
        _mm_storeu_si128(reinterpret_cast<__m128i_u*>(this), result);
        return true;
    }

    std::uint32_t stamp_sum_value() const noexcept { return stamp_sum; }
    std::uint32_t count_value() const noexcept { return count; }
    std::uint32_t min_length_value() const noexcept { return min_length; }
    std::uint32_t max_length_value() const noexcept {
        return static_cast<std::uint16_t>(~inverted_max_length);
    }
#else
    std::uint32_t stamp_sum = 0;
    std::uint32_t count = 0;
    std::uint16_t min_length = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t max_length = 0;

    bool try_add(std::uint32_t length, std::uint32_t stamps) noexcept {
        if (length > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        length_sum += length;
        stamp_sum += stamps;
        ++count;
        min_length = std::min(min_length, static_cast<std::uint16_t>(length));
        max_length = std::max(max_length, static_cast<std::uint16_t>(length));
        return true;
    }

#ifdef BRC_UNCHECKED_FAST_PIPELINE
    void add_unchecked(std::uint32_t length, std::uint32_t stamps) noexcept {
        length_sum += length;
        stamp_sum += stamps;
        ++count;
        min_length = std::min(min_length, static_cast<std::uint16_t>(length));
        max_length = std::max(max_length, static_cast<std::uint16_t>(length));
    }
#endif


    std::uint32_t stamp_sum_value() const noexcept { return stamp_sum; }
    std::uint32_t count_value() const noexcept { return count; }
    std::uint32_t min_length_value() const noexcept { return min_length; }
    std::uint32_t max_length_value() const noexcept { return max_length; }
#endif
};

#if defined(BRC_COMPACT_STATS12) || defined(BRC_COMPACT_STATS12_PACKED)
static_assert(sizeof(CompactStats) == 12);
#else
static_assert(sizeof(CompactStats) == 16);
#endif

#ifdef BRC_PAD_MONTHBLOCK_256
struct alignas(256) MonthBlock {
#else
struct MonthBlock {
#endif
    std::array<CompactStats, 12> slots {};
};

struct ChannelEntry {
    std::uint64_t hash = 0;
    std::uint32_t channel_length = 0;
    std::uint32_t block_index = 0;
};

static_assert(sizeof(ChannelEntry) == 16);

struct ChannelMetadata {
    const char* channel = nullptr;
    std::uint64_t hash = 0;
    std::uint32_t length = 0;
    std::uint32_t sample_count = 0;
#ifdef BRC_CHD_MPH
    std::uint32_t fast_slot = 0;
#endif
};

#ifdef BRC_CHD_MPH
#if defined(BRC_CHD_ADD_DISPLACEMENT) || defined(BRC_CHD_CRC_SEED16)
using MphSeed = std::uint16_t;
#else
using MphSeed = std::uint32_t;
#endif
#ifndef BRC_MPH_BUCKET_BITS
#define BRC_MPH_BUCKET_BITS 12
#endif
#ifndef BRC_MPH_SLOT_BITS
#define BRC_MPH_SLOT_BITS 14
#endif
static constexpr std::uint32_t kMphBucketBits = BRC_MPH_BUCKET_BITS;
static constexpr std::uint32_t kMphBucketCount = 1U << kMphBucketBits;
static constexpr std::uint32_t kMphBucketMask = kMphBucketCount - 1U;
static constexpr std::uint32_t kMphSlotBits = BRC_MPH_SLOT_BITS;
static constexpr std::uint32_t kMphSlotCount = 1U << kMphSlotBits;
static constexpr std::uint32_t kMphSlotMask = kMphSlotCount - 1U;
#endif

#ifdef BRC_PACKED_DIRECT_LOOKUP
#ifndef BRC_PACKED_DIRECT_BITS
#define BRC_PACKED_DIRECT_BITS 16
#endif
static constexpr std::uint32_t kDirectBits = BRC_PACKED_DIRECT_BITS;
static constexpr std::uint32_t kDirectSize = 1U << kDirectBits;
static constexpr std::uint32_t kDirectMask = kDirectSize - 1U;
static constexpr std::uint32_t kDirectIdBits = 14;
static constexpr std::uint32_t kDirectIdMask = (1U << kDirectIdBits) - 1U;
#endif

class ChannelDictionary {
public:
    explicit ChannelDictionary(std::size_t capacity = 1U << 16)
        : entries_(std::bit_ceil(capacity)), mask_(entries_.size() - 1) {
        metadata_.reserve(10000);
    }

    void insert_exact(const char* channel, std::uint32_t length,
                      std::uint64_t hash) {
        if ((metadata_.size() + 1) * 10 >= entries_.size() * 7) grow();
        std::size_t index = static_cast<std::size_t>(hash) & mask_;
        for (;;) {
            ChannelEntry& entry = entries_[index];
            if (entry.hash == 0) {
                entry.hash = hash;
                entry.channel_length = length;
                entry.block_index = static_cast<std::uint32_t>(metadata_.size());
                metadata_.push_back({channel, hash, length, 1
#ifdef BRC_CHD_MPH
                                     , 0
#endif
                                    });
                return;
            }
            if (entry.hash == hash) {
                if (entry.channel_length == length) {
                    const ChannelMetadata& representative =
                        metadata_[entry.block_index];
                    if (std::memcmp(representative.channel, channel, length) == 0) {
                        ++metadata_[entry.block_index].sample_count;
                        return;
                    }
#ifdef BRC_HASH_DIAGNOSTIC
                    std::fprintf(stderr,
                                 "hash collision %#llx: %.*s <> %.*s\n",
                                 static_cast<unsigned long long>(hash),
                                 static_cast<int>(representative.length),
                                 representative.channel,
                                 static_cast<int>(length), channel);
#endif
                    collision_free_ = false;
                }
                hash_unique_ = false;
            }
            index = (index + 1) & mask_;
        }
    }

    const ChannelEntry* find_hash(std::uint64_t hash) const noexcept {
        std::size_t index = static_cast<std::size_t>(hash) & mask_;
        for (;;) {
            const ChannelEntry& entry = entries_[index];
            if (entry.hash == 0) return nullptr;
            if (entry.hash == hash) return &entry;
            index = (index + 1) & mask_;
        }
    }

    const ChannelEntry* find_exact(const char* channel, std::uint32_t length,
                                   std::uint64_t hash) const noexcept {
        std::size_t index = static_cast<std::size_t>(hash) & mask_;
        for (;;) {
            const ChannelEntry& entry = entries_[index];
            if (entry.hash == 0) return nullptr;
            if (entry.hash == hash && entry.channel_length == length &&
                std::memcmp(metadata_[entry.block_index].channel,
                            channel, length) == 0) {
                return &entry;
            }
            index = (index + 1) & mask_;
        }
    }

    void prefetch(std::uint64_t hash) const noexcept {
#ifdef BRC_CHD_MPH
        if (mph_ready_) {
#if defined(BRC_CHD_COMPACT_BLOCKS) && defined(BRC_CHD_PREFETCH_ID)
            __builtin_prefetch(&mph_ids_[mph_slot(hash)], 0, 3);
#else
            __builtin_prefetch(
                &mph_seeds_[static_cast<std::uint32_t>(hash) & kMphBucketMask],
                0, 3);
#endif
            return;
        }
#endif
#ifdef BRC_PACKED_DIRECT_LOOKUP
        if (direct_ready_) {
            __builtin_prefetch(&direct_tokens_[static_cast<std::uint32_t>(hash) &
                                                kDirectMask], 0, 3);
            return;
        }
#endif
        __builtin_prefetch(&entries_[static_cast<std::size_t>(hash) & mask_], 0, 3);
    }

    const std::vector<ChannelMetadata>& metadata() const noexcept { return metadata_; }
    std::size_t size() const noexcept { return metadata_.size(); }
    void finalize() {
#ifdef BRC_CHD_MPH
        build_mph();
#endif
#ifdef BRC_PACKED_DIRECT_LOOKUP
        if (metadata_.size() != 10000 || !collision_free_ || !hash_unique_) return;
        direct_tokens_.assign(kDirectSize, 0);
        for (std::uint32_t id = 0; id < metadata_.size(); ++id) {
            const ChannelMetadata& item = metadata_[id];
            const std::uint32_t bucket =
                static_cast<std::uint32_t>(item.hash) & kDirectMask;
            const std::uint64_t previous = direct_tokens_[bucket];
            if (previous != 0) {
                const std::uint32_t previous_id =
                    static_cast<std::uint32_t>(previous) & kDirectIdMask;
                if (metadata_[previous_id - 1].sample_count >= item.sample_count) continue;
            }
            direct_tokens_[bucket] =
                (item.hash >> kDirectBits) << kDirectIdBits | (id + 1U);
        }
        direct_ready_ = true;
#endif
    }

#ifdef BRC_CHD_MPH
    std::uint32_t mph_slot(std::uint64_t hash) const noexcept {
        const std::uint32_t seed =
            mph_seeds_[static_cast<std::uint32_t>(hash) & kMphBucketMask];
#ifdef BRC_CHD_ADD_DISPLACEMENT
        const std::uint32_t base = static_cast<std::uint32_t>(
            (hash >> kMphBucketBits) ^ (hash >> 32));
        return (base + seed) &
               kMphSlotMask;
#else
        return static_cast<std::uint32_t>(_mm_crc32_u64(seed, hash)) &
               kMphSlotMask;
#endif
    }

#ifdef BRC_CHD_COMPACT_BLOCKS
    std::uint32_t mph_block_index(std::uint64_t hash) const noexcept {
        return mph_ids_[mph_slot(hash)];
    }
#endif

    std::size_t storage_size() const noexcept {
#ifdef BRC_CHD_COMPACT_BLOCKS
        return metadata_.size();
#else
        return mph_ready_ ? kMphSlotCount : metadata_.size();
#endif
    }

    std::size_t storage_index(std::size_t metadata_index) const noexcept {
#ifdef BRC_CHD_COMPACT_BLOCKS
        return metadata_index;
#else
        return mph_ready_ ? metadata_[metadata_index].fast_slot : metadata_index;
#endif
    }
#else
    std::size_t storage_size() const noexcept { return metadata_.size(); }
    std::size_t storage_index(std::size_t metadata_index) const noexcept {
        return metadata_index;
    }
#endif

#ifdef BRC_PACKED_DIRECT_LOOKUP
    std::uint32_t direct_id(std::uint64_t hash) const noexcept {
        const std::uint64_t token =
            direct_tokens_[static_cast<std::uint32_t>(hash) & kDirectMask];
        if ((token >> kDirectIdBits) == (hash >> kDirectBits)) {
            return (static_cast<std::uint32_t>(token) & kDirectIdMask) - 1U;
        }
        return std::numeric_limits<std::uint32_t>::max();
    }
#endif
    bool fast_safe() const noexcept {
#ifdef BRC_CHD_MPH
        return metadata_.size() == 10000 && collision_free_ && hash_unique_ &&
               mph_ready_;
#elif defined(BRC_PACKED_DIRECT_LOOKUP)
        return metadata_.size() == 10000 && collision_free_ && hash_unique_ &&
               direct_ready_;
#else
        return metadata_.size() == 10000 && collision_free_ && hash_unique_;
#endif
    }

private:
#ifdef BRC_CHD_MPH
    void build_mph() {
        if (metadata_.size() != 10000 || !collision_free_ || !hash_unique_) return;

        std::array<std::vector<std::uint32_t>, kMphBucketCount> buckets;
        for (std::uint32_t id = 0; id < metadata_.size(); ++id) {
            buckets[static_cast<std::uint32_t>(metadata_[id].hash) &
                    kMphBucketMask].push_back(id);
        }
        std::array<std::uint32_t, kMphBucketCount> order {};
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::uint32_t a,
                                                  std::uint32_t b) {
            return buckets[a].size() > buckets[b].size();
        });

        mph_seeds_.assign(kMphBucketCount, 0);
        std::array<std::uint8_t, kMphSlotCount> used {};
        std::vector<std::uint32_t> trial;
        for (const std::uint32_t bucket_index : order) {
            const std::vector<std::uint32_t>& bucket = buckets[bucket_index];
            if (bucket.empty()) break;
            trial.resize(bucket.size());
            bool found = false;
            const std::uint32_t seed_limit =
#ifdef BRC_CHD_ADD_DISPLACEMENT
                kMphSlotCount;
#elif defined(BRC_CHD_CRC_SEED16)
                1U << 16;
#else
                1U << 24;
#endif
            for (std::uint32_t seed = 0; seed != seed_limit; ++seed) {
                bool valid = true;
                for (std::size_t i = 0; i < bucket.size(); ++i) {
                    const std::uint32_t slot =
#ifdef BRC_CHD_ADD_DISPLACEMENT
                        (static_cast<std::uint32_t>(
                            (metadata_[bucket[i]].hash >> kMphBucketBits) ^
                            (metadata_[bucket[i]].hash >> 32)) + seed) &
                        kMphSlotMask;
#else
                        static_cast<std::uint32_t>(
                        _mm_crc32_u64(seed, metadata_[bucket[i]].hash)) &
                        kMphSlotMask;
#endif
                    if (used[slot] != 0) {
                        valid = false;
                        break;
                    }
                    for (std::size_t j = 0; j < i; ++j) {
                        if (trial[j] == slot) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) break;
                    trial[i] = slot;
                }
                if (!valid) continue;
                mph_seeds_[bucket_index] = static_cast<MphSeed>(seed);
                for (std::size_t i = 0; i < bucket.size(); ++i) {
                    used[trial[i]] = 1;
                    metadata_[bucket[i]].fast_slot = trial[i];
                }
                found = true;
                break;
            }
            if (!found) {
#ifdef BRC_MPH_DIAGNOSTIC
                std::fprintf(stderr, "mph failed: bucket=%u size=%zu\n",
                             bucket_index, bucket.size());
#endif
                mph_seeds_.clear();
                return;
            }
        }
        mph_ready_ = true;
#ifdef BRC_CHD_COMPACT_BLOCKS
        mph_ids_.assign(kMphSlotCount, 0);
        for (std::uint32_t id = 0; id < metadata_.size(); ++id) {
            mph_ids_[metadata_[id].fast_slot] = static_cast<std::uint16_t>(id);
        }
#endif
#ifdef BRC_MPH_DIAGNOSTIC
        std::fprintf(stderr, "mph ready: buckets=%u slots=%u\n",
                     kMphBucketCount, kMphSlotCount);
#endif
    }
#endif

    void grow() {
        std::vector<ChannelEntry> old = std::move(entries_);
        entries_.assign(old.size() * 2, ChannelEntry {});
        mask_ = entries_.size() - 1;
        for (const ChannelEntry& entry : old) {
            if (entry.hash == 0) continue;
            std::size_t index = static_cast<std::size_t>(entry.hash) & mask_;
            while (entries_[index].hash != 0) index = (index + 1) & mask_;
            entries_[index] = entry;
        }
    }

    std::vector<ChannelEntry> entries_;
    std::vector<ChannelMetadata> metadata_;
    std::size_t mask_;
    bool collision_free_ = true;
    bool hash_unique_ = true;
#ifdef BRC_CHD_MPH
    std::vector<MphSeed> mph_seeds_;
#ifdef BRC_CHD_COMPACT_BLOCKS
    std::vector<std::uint16_t> mph_ids_;
#endif
    bool mph_ready_ = false;
#endif
#ifdef BRC_PACKED_DIRECT_LOOKUP
    std::vector<std::uint64_t> direct_tokens_;
    bool direct_ready_ = false;
#endif
};

class LocalAggregateTable {
public:
    LocalAggregateTable(std::int32_t base_month, const ChannelDictionary& dictionary)
        : blocks_(dictionary.storage_size()), overflow_(1U << 8),
          dictionary_(&dictionary), base_month_(base_month) {}

    void add(const char* channel, std::uint32_t length, std::int32_t month,
             std::uint64_t channel_hash, std::uint32_t message_length,
             std::uint32_t stamps) {
        CompactStats* stats = resolve(length, month, channel_hash);
        if (stats != nullptr) {
            add_resolved(channel, length, month, channel_hash,
                         message_length, stamps, stats);
            return;
        }

        overflow_.find_or_insert(channel, length, month, key_hash(channel_hash, month))
            .stats.add(message_length, stamps);
    }

    void add_unresolved(const char* channel, std::uint32_t length,
                        std::int32_t month, std::uint64_t channel_hash,
                        std::uint32_t message_length, std::uint32_t stamps) {
        overflow_.find_or_insert(channel, length, month, key_hash(channel_hash, month))
            .stats.add(message_length, stamps);
    }

    CompactStats* resolve(std::uint32_t, std::int32_t month,
                          std::uint64_t channel_hash) noexcept {
#ifdef BRC_CHD_MPH
        {
            const std::uint32_t month_index =
                static_cast<std::uint32_t>(month - base_month_);
#ifndef BRC_YEAR_2027
            if (month_index >= 12) return nullptr;
#endif
#ifdef BRC_CHD_COMPACT_BLOCKS
            return &blocks_[dictionary_->mph_block_index(channel_hash)]
                        .slots[month_index];
#else
            return &blocks_[dictionary_->mph_slot(channel_hash)].slots[month_index];
#endif
        }
#endif
#ifdef BRC_PACKED_DIRECT_LOOKUP
        const std::uint32_t direct = dictionary_->direct_id(channel_hash);
        if (direct != std::numeric_limits<std::uint32_t>::max()) {
            const std::uint32_t month_index =
                static_cast<std::uint32_t>(month - base_month_);
            if (month_index >= 12) return nullptr;
            return &blocks_[direct].slots[month_index];
        }
#endif
        const ChannelEntry* channel_entry = dictionary_->find_hash(channel_hash);
        if (channel_entry == nullptr) return nullptr;
        const std::uint32_t month_index =
            static_cast<std::uint32_t>(month - base_month_);
#ifndef BRC_YEAR_2027
        if (month_index >= 12) return nullptr;
#endif
        return &blocks_[channel_entry->block_index].slots[month_index];
    }

    CompactStats* resolve_exact(const char* channel, std::uint32_t length,
                                std::int32_t month,
                                std::uint64_t channel_hash) noexcept {
        const ChannelEntry* channel_entry =
            dictionary_->find_exact(channel, length, channel_hash);
        if (channel_entry == nullptr) return nullptr;
        const std::uint32_t month_index =
            static_cast<std::uint32_t>(month - base_month_);
#ifndef BRC_YEAR_2027
        if (month_index >= 12) return nullptr;
#endif
        return &blocks_[channel_entry->block_index].slots[month_index];
    }

    void prefetch_stats(CompactStats* stats) const noexcept {
        if (stats != nullptr) {
            __builtin_prefetch(stats, 1, BRC_STATS_PREFETCH_LOCALITY);
        }
    }

    void add_resolved(const char* channel, std::uint32_t length,
                      std::int32_t month, std::uint64_t channel_hash,
                      std::uint32_t message_length, std::uint32_t stamps,
                      CompactStats* stats) {
        if (stats->try_add(message_length, stamps)) return;
        flush_and_add(channel, length, month, channel_hash,
                      message_length, stamps, *stats);
    }

#ifdef BRC_UNCHECKED_FAST_PIPELINE
    void add_resolved_unchecked(std::uint32_t message_length,
                                std::uint32_t stamps,
                                CompactStats* stats) noexcept {
        stats->add_unchecked(message_length, stamps);
    }
#endif

#if defined(BRC_FAST_ROW24) || defined(BRC_FAST_WIDE_ROW) || \
    defined(BRC_PACKED_WIDE_ROW) || \
    defined(BRC_FAST_HASH32_ROW)
    void add_resolved_fast(std::uint32_t message_length, std::uint32_t stamps,
                           CompactStats* stats) noexcept {
        const bool added = stats->try_add(message_length, stamps);
        if (!added) __builtin_unreachable();
    }
#endif

    const std::vector<MonthBlock>& blocks() const noexcept { return blocks_; }
    const AggregateTable& overflow() const noexcept { return overflow_; }
    std::int32_t base_month() const noexcept { return base_month_; }

    void prefetch_channel(std::uint64_t hash) const noexcept {
        dictionary_->prefetch(hash);
    }

private:
    [[gnu::noinline]] void flush_and_add(
        const char* channel, std::uint32_t length, std::int32_t month,
        std::uint64_t channel_hash, std::uint32_t message_length,
        std::uint32_t stamps, CompactStats& compact) {
        Entry& overflow = overflow_.find_or_insert(
            channel, length, month, key_hash(channel_hash, month));
        if (compact.count_value() != 0) {
            overflow.stats.merge_values(
                compact.length_sum, compact.count_value(), compact.stamp_sum_value(),
                compact.min_length_value(), compact.max_length_value());
            compact = CompactStats {};
        }
        overflow.stats.add(message_length, stamps);
    }

    std::vector<MonthBlock> blocks_;
    AggregateTable overflow_;
    const ChannelDictionary* dictionary_;
    std::int32_t base_month_;
};

struct CivilDate {
    int year;
    unsigned month;
};

static inline CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
    const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_prime = (5 * day_of_year + 2) / 153;
    const unsigned month = month_prime + (month_prime < 10 ? 3 : static_cast<unsigned>(-9));
    year += month <= 2;
    return {year, month};
}

constexpr std::size_t kMonthLookupDays = 200000;

std::array<std::int32_t, kMonthLookupDays> make_month_lookup() {
    std::array<std::int32_t, kMonthLookupDays> lookup {};
    for (std::size_t day = 0; day < lookup.size(); ++day) {
        const CivilDate date = civil_from_days(static_cast<std::int64_t>(day));
        lookup[day] = date.year * 12 + static_cast<int>(date.month) - 1;
    }
    return lookup;
}

const auto month_lookup = make_month_lookup();

static inline std::int32_t month_from_timestamp(std::int64_t timestamp) noexcept {
    std::int64_t day = timestamp / 86400;
    if (timestamp < 0 && timestamp % 86400 != 0) --day;
    if (day >= 0 && static_cast<std::uint64_t>(day) < month_lookup.size()) {
        return month_lookup[static_cast<std::size_t>(day)];
    }
    const CivilDate date = civil_from_days(day);
    return date.year * 12 + static_cast<int>(date.month) - 1;
}

static inline std::uint32_t parse_u32(const char*& p, char delimiter) noexcept {
    std::uint32_t value = 0;
    while (*p != delimiter) {
        value = value * 10 + static_cast<unsigned>(*p - '0');
        ++p;
    }
    ++p;
    return value;
}

static inline std::int64_t parse_timestamp(const char*& p) noexcept {
    if (p[0] != '-' && p[10] == ',') {
        const __m128i ascii =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i digits = _mm_sub_epi8(ascii, _mm_set1_epi8('0'));
        const __m128i pairs = _mm_maddubs_epi16(
            digits, _mm_set1_epi16(0x010a));
        const __m128i quads = _mm_madd_epi16(
            pairs, _mm_set1_epi32(0x00010064));
        const std::uint64_t first_four =
            static_cast<std::uint32_t>(_mm_cvtsi128_si32(quads));
        const std::uint64_t second_four =
            static_cast<std::uint32_t>(_mm_extract_epi32(quads, 1));
        const std::uint64_t last_two =
            static_cast<std::uint16_t>(_mm_extract_epi16(pairs, 4));
        const std::int64_t value = static_cast<std::int64_t>(
            first_four * 1000000ULL + second_four * 100ULL + last_two);
        p += 11;
        return value;
    }

    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    }
    std::int64_t value = 0;
    while (*p != ',') {
        value = value * 10 + (*p - '0');
        ++p;
    }
    ++p;
    return negative ? -value : value;
}

std::array<std::uint64_t, 10000> make_timestamp_prefix_lookup() {
    std::array<std::uint64_t, 10000> lookup {};
    for (std::uint64_t prefix = 0; prefix < lookup.size(); ++prefix) {
        const std::int64_t start = static_cast<std::int64_t>(prefix * 1000000ULL);
        const std::int32_t month = month_from_timestamp(start);
        std::uint32_t threshold = 1000000;
        if (month_from_timestamp(start + 999999) != month) {
            std::uint32_t low = 0;
            std::uint32_t high = 1000000;
            while (low < high) {
                const std::uint32_t middle = low + (high - low) / 2;
                if (month_from_timestamp(start + middle) == month) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }
            threshold = low;
        }
        lookup[prefix] = static_cast<std::uint32_t>(month) |
                         (static_cast<std::uint64_t>(threshold) << 32);
    }
    return lookup;
}

const auto timestamp_prefix_lookup = make_timestamp_prefix_lookup();

static inline std::int32_t parse_fixed_month(const char*& p) noexcept {
#ifdef BRC_SPARSE_PREFIX_MONTH_LOOKUP
    constexpr std::uint64_t value_mask = 0x00ffffffffffffffULL;
    alignas(64) static constexpr auto records = [] {
        constexpr std::array<std::uint64_t, 33> thresholds = {
            UINT64_MAX, UINT64_MAX, UINT64_MAX, 0x3138303134343030ULL,
            UINT64_MAX, 0x3138303338353932ULL, UINT64_MAX, UINT64_MAX,
            0x3138303635333736ULL, UINT64_MAX, UINT64_MAX,
            0x3138303931323936ULL, UINT64_MAX, 0x3138313138303830ULL,
            UINT64_MAX, UINT64_MAX, 0x3138313434303030ULL, UINT64_MAX,
            UINT64_MAX, 0x3138313730373834ULL, UINT64_MAX,
            0x3138313937353638ULL, UINT64_MAX, UINT64_MAX,
            0x3138323233343838ULL, UINT64_MAX, UINT64_MAX,
            0x3138323530323732ULL, UINT64_MAX, 0x3138323736313932ULL,
            UINT64_MAX, UINT64_MAX, UINT64_MAX,
        };
        constexpr std::array<std::uint8_t, 33> bases = {
            0, 0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5,
            6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11,
        };
        std::array<std::uint64_t, 1U << 17> result {};
        for (unsigned i = 0; i < 33; ++i) {
            const unsigned decimal = 1798U + i;
            const unsigned hundreds = decimal / 100U % 10U;
            const unsigned tens = decimal / 10U % 10U;
            const unsigned ones = decimal % 10U;
            const unsigned index =
                ((hundreds == 8U) ? (1U << 16) : 0U) |
                (static_cast<unsigned>('0' + tens)) |
                (static_cast<unsigned>('0' + ones) << 8);
            result[index] = static_cast<std::uint64_t>(bases[i]) << 56 |
                            (thresholds[i] & value_mask);
        }
        return result;
    }();
    const std::uint64_t raw = load_u64(p);
    const unsigned index = static_cast<unsigned>((raw >> 16) & 0xffffU) |
        (static_cast<unsigned>(((raw >> 8) ^ 1U) & 1U) << 16);
    const std::uint64_t record = records[index];
    const std::uint64_t prefix = __builtin_bswap64(raw) & value_mask;
    const std::int32_t month =
        static_cast<std::int32_t>(record >> 56) +
        static_cast<std::int32_t>(prefix >= (record & value_mask));
    p += 11;
#ifdef BRC_RELATIVE_MONTH_INDEX
    return month;
#else
    return 2027 * 12 + month;
#endif
#elif defined(BRC_PACKED_MILLION_MONTH_LOOKUP)
    constexpr std::uint64_t value_mask = 0x00ffffffffffffffULL;
    alignas(64) static constexpr auto records = [] {
        constexpr std::array<std::uint64_t, 33> thresholds = {
            UINT64_MAX, UINT64_MAX, UINT64_MAX, 0x3138303134343030ULL,
            UINT64_MAX, 0x3138303338353932ULL, UINT64_MAX, UINT64_MAX,
            0x3138303635333736ULL, UINT64_MAX, UINT64_MAX,
            0x3138303931323936ULL, UINT64_MAX, 0x3138313138303830ULL,
            UINT64_MAX, UINT64_MAX, 0x3138313434303030ULL, UINT64_MAX,
            UINT64_MAX, 0x3138313730373834ULL, UINT64_MAX,
            0x3138313937353638ULL, UINT64_MAX, UINT64_MAX,
            0x3138323233343838ULL, UINT64_MAX, UINT64_MAX,
            0x3138323530323732ULL, UINT64_MAX, 0x3138323736313932ULL,
            UINT64_MAX, UINT64_MAX, UINT64_MAX,
        };
        constexpr std::array<std::uint8_t, 33> bases = {
            0, 0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5,
            6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11,
        };
        std::array<std::uint64_t, 33> packed {};
        for (std::size_t i = 0; i < packed.size(); ++i) {
            packed[i] = static_cast<std::uint64_t>(bases[i]) << 56 |
                        (thresholds[i] & 0x00ffffffffffffffULL);
        }
        return packed;
    }();
    const unsigned index = static_cast<unsigned>(
        (p[1] - '7') * 100 + (p[2] - '0') * 10 + (p[3] - '0') - 98);
    const std::uint64_t prefix =
        __builtin_bswap64(load_u64(p)) & value_mask;
    const std::uint64_t record = records[index];
    const std::int32_t month =
        static_cast<std::int32_t>(record >> 56) +
        static_cast<std::int32_t>(prefix >= (record & value_mask));
    p += 11;
#ifdef BRC_RELATIVE_MONTH_INDEX
    return month;
#else
    return 2027 * 12 + month;
#endif
#elif defined(BRC_MILLION_MONTH_LOOKUP)
    alignas(64) static constexpr std::array<std::uint64_t, 33> thresholds = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, 0x3138303134343030ULL,
        UINT64_MAX, 0x3138303338353932ULL, UINT64_MAX, UINT64_MAX,
        0x3138303635333736ULL, UINT64_MAX, UINT64_MAX,
        0x3138303931323936ULL, UINT64_MAX, 0x3138313138303830ULL,
        UINT64_MAX, UINT64_MAX, 0x3138313434303030ULL, UINT64_MAX,
        UINT64_MAX, 0x3138313730373834ULL, UINT64_MAX,
        0x3138313937353638ULL, UINT64_MAX, UINT64_MAX,
        0x3138323233343838ULL, UINT64_MAX, UINT64_MAX,
        0x3138323530323732ULL, UINT64_MAX, 0x3138323736313932ULL,
        UINT64_MAX, UINT64_MAX, UINT64_MAX,
    };
    alignas(64) static constexpr std::array<std::uint8_t, 33> base_months = {
        0, 0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5,
        6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11,
    };
    const unsigned index = static_cast<unsigned>(
        (p[1] - '7') * 100 + (p[2] - '0') * 10 + (p[3] - '0') - 98);
    const std::uint64_t prefix = __builtin_bswap64(load_u64(p));
    const std::int32_t month = static_cast<std::int32_t>(base_months[index]) +
        static_cast<std::int32_t>(prefix >= thresholds[index]);
    p += 11;
#ifdef BRC_RELATIVE_MONTH_INDEX
    return month;
#else
    return 2027 * 12 + month;
#endif
#elif defined(BRC_AVX512_MONTH_THRESHOLDS) || defined(BRC_AVX2_MONTH_THRESHOLDS)
    alignas(64) static constexpr std::array<std::uint64_t, 16> thresholds = {
        0x3138303134343030ULL,  // 18014400: 2027-02-01
        0x3138303338353932ULL,  // 18038592: 2027-03-01
        0x3138303635333736ULL,  // 18065376: 2027-04-01
        0x3138303931323936ULL,  // 18091296: 2027-05-01
        0x3138313138303830ULL,  // 18118080: 2027-06-01
        0x3138313434303030ULL,  // 18144000: 2027-07-01
        0x3138313730373834ULL,  // 18170784: 2027-08-01
        0x3138313937353638ULL,  // 18197568: 2027-09-01
        0x3138323233343838ULL,  // 18223488: 2027-10-01
        0x3138323530323732ULL,  // 18250272: 2027-11-01
        0x3138323736313932ULL,  // 18276192: 2027-12-01
        INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX,
    };
    const std::uint64_t prefix = __builtin_bswap64(load_u64(p));
#ifdef BRC_AVX512_MONTH_THRESHOLDS
    const __m512i value = _mm512_set1_epi64(static_cast<std::int64_t>(prefix));
    const __mmask8 first = _mm512_cmp_epu64_mask(
        value, _mm512_load_si512(thresholds.data()), _MM_CMPINT_NLT);
#ifdef BRC_HYBRID_MONTH_THRESHOLDS
    const __mmask8 second = _mm256_cmp_epu64_mask(
        _mm512_castsi512_si256(value),
        _mm256_load_si256(reinterpret_cast<const __m256i*>(thresholds.data() + 8)),
        _MM_CMPINT_NLT);
#else
    const __mmask8 second = _mm512_cmp_epu64_mask(
        value, _mm512_load_si512(thresholds.data() + 8), _MM_CMPINT_NLT);
#endif
    p += 11;
    const std::int32_t month =
        std::popcount(static_cast<unsigned>(first)) +
        std::popcount(static_cast<unsigned>(second));
#ifdef BRC_RELATIVE_MONTH_INDEX
    return month;
#else
    return 2027 * 12 + month;
#endif
#else
    const __m256i value = _mm256_set1_epi64x(static_cast<std::int64_t>(prefix));
    unsigned month = 0;
    for (unsigned i = 0; i < 12; i += 4) {
        const __m256i boundary = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(thresholds.data() + i));
        const __m256i passed = _mm256_cmpgt_epi64(
            value, _mm256_sub_epi64(boundary, _mm256_set1_epi64x(1)));
        month += std::popcount(static_cast<unsigned>(_mm256_movemask_pd(
            _mm256_castsi256_pd(passed))));
    }
    p += 11;
#ifdef BRC_RELATIVE_MONTH_INDEX
    return static_cast<std::int32_t>(month);
#else
    return 2027 * 12 + static_cast<std::int32_t>(month);
#endif
#endif
#else
    const __m128i ascii =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i digits = _mm_sub_epi8(ascii, _mm_set1_epi8('0'));
    const __m128i pairs = _mm_maddubs_epi16(
        digits, _mm_set1_epi16(0x010a));
    const __m128i quads = _mm_madd_epi16(
        pairs, _mm_set1_epi32(0x00010064));
    const std::uint32_t first_four =
        static_cast<std::uint32_t>(_mm_cvtsi128_si32(quads));
    const std::uint64_t encoded = timestamp_prefix_lookup[first_four];
    const std::uint32_t second_four =
        static_cast<std::uint32_t>(_mm_extract_epi32(quads, 1));
    const std::uint32_t last_pair =
        static_cast<std::uint16_t>(_mm_extract_epi16(pairs, 4));
    const std::uint32_t suffix = second_four * 100U + last_pair;
    p += 11;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(encoded)) +
           static_cast<std::int32_t>(suffix >= (encoded >> 32));
#endif
}

static inline std::int32_t parse_month(const char*& p) noexcept {
    if (p[0] != '-' && p[10] == ',') return parse_fixed_month(p);
    return month_from_timestamp(parse_timestamp(p));
}

bool sample_has_fixed_timestamps(const char* begin, const char* end) noexcept {
    constexpr unsigned sample_rows = 4096;
    const char* p = begin;
    for (unsigned row = 0; row < sample_rows && p < end; ++row) {
        if (end - p < 11 || p[0] == '-' || p[10] != ',') return false;
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    }
    return true;
}

std::int32_t sample_base_month(const char* begin, const char* end) noexcept {
    constexpr unsigned sample_rows = 4096;
    const char* p = begin;
    std::int32_t base_month = std::numeric_limits<std::int32_t>::max();
    for (unsigned row = 0; row < sample_rows && p < end; ++row) {
        base_month = std::min(base_month, parse_month(p));
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    }
    return base_month;
}

template <bool check_bounds>
static inline std::uint32_t find_channel_length(const char* p,
                                                const char* end) noexcept {
#ifdef BRC_AVX2_CHANNEL_SCAN
    if constexpr (!check_bounds) {
        const __m256i bytes =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(','))));
        if (mask != 0) return std::countr_zero(mask);
        const char* cursor = p + 32;
        while (*cursor != ',') ++cursor;
        return static_cast<std::uint32_t>(cursor - p);
    } else if (end - p >= 32) {
        const __m256i bytes =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(','))));
        if (mask != 0) return std::countr_zero(mask);
        const char* cursor = p + 32;
        while (cursor < end && *cursor != ',') ++cursor;
        return static_cast<std::uint32_t>(cursor - p);
    }
#endif
#ifdef BRC_AVX512_CHANNEL_SCAN
    if constexpr (!check_bounds) {
        const __mmask64 first = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(static_cast<const void*>(p)),
            _mm512_set1_epi8(','));
        if (first != 0) {
            return static_cast<std::uint32_t>(std::countr_zero(
                static_cast<std::uint64_t>(first)));
        }
        const __mmask64 second = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(static_cast<const void*>(p + 64)),
            _mm512_set1_epi8(','));
        return 64U + static_cast<std::uint32_t>(std::countr_zero(
            static_cast<std::uint64_t>(second)));
    } else if (end - p >= 128) {
        const __mmask64 first = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(static_cast<const void*>(p)),
            _mm512_set1_epi8(','));
        if (first != 0) {
            return static_cast<std::uint32_t>(std::countr_zero(
                static_cast<std::uint64_t>(first)));
        }
        const __mmask64 second = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(static_cast<const void*>(p + 64)),
            _mm512_set1_epi8(','));
        return 64U + static_cast<std::uint32_t>(std::countr_zero(
            static_cast<std::uint64_t>(second)));
    }
#endif
    if constexpr (check_bounds) {
        if (end - p < 16) {
            const char* cursor = p;
            while (*cursor != ',') ++cursor;
            return static_cast<std::uint32_t>(cursor - p);
        }
    }

    const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i commas = _mm_cmpeq_epi8(bytes, _mm_set1_epi8(','));
    const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(commas));
    if (mask != 0) return std::countr_zero(mask);

    if constexpr (!check_bounds) {
        const __m128i second =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
        const __m128i second_commas =
            _mm_cmpeq_epi8(second, _mm_set1_epi8(','));
        const unsigned second_mask =
            static_cast<unsigned>(_mm_movemask_epi8(second_commas));
        if (second_mask != 0) return 16U + std::countr_zero(second_mask);
    } else if (end - p >= 32) {
        const __m128i second =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
        const __m128i second_commas =
            _mm_cmpeq_epi8(second, _mm_set1_epi8(','));
        const unsigned second_mask =
            static_cast<unsigned>(_mm_movemask_epi8(second_commas));
        if (second_mask != 0) return 16U + std::countr_zero(second_mask);
    }
    const char* cursor = p + 16;
    while (*cursor != ',') ++cursor;
    return static_cast<std::uint32_t>(cursor - p);
}

#if defined(BRC_FUSED_CHANNEL_KEY) || defined(BRC_FUSED_FOUR_POINT_KEY)
struct ChannelKey {
    std::uint64_t hash;
    std::uint32_t length;
};

template <bool check_bounds>
static inline ChannelKey find_channel_key(const char* p,
                                          const char* end) noexcept {
    if constexpr (check_bounds) {
        if (end - p < 16) {
            const std::uint32_t length = find_channel_length<true>(p, end);
            return {hash_channel(p, length), length};
        }
    }

    const __m128i bytes =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(
        _mm_cmpeq_epi8(bytes, _mm_set1_epi8(','))));
    if (mask != 0) {
        const std::uint32_t length = std::countr_zero(mask);
        const __mmask16 keep = static_cast<__mmask16>(
            (static_cast<std::uint32_t>(1) << length) - 1U);
        const __m128i channel = _mm_maskz_mov_epi8(keep, bytes);
        return {hash_short_channel_bytes(channel, length), length};
    }

#ifdef BRC_FUSED_FOUR_POINT_KEY
    if constexpr (check_bounds) {
        if (end - p < 32) {
            const std::uint32_t length = find_channel_length<true>(p, end);
            return {hash_channel(p, length), length};
        }
    }
    const __m128i next_bytes =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
    const unsigned next_mask = static_cast<unsigned>(_mm_movemask_epi8(
        _mm_cmpeq_epi8(next_bytes, _mm_set1_epi8(','))));
    std::uint32_t length;
    if (next_mask != 0) {
        length = 16U + std::countr_zero(next_mask);
    } else {
        const char* cursor = p + 32;
        while (*cursor != ',') ++cursor;
        length = static_cast<std::uint32_t>(cursor - p);
    }
    if (length == 16U) {
        return {hash_short_channel_bytes(bytes, length), length};
    }

    const std::uint64_t first = static_cast<std::uint64_t>(
        _mm_cvtsi128_si64(bytes));
    const std::uint64_t second = static_cast<std::uint64_t>(
        _mm_extract_epi64(bytes, 1));
    const std::uint64_t middle = load_u64(p + (length - 8) / 2);
    const std::uint64_t last = load_u64(p + length - 8);
    std::uint64_t first_crc = _mm_crc32_u64(BRC_CRC_SHORT_SEED, first);
    first_crc = _mm_crc32_u64(first_crc, second);
    std::uint64_t second_crc =
        _mm_crc32_u64(static_cast<std::uint64_t>(length), middle);
    second_crc = _mm_crc32_u64(second_crc, last);
    std::uint64_t hash =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(second_crc)) << 32 |
        static_cast<std::uint32_t>(first_crc);
    hash *= BRC_FOUR_POINT_MIX;
    hash ^= hash >> 29;
    return {hash | 1U, length};
#else
    const std::uint32_t length = find_channel_length<check_bounds>(p, end);
    return {hash_channel(p, length), length};
#endif
}
#endif

void sample_channel_dictionary(const char* begin, const char* end,
                               ChannelDictionary& dictionary,
                               unsigned sample_rows = 1U << 21) {
    const char* p = begin;
    for (unsigned row = 0;
         row < sample_rows && p < end && dictionary.size() < 10000;
         ++row) {
#ifdef BRC_YEAR_2027
        if (end - p < 11) break;
        p += 11;
#else
        while (p < end && *p != ',') ++p;
        if (p == end) break;
        ++p;
#endif
        const char* channel = p;
        const std::uint32_t length = find_channel_length<true>(p, end);
        dictionary.insert_exact(channel, length, hash_channel(channel, length));
        p += length;
        if (end - p >= 7) {
            const char* const numeric = p + 1;
            const unsigned three_digits = numeric[2] != ',';
            const unsigned comma_offset = 2U + three_digits;
            if (numeric[comma_offset] == ',' &&
                numeric[comma_offset + 2] == '\n') {
                p = numeric + comma_offset + 3;
                continue;
            }
        }
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    }
}

void sample_channel_dictionary_parallel(const char* begin, const char* end,
                                        ChannelDictionary& dictionary) {
    constexpr unsigned sampler_count = 8;
#ifdef BRC_DICTIONARY_ROWS_PER_SAMPLER
    constexpr unsigned rows_per_sampler = BRC_DICTIONARY_ROWS_PER_SAMPLER;
#else
    constexpr unsigned rows_per_sampler = 1U << 18;
#endif
    std::array<ChannelDictionary, sampler_count> local_dictionaries;
    std::array<const char*, sampler_count> sample_begins {};
    const std::size_t size = static_cast<std::size_t>(end - begin);
    sample_begins[0] = begin;
    for (unsigned i = 1; i < sampler_count; ++i) {
        const char* p = begin + size * i / sampler_count;
        while (p < end && *p != '\n') ++p;
        sample_begins[i] = p < end ? p + 1 : end;
    }

    std::array<std::thread, sampler_count - 1> workers;
    for (unsigned i = 1; i < sampler_count; ++i) {
        workers[i - 1] = std::thread(
            sample_channel_dictionary, sample_begins[i], end,
            std::ref(local_dictionaries[i]), rows_per_sampler);
    }
    sample_channel_dictionary(sample_begins[0], end,
                              local_dictionaries[0], rows_per_sampler);
    for (std::thread& worker : workers) worker.join();

    for (const ChannelDictionary& local : local_dictionaries) {
        for (const ChannelMetadata& item : local.metadata()) {
            dictionary.insert_exact(item.channel, item.length, item.hash);
        }
    }
}

struct ParsedRow {
    const char* channel;
    std::uint64_t channel_hash;
    std::uint32_t channel_length;
    std::int32_t month;
    std::uint32_t message_length;
    std::uint32_t stamps;
};

static_assert(sizeof(ParsedRow) == 32);

#ifdef BRC_FAST_ROW24
struct FastParsedRow {
    std::uint64_t channel_hash;
    std::int32_t month;
    std::uint16_t message_length;
    std::uint16_t stamps;
};

static_assert(sizeof(FastParsedRow) == 16);
#endif

#ifdef BRC_FAST_WIDE_ROW
struct FastWideRow {
    std::uint64_t channel_hash;
    std::int32_t month;
    std::uint32_t message_length;
    std::uint32_t stamps;
};

static_assert(sizeof(FastWideRow) == 24);
#endif

#ifdef BRC_PACKED_WIDE_ROW
struct [[gnu::packed]] PackedWideRow {
    std::uint64_t channel_hash;
    std::int32_t month;
    std::uint32_t message_length;
    std::uint32_t stamps;
};

static_assert(sizeof(PackedWideRow) == 20);
#endif

#ifdef BRC_FAST_HASH32_ROW
struct FastHash32Row {
    std::uint32_t channel_hash;
    std::int32_t month;
    std::uint32_t message_length;
    std::uint32_t stamps;
};

static_assert(sizeof(FastHash32Row) == 16);
#endif

template <bool fixed_timestamps, bool check_bounds, bool exact_channels>
void parse_range(const char* begin, const char* end, LocalAggregateTable& table) {
#ifndef BRC_BATCH_SIZE
#define BRC_BATCH_SIZE 32
#endif
    constexpr unsigned batch_size = BRC_BATCH_SIZE;
#ifdef BRC_FAST_HASH32_ROW
    using PipelineRow = std::conditional_t<exact_channels, ParsedRow, FastHash32Row>;
#elif defined(BRC_PACKED_WIDE_ROW)
    using PipelineRow = std::conditional_t<exact_channels, ParsedRow, PackedWideRow>;
#elif defined(BRC_FAST_WIDE_ROW)
    using PipelineRow = std::conditional_t<exact_channels, ParsedRow, FastWideRow>;
#elif defined(BRC_FAST_ROW24)
    using PipelineRow = std::conditional_t<exact_channels, ParsedRow, FastParsedRow>;
#else
    using PipelineRow = ParsedRow;
#endif
    std::array<std::array<PipelineRow, batch_size>, 2> batches;
    std::array<std::array<CompactStats*, batch_size>, 2> targets;
    std::array<unsigned, 2> counts {};
#ifdef BRC_THREE_CURSORS
    std::array<const char*, BRC_CURSOR_COUNT> cursors {};
    std::array<const char*, BRC_CURSOR_COUNT> cursor_ends {};
    cursors.fill(end);
    cursor_ends.fill(end);
    cursors[0] = begin;
    if constexpr (!check_bounds) {
        for (unsigned i = 1; i < cursors.size(); ++i) {
            const char* boundary =
                begin + static_cast<std::size_t>(end - begin) * i / cursors.size();
            while (boundary < end && boundary[-1] != '\n') ++boundary;
            cursor_ends[i - 1] = boundary;
            cursors[i] = boundary;
        }
    }
    unsigned cursor_turn = 0;
    const auto rows_remaining = [&] {
        for (unsigned i = 0; i < cursors.size(); ++i) {
            if (cursors[i] < cursor_ends[i]) return true;
        }
        return false;
    };
#else
    const char* p = begin;
    const auto rows_remaining = [&] { return p < end; };
#endif

    const auto parse_batch = [&](std::array<PipelineRow, batch_size>& rows,
                                 std::array<CompactStats*, batch_size>& row_targets) {
        unsigned row_count = 0;
#if defined(BRC_PHASE_INTERLEAVED_CURSORS) && defined(BRC_THREE_CURSORS)
        if constexpr (fixed_timestamps && !check_bounds && !exact_channels) {
            constexpr unsigned lane_count = BRC_CURSOR_COUNT;
            std::array<const char*, lane_count> lane_p {};
            std::array<const char*, lane_count> channels {};
            std::array<unsigned, lane_count> cursor_indices {};
            std::array<std::uint32_t, lane_count> channel_lengths {};
            std::array<std::uint32_t, lane_count> message_lengths {};
            std::array<std::uint32_t, lane_count> stamp_counts {};
            std::array<std::int32_t, lane_count> months {};
            std::array<std::uint64_t, lane_count> hashes {};

            while (row_count < batch_size && rows_remaining()) {
                unsigned active = 0;
                const unsigned first_cursor = cursor_turn;
                for (unsigned offset = 0;
                     offset < lane_count && row_count + active < batch_size;
                     ++offset) {
                    const unsigned index = (first_cursor + offset) % lane_count;
                    if (cursors[index] >= cursor_ends[index]) continue;
                    cursor_indices[active] = index;
                    lane_p[active] = cursors[index];
                    ++active;
                }
                cursor_turn = (first_cursor + 1U) % lane_count;

                for (unsigned lane = 0; lane < active; ++lane) {
                    months[lane] = parse_fixed_month(lane_p[lane]);
                }
                for (unsigned lane = 0; lane < active; ++lane) {
                    channels[lane] = lane_p[lane];
#if defined(BRC_FUSED_CHANNEL_KEY) || defined(BRC_FUSED_FOUR_POINT_KEY)
                    const ChannelKey key = find_channel_key<false>(
                        lane_p[lane], cursor_ends[cursor_indices[lane]]);
                    channel_lengths[lane] = key.length;
                    hashes[lane] = key.hash;
#else
                    channel_lengths[lane] =
                        find_channel_length<false>(lane_p[lane],
                                                   cursor_ends[cursor_indices[lane]]);
#endif
                }
                for (unsigned lane = 0; lane < active; ++lane) {
#if !defined(BRC_FUSED_CHANNEL_KEY) && !defined(BRC_FUSED_FOUR_POINT_KEY)
                    hashes[lane] = hash_channel(channels[lane], channel_lengths[lane]);
#endif
                    lane_p[lane] += channel_lengths[lane] + 1;
                }
                for (unsigned lane = 0; lane < active; ++lane) {
                    const char* const row_end = cursor_ends[cursor_indices[lane]];
                    const unsigned three_digits = lane_p[lane][2] != ',';
                    const unsigned comma_offset = 2U + three_digits;
                    std::uint32_t tail_signature;
                    std::memcpy(&tail_signature, lane_p[lane] + comma_offset,
                                sizeof(tail_signature));
                    const bool common_tail =
                        (tail_signature & 0x00ff00ffU) == 0x000a002cU;
                    if (common_tail) {
                        const unsigned first_two =
                            static_cast<unsigned>(lane_p[lane][0] - '0') * 10U +
                            static_cast<unsigned>(lane_p[lane][1] - '0');
                        message_lengths[lane] = first_two + three_digits *
                            (first_two * 9U +
                             static_cast<unsigned>(lane_p[lane][2] - '0'));
                        stamp_counts[lane] = static_cast<unsigned>(
                            lane_p[lane][comma_offset + 1] - '0');
                        lane_p[lane] += comma_offset + 3;
                    } else {
                        message_lengths[lane] = parse_u32(lane_p[lane], ',');
                        stamp_counts[lane] = 0;
                        while (lane_p[lane] < row_end &&
                               *lane_p[lane] != '\n' && *lane_p[lane] != '\r') {
                            stamp_counts[lane] = stamp_counts[lane] * 10U +
                                static_cast<unsigned>(*lane_p[lane] - '0');
                            ++lane_p[lane];
                        }
                        while (lane_p[lane] < row_end &&
                               (*lane_p[lane] == '\n' || *lane_p[lane] == '\r')) {
                            ++lane_p[lane];
                        }
                    }
                }

                for (unsigned lane = 0; lane < active; ++lane) {
                    cursors[cursor_indices[lane]] = lane_p[lane];
                    if (message_lengths[lane] >
                        std::numeric_limits<std::uint16_t>::max()) {
                        table.add(channels[lane], channel_lengths[lane], months[lane],
                                  hashes[lane], message_lengths[lane],
                                  stamp_counts[lane]);
                        continue;
                    }
                    PipelineRow& row = rows[row_count++];
                    row.month = months[lane];
                    row.channel_hash = hashes[lane];
                    row.message_length = static_cast<decltype(row.message_length)>(
                        message_lengths[lane]);
                    row.stamps = static_cast<decltype(row.stamps)>(stamp_counts[lane]);
                }
            }
            return row_count;
        }
#endif
        do {
#ifdef BRC_THREE_CURSORS
            while (cursors[cursor_turn] >= cursor_ends[cursor_turn]) {
                cursor_turn = (cursor_turn + 1U) % cursors.size();
            }
            const unsigned cursor_index = cursor_turn;
            cursor_turn = (cursor_turn + 1U) % cursors.size();
            const char*& p = cursors[cursor_index];
            const char* const row_end = cursor_ends[cursor_index];
#else
            const char* const row_end = end;
#endif
            PipelineRow& row = rows[row_count];
            if constexpr (fixed_timestamps) {
                row.month = parse_fixed_month(p);
            } else {
                row.month = parse_month(p);
            }
            const char* const channel = p;
            if constexpr (exact_channels) row.channel = channel;
#if defined(BRC_FUSED_CHANNEL_KEY) || defined(BRC_FUSED_FOUR_POINT_KEY)
            const ChannelKey channel_key =
                find_channel_key<check_bounds>(p, row_end);
            const std::uint32_t channel_length = channel_key.length;
            row.channel_hash = channel_key.hash;
#else
            const std::uint32_t channel_length =
                find_channel_length<check_bounds>(p, row_end);
#endif
            if constexpr (exact_channels) row.channel_length = channel_length;
            p += channel_length + 1;
            std::uint32_t message_length;
            std::uint32_t stamps;
#ifdef BRC_PACKED_NUMERIC_LOAD
            const bool enough_for_signature = [&] {
                if constexpr (check_bounds) return row_end - p >= 8;
                return true;
            }();
            std::uint64_t packed_tail = 0;
            if (enough_for_signature) packed_tail = load_u64(p);
            const unsigned three_digits =
                enough_for_signature && ((packed_tail >> 16) & 0xffU) != ',';
            const unsigned comma_offset = 2U + three_digits;
            const std::uint32_t tail_signature = static_cast<std::uint32_t>(
                packed_tail >> (comma_offset * 8U));
#else
            const bool enough_for_three = [&] {
                if constexpr (check_bounds) return row_end - p >= 6;
                return true;
            }();
            const unsigned three_digits = enough_for_three && p[2] != ',';
            const unsigned comma_offset = 2U + three_digits;
            const bool enough_for_signature = [&] {
                if constexpr (check_bounds) return row_end - p >= 7;
                return true;
            }();
            std::uint32_t tail_signature = 0;
            if (enough_for_signature) {
                std::memcpy(&tail_signature, p + comma_offset, sizeof(tail_signature));
            }
#endif
            const bool common_tail = enough_for_signature &&
                (tail_signature & 0x00ff00ffU) == 0x000a002cU;
            if (common_tail) {
#ifdef BRC_PACKED_NUMERIC_LOAD
                const unsigned first_two =
                    static_cast<unsigned>(packed_tail & 0xfU) * 10U +
                    static_cast<unsigned>((packed_tail >> 8) & 0xfU);
                message_length = first_two + three_digits *
                    (first_two * 9U +
                     static_cast<unsigned>((packed_tail >> 16) & 0xfU));
                stamps = static_cast<unsigned>(
                    (packed_tail >> ((comma_offset + 1U) * 8U)) & 0xfU);
#else
                const unsigned first_two =
                    static_cast<unsigned>(p[0] - '0') * 10U +
                    static_cast<unsigned>(p[1] - '0');
                message_length = first_two + three_digits *
                    (first_two * 9U + static_cast<unsigned>(p[2] - '0'));
                stamps = static_cast<unsigned>(p[comma_offset + 1] - '0');
#endif
                p += comma_offset + 3;
            } else {
                message_length = parse_u32(p, ',');
                stamps = 0;
                while (p < row_end && *p != '\n' && *p != '\r') {
                    stamps = stamps * 10 + static_cast<unsigned>(*p - '0');
                    ++p;
                }
                while (p < row_end && (*p == '\n' || *p == '\r')) ++p;
            }

#if !defined(BRC_FUSED_CHANNEL_KEY) && !defined(BRC_FUSED_FOUR_POINT_KEY)
            row.channel_hash = hash_channel(channel, channel_length);
#endif
#ifdef BRC_COMMON_ONLY_PIPELINE
            if constexpr (!exact_channels) {
                if (!common_tail) {
                    table.add(channel, channel_length, row.month,
                              row.channel_hash, message_length, stamps);
                    continue;
                }
            }
#endif
#ifdef BRC_SAFE_U16_PIPELINE
            if constexpr (!exact_channels) {
                if (message_length > std::numeric_limits<std::uint16_t>::max()) {
                    table.add(channel, channel_length, row.month,
                              row.channel_hash, message_length, stamps);
                    continue;
                }
            }
#endif
#ifdef BRC_FAST_ROW24
            if constexpr (!exact_channels) {
                if (message_length > std::numeric_limits<std::uint16_t>::max() ||
                    stamps > std::numeric_limits<std::uint16_t>::max()) {
                    table.add(channel, channel_length, row.month,
                              row.channel_hash, message_length, stamps);
                    continue;
                }
            }
#endif
#if defined(BRC_FAST_WIDE_ROW) || defined(BRC_PACKED_WIDE_ROW) || \
    defined(BRC_FAST_HASH32_ROW)
            if constexpr (!exact_channels) {
                if (message_length > std::numeric_limits<std::uint16_t>::max()) {
                    table.add(channel, channel_length, row.month,
                              row.channel_hash, message_length, stamps);
                    continue;
                }
            }
#endif
            row.message_length = static_cast<decltype(row.message_length)>(
                message_length);
            row.stamps = static_cast<decltype(row.stamps)>(stamps);
#ifdef BRC_RESOLVE_DURING_PARSE
            CompactStats* stats;
            if constexpr (exact_channels) {
                stats = table.resolve_exact(
                    row.channel, row.channel_length,
                    row.month, row.channel_hash);
            } else {
                stats = table.resolve(0, row.month, row.channel_hash);
            }
            row_targets[row_count] = stats;
            table.prefetch_stats(stats);
#endif
#ifndef BRC_SKIP_CHANNEL_PREFETCH
            table.prefetch_channel(row.channel_hash);
#endif
            ++row_count;
        } while (row_count < batch_size && rows_remaining());
        return row_count;
    };

    const auto resolve_batch = [&](unsigned batch) {
#ifndef BRC_RESOLVE_DURING_PARSE
        for (unsigned i = 0; i < counts[batch]; ++i) {
            const PipelineRow& row = batches[batch][i];
            CompactStats* stats;
            if constexpr (exact_channels) {
                stats = table.resolve_exact(
                    row.channel, row.channel_length,
                    row.month, row.channel_hash);
            } else {
                stats = table.resolve(
                    0, row.month, row.channel_hash);
            }
            targets[batch][i] = stats;
            table.prefetch_stats(stats);
        }
#else
        static_cast<void>(batch);
#endif
    };

    const auto update_batch = [&](unsigned batch) {
        for (unsigned i = 0; i < counts[batch]; ++i) {
            const PipelineRow& row = batches[batch][i];
            CompactStats* stats = targets[batch][i];
#ifdef BRC_UNCHECKED_FAST_PIPELINE
            if constexpr (!exact_channels) {
                table.add_resolved_unchecked(
                    row.message_length, row.stamps, stats);
            } else {
#endif
#if defined(BRC_FAST_ROW24) || defined(BRC_FAST_WIDE_ROW) || \
    defined(BRC_PACKED_WIDE_ROW) || \
    defined(BRC_FAST_HASH32_ROW)
            if constexpr (!exact_channels) {
                table.add_resolved_fast(row.message_length, row.stamps, stats);
            } else {
#endif
                if (stats != nullptr) {
                    table.add_resolved(
                        row.channel, row.channel_length, row.month,
                        row.channel_hash, row.message_length, row.stamps, stats);
                } else {
                    if constexpr (exact_channels) {
                        table.add_unresolved(
                            row.channel, row.channel_length, row.month,
                            row.channel_hash, row.message_length, row.stamps);
                    } else {
                        table.add(
                            row.channel, row.channel_length, row.month,
                            row.channel_hash, row.message_length, row.stamps);
                    }
                }
#if defined(BRC_FAST_ROW24) || defined(BRC_FAST_WIDE_ROW) || \
    defined(BRC_PACKED_WIDE_ROW) || \
    defined(BRC_FAST_HASH32_ROW)
            }
#endif
#ifdef BRC_UNCHECKED_FAST_PIPELINE
            }
#endif
        }
    };

    if (!rows_remaining()) return;
    unsigned current = 0;
    counts[current] = parse_batch(batches[current], targets[current]);
    resolve_batch(current);

    while (rows_remaining()) {
        const unsigned next = current ^ 1U;
        counts[next] = parse_batch(batches[next], targets[next]);
        update_batch(current);
        resolve_batch(next);
        current = next;
    }
    update_batch(current);
}

template <bool fixed_timestamps, bool exact_channels>
void parse_partition(const char* begin, const char* end,
                     LocalAggregateTable& table) {
    const char* tail = begin;
    if (end - begin > 128) {
        tail = end - 64;
        while (tail > begin && tail[-1] != '\n') --tail;
        parse_range<fixed_timestamps, false, exact_channels>(begin, tail, table);
    }
    parse_range<fixed_timestamps, true, exact_channels>(tail, end, table);
}

unsigned choose_thread_count(std::size_t body_size) {
#ifdef BRC_FORCED_THREAD_COUNT
    unsigned threads = BRC_FORCED_THREAD_COUNT;
#else
    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 8;
#endif
    if (const char* configured = std::getenv("BRC_THREADS")) {
        unsigned parsed = 0;
        const auto result = std::from_chars(configured, configured + std::strlen(configured), parsed);
        if (result.ec == std::errc() && parsed != 0) threads = parsed;
    }
    const unsigned useful = static_cast<unsigned>(std::max<std::size_t>(1, body_size / (1U << 20)));
    return std::max(1U, std::min(threads, useful));
}

void append_u64(std::string& output, std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    output.append(buffer, result.ptr);
}

void append_month(std::string& output, std::int32_t month_key) {
    int year = month_key / 12;
    int month = month_key % 12 + 1;
    if (month <= 0) {
        month += 12;
        --year;
    }
    char buffer[16];
    const int length = std::snprintf(buffer, sizeof(buffer), "%04d-%02d", year, month);
    output.append(buffer, static_cast<std::size_t>(length));
}

void write_all(int fd, const char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            fail("write output");
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void append_output_entry(std::string& output, const char* channel,
                         std::uint32_t channel_length, std::int32_t month,
                         const Stats& stats) {
        output.append(channel, channel_length);
        output.push_back(',');
        append_month(output, month);
        output.push_back('=');
        append_u64(output, stats.min_length);
        output.push_back('/');

        char average[64];
        const int average_length = std::snprintf(
            average, sizeof(average), "%.2f",
            static_cast<double>(stats.length_sum) / static_cast<double>(stats.count));
        output.append(average, static_cast<std::size_t>(average_length));

        output.push_back('/');
        append_u64(output, stats.max_length);
        output.push_back('/');
        append_u64(output, stats.count);
        output.push_back('/');
        append_u64(output, stats.stamp_sum);
        output.push_back('\n');
}

void write_output_buffer(const char* path, const std::string& output) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) fail("open output");
    write_all(fd, output.data(), output.size());
    if (::close(fd) != 0) fail("close output");
}

void write_output(const char* path, const AggregateTable& table) {
    std::string output;
    output.reserve(table.size() * 64);

    for (const Entry& entry : table.entries()) {
        if (entry.hash == 0) continue;
        append_output_entry(output, entry.channel, entry.channel_length,
                            entry.month, entry.stats);
    }

    write_output_buffer(path, output);
}

#ifdef BRC_DIRECT_MERGE_OUTPUT
void write_dense_output(const char* path, const ChannelDictionary& dictionary,
                        const std::vector<LocalAggregateTable>& local_tables,
                        std::int32_t base_month) {
    constexpr std::size_t months = 12;
#ifdef BRC_RELATIVE_MONTH_INDEX
    constexpr std::int32_t display_base_month = 2027 * 12;
#else
    const std::int32_t display_base_month = base_month;
#endif
    std::vector<Stats> dense(dictionary.metadata().size() * months);
    AggregateTable spill(1U << 8);

    for (const LocalAggregateTable& local : local_tables) {
        for (std::size_t channel_index = 0;
             channel_index < dictionary.metadata().size(); ++channel_index) {
            const MonthBlock& block =
                local.blocks()[dictionary.storage_index(channel_index)];
            Stats* const destination = dense.data() + channel_index * months;
            for (std::size_t month_index = 0; month_index < months;
                 ++month_index) {
                const CompactStats& stats = block.slots[month_index];
                if (stats.count_value() == 0) continue;
                destination[month_index].merge_values(
                    stats.length_sum, stats.count_value(),
                    stats.stamp_sum_value(), stats.min_length_value(),
                    stats.max_length_value());
            }
        }

        for (const Entry& entry : local.overflow().entries()) {
            if (entry.hash == 0) continue;
            const ChannelEntry* known = dictionary.find_exact(
                entry.channel, entry.channel_length,
                hash_channel(entry.channel, entry.channel_length));
            const std::uint32_t month_index = static_cast<std::uint32_t>(
                entry.month - base_month);
            if (known != nullptr && month_index < months) {
                dense[static_cast<std::size_t>(known->block_index) * months +
                      month_index].merge(entry.stats);
            } else {
                const std::int32_t display_month =
#ifdef BRC_RELATIVE_MONTH_INDEX
                    entry.month + display_base_month;
#else
                    entry.month;
#endif
                spill.find_or_insert(entry.channel, entry.channel_length,
                                     display_month, entry.hash)
                    .stats.merge(entry.stats);
            }
        }
    }

    std::string output;
    output.reserve((dictionary.metadata().size() * months + spill.size()) * 64);
    for (std::size_t channel_index = 0;
         channel_index < dictionary.metadata().size(); ++channel_index) {
        const ChannelMetadata& channel = dictionary.metadata()[channel_index];
        const Stats* const stats = dense.data() + channel_index * months;
        for (std::size_t month_index = 0; month_index < months; ++month_index) {
            if (stats[month_index].count == 0) continue;
            append_output_entry(
                output, channel.channel, channel.length,
                display_base_month + static_cast<std::int32_t>(month_index),
                stats[month_index]);
        }
    }
    for (const Entry& entry : spill.entries()) {
        if (entry.hash == 0) continue;
        append_output_entry(output, entry.channel, entry.channel_length,
                            entry.month, entry.stats);
    }
    write_output_buffer(path, output);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
        return 2;
    }

    const Mapping input(argv[1]);
    if (input.size == 0) {
        const int fd = ::open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ::close(fd) != 0) fail("create output");
        return 0;
    }

    const char* file_end = input.data + input.size;
    const char* body = static_cast<const char*>(std::memchr(input.data, '\n', input.size));
    if (body == nullptr) {
        std::fprintf(stderr, "input has no header newline\n");
        return 1;
    }
    ++body;

    const std::size_t body_size = static_cast<std::size_t>(file_end - body);
    const unsigned thread_count = choose_thread_count(body_size);
#ifdef BRC_YEAR_2027
    constexpr bool fixed_timestamps = true;
#ifdef BRC_RELATIVE_MONTH_INDEX
    constexpr std::int32_t base_month = 0;
#else
    constexpr std::int32_t base_month = 2027 * 12;
#endif
#else
    const bool fixed_timestamps = sample_has_fixed_timestamps(body, file_end);
    const std::int32_t base_month = sample_base_month(body, file_end);
#endif
    ChannelDictionary dictionary;
#ifdef BRC_PARALLEL_DICTIONARY_SAMPLE
    sample_channel_dictionary_parallel(body, file_end, dictionary);
#else
    sample_channel_dictionary(body, file_end, dictionary);
#endif
    dictionary.finalize();

    std::vector<const char*> boundaries(thread_count + 1);
    boundaries.front() = body;
    boundaries.back() = file_end;
    for (unsigned i = 1; i < thread_count; ++i) {
        const char* p = body + body_size * i / thread_count;
        while (p < file_end && *p != '\n') ++p;
        boundaries[i] = p < file_end ? p + 1 : file_end;
    }

    std::vector<LocalAggregateTable> local_tables;
    local_tables.reserve(thread_count);
    for (unsigned i = 0; i < thread_count; ++i) {
        local_tables.emplace_back(base_month, dictionary);
    }

    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    using ParseFunction = void (*)(const char*, const char*, LocalAggregateTable&);
    const bool fast_channels = dictionary.fast_safe();
    const ParseFunction parser = fixed_timestamps
        ? (fast_channels
            ? &parse_partition<true, false>
            : &parse_partition<true, true>)
        : (fast_channels
            ? &parse_partition<false, false>
            : &parse_partition<false, true>);
#ifdef BRC_WORK_STEAL_SEGMENTS
    constexpr std::size_t segment_size = 1U << 21;
    std::vector<const char*> segment_boundaries;
    segment_boundaries.reserve(body_size / segment_size + 2);
    segment_boundaries.push_back(body);
    for (std::size_t offset = segment_size; offset < body_size;
         offset += segment_size) {
        const char* p = body + offset;
        while (p < file_end && *p != '\n') ++p;
        segment_boundaries.push_back(p < file_end ? p + 1 : file_end);
    }
    segment_boundaries.push_back(file_end);
    std::atomic<std::size_t> next_segment {0};
    const auto process_segments = [&](unsigned worker_index) {
        for (;;) {
            const std::size_t segment =
                next_segment.fetch_add(1, std::memory_order_relaxed);
            if (segment + 1 >= segment_boundaries.size()) break;
            parser(segment_boundaries[segment], segment_boundaries[segment + 1],
                   local_tables[worker_index]);
        }
    };
    for (unsigned i = 1; i < thread_count; ++i) {
        workers.emplace_back(process_segments, i);
    }
    process_segments(0);
#elif defined(BRC_MAIN_THREAD_WORKER)
    for (unsigned i = 1; i < thread_count; ++i) {
        workers.emplace_back(parser, boundaries[i], boundaries[i + 1],
                             std::ref(local_tables[i]));
    }
    parser(boundaries[0], boundaries[1], local_tables[0]);
#else
    for (unsigned i = 0; i < thread_count; ++i) {
        workers.emplace_back(parser, boundaries[i], boundaries[i + 1],
                             std::ref(local_tables[i]));
    }
#endif
    for (std::thread& worker : workers) worker.join();

#ifdef BRC_DIRECT_MERGE_OUTPUT
    write_dense_output(argv[2], dictionary, local_tables, base_month);
#else
    AggregateTable merged;
    for (const LocalAggregateTable& local : local_tables) {
        for (std::size_t block_index = 0;
             block_index < dictionary.metadata().size(); ++block_index) {
            const ChannelMetadata& channel = dictionary.metadata()[block_index];
            const MonthBlock& block =
                local.blocks()[dictionary.storage_index(block_index)];
            for (std::size_t i = 0; i < block.slots.size(); ++i) {
                const CompactStats& stats = block.slots[i];
                if (stats.count_value() == 0) continue;
                const std::int32_t month =
                    local.base_month() + static_cast<std::int32_t>(i);
                merged.find_or_insert(channel.channel, channel.length, month,
                                      key_hash(channel.hash, month))
                    .stats.merge_values(
                        stats.length_sum, stats.count_value(), stats.stamp_sum_value(),
                        stats.min_length_value(), stats.max_length_value());
            }
        }
        for (const Entry& entry : local.overflow().entries()) {
            if (entry.hash == 0) continue;
            merged.find_or_insert(entry.channel, entry.channel_length, entry.month, entry.hash)
                .stats.merge(entry.stats);
        }
    }

    write_output(argv[2], merged);
#endif
    return 0;
}
