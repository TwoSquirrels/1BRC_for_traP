#ifndef PROFILE_PHASES
#define PROFILE_PHASES 0
#endif
#ifndef FAST_AVERAGE_FORMAT
#define FAST_AVERAGE_FORMAT 0
#endif
#ifndef REUSE_AVX2_CHANNEL_WORDS
#define REUSE_AVX2_CHANNEL_WORDS 0
#endif
#ifndef MEMORY_COMMA_COMPARE
#define MEMORY_COMMA_COMPARE 1
#endif
#ifndef SHORT_HASH_MODE
#define SHORT_HASH_MODE 4
#endif
#ifndef COLD_FIRST_AGGREGATE
#define COLD_FIRST_AGGREGATE 0
#endif
#ifndef TIGHT_CRC_CODEGEN
#define TIGHT_CRC_CODEGEN 0
#endif
#ifndef CRC_RORX_FOLD
#define CRC_RORX_FOLD TIGHT_CRC_CODEGEN
#endif
#ifndef CRC_RORX_INDEX
#define CRC_RORX_INDEX TIGHT_CRC_CODEGEN
#endif
#ifndef LUT_SHORT_MASK_WIDTHS
#define LUT_SHORT_MASK_WIDTHS 0
#endif
#ifndef WIDE_TIMESTAMP_MONTH
#define WIDE_TIMESTAMP_MONTH 1
#endif
#ifndef WIDE_SCANNER_OFFSETS
#define WIDE_SCANNER_OFFSETS 1
#endif
#ifndef DIRECT_MESSAGE_POINTER_LEA
#define DIRECT_MESSAGE_POINTER_LEA 0
#endif
#ifndef TZCNT64_DELIMITER_OFFSETS
#define TZCNT64_DELIMITER_OFFSETS 0
#endif
#ifndef COLD_REEXTRACT_EXTREMA
#define COLD_REEXTRACT_EXTREMA 0
#endif
#ifndef COMMON_DIGIT_CMOV
#define COMMON_DIGIT_CMOV 0
#endif
#ifndef INLINE_COMMON_DIGIT_CMOV
#define INLINE_COMMON_DIGIT_CMOV 1
#endif
#ifndef DEFER_VALIDATED_AGGREGATE
#define DEFER_VALIDATED_AGGREGATE 8
#endif
#ifndef DEFER_AGGREGATE_BYTE_OFFSET
#define DEFER_AGGREGATE_BYTE_OFFSET 0
#endif
#ifndef DEFER_AGGREGATE_SOA_RING
#define DEFER_AGGREGATE_SOA_RING 1
#endif
#ifndef VALIDATED_POINTER_PROBE
#define VALIDATED_POINTER_PROBE 1
#endif
#ifndef XOR_RETRIEVAL_LOOKUP
#define XOR_RETRIEVAL_LOOKUP 1
#endif
#ifndef XOR_RETRIEVAL_MIX
#define XOR_RETRIEVAL_MIX 0
#endif
#ifndef XOR_RETRIEVAL_LEFT_BITS
#define XOR_RETRIEVAL_LEFT_BITS 14
#endif
#ifndef XOR_RETRIEVAL_RIGHT_BITS
#define XOR_RETRIEVAL_RIGHT_BITS 14
#endif
#ifndef XOR_RETRIEVAL_PREFETCH_LEFT
#define XOR_RETRIEVAL_PREFETCH_LEFT 1
#endif
#ifndef DEFER_AGGREGATE_EARLY_PREFETCH
#define DEFER_AGGREGATE_EARLY_PREFETCH 0
#endif
#ifndef PACKED_COMMON_DIGITS
#define PACKED_COMMON_DIGITS 0
#endif

#include <array>
#include <cerrno>
#include <charconv>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__x86_64__) && defined(__AVX2__) && defined(__BMI2__)
#include <immintrin.h>
#define HAS_X86_ROW_SIMD 1
#else
#define HAS_X86_ROW_SIMD 0
#endif

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if PROFILE_PHASES
#include <time.h>
#endif
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FORCE_INLINE inline
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

namespace {

#if PROFILE_PHASES
[[noreturn]] void fatal_errno(const char *operation);

std::uint64_t profile_now_ns() noexcept {
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
    fatal_errno("clock_gettime");
  }
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000u +
         static_cast<std::uint64_t>(value.tv_nsec);
}

void print_profile_duration(const char *name, std::uint64_t begin,
                            std::uint64_t end) {
  std::fprintf(stderr, "phase %-12s %9.3f ms\n", name,
               static_cast<double>(end - begin) / 1'000'000.0);
}
#endif

// -----------------------------------------------------------------------------
// 実行環境・入力形式に合わせた主要設定
// -----------------------------------------------------------------------------
constexpr std::size_t kRequestedWorkerCount = 8;
static_assert(kRequestedWorkerCount >= 1);

constexpr char kHeader[] =
    "unix_timestamp,channel_path,message_length,stamp_count\n";
constexpr std::size_t kHeaderLength = sizeof(kHeader) - 1;
constexpr std::size_t kTimestampLength = 10;

constexpr std::uint32_t kMinTimestamp = 1'798'761'600u; // 2027-01-01 UTC
constexpr std::uint32_t kMaxTimestamp = 1'830'297'599u; // 2027-12-31 UTC
constexpr std::uint32_t kSecondsPerDay = 86'400u;
constexpr std::uint32_t kFirstDay = kMinTimestamp / kSecondsPerDay;
constexpr std::uint32_t kLastDay = kMaxTimestamp / kSecondsPerDay;
constexpr std::size_t kDayCount =
    static_cast<std::size_t>(kLastDay - kFirstDay + 1);

constexpr std::size_t kMaxChannelCount = 10'000;
constexpr unsigned kChannelTableBits = 15;
constexpr std::size_t kChannelTableCapacity =
    std::size_t{1} << kChannelTableBits;
constexpr std::size_t kValidatedProbeGuard = 16;
constexpr std::size_t kXorRetrievalLeftSize =
    1u << XOR_RETRIEVAL_LEFT_BITS;
constexpr std::size_t kXorRetrievalRightSize =
    1u << XOR_RETRIEVAL_RIGHT_BITS;
constexpr std::size_t kXorRetrievalVertexCount =
    kXorRetrievalLeftSize + kXorRetrievalRightSize;
constexpr std::size_t kXorRetrievalLeftMask = kXorRetrievalLeftSize - 1u;
constexpr std::size_t kXorRetrievalRightMask = kXorRetrievalRightSize - 1u;
constexpr std::size_t kChannelSlotAllocation =
    kChannelTableCapacity +
    (VALIDATED_POINTER_PROBE ? kValidatedProbeGuard : 0u);
static_assert(kChannelTableBits >= 14 && kChannelTableBits <= 20);
static_assert(kMaxChannelCount < kChannelTableCapacity);

// aggregateはTHPを利用できるよう16,384 channel分を連続確保します。
// 入力上限は10,000なので、channel_idから常に直接indexできます。
constexpr std::size_t kDenseChannelCapacity = 1u << 14;
static_assert(kMaxChannelCount <= kDenseChannelCapacity);

// mmapによる仮想アドレス予約です。実メモリは主に書き込んだページで消費します。
constexpr std::size_t kLocalNamePoolCapacity = 1ull << 30;  // 1 GiB / worker
constexpr std::size_t kGlobalNamePoolCapacity = 2ull << 30; // 2 GiB

constexpr std::size_t kOutputBufferSize = 32u << 20; // 32 MiB

[[noreturn]] void fatal(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  std::exit(EXIT_FAILURE);
}

[[noreturn]] void fatal_errno(const char *operation) {
  const int saved = errno;
  std::fprintf(stderr, "%s: %s\n", operation, std::strerror(saved));
  std::exit(EXIT_FAILURE);
}

void *map_zeroed_bytes(std::size_t bytes, bool use_huge_pages = false) {
  if (bytes == 0) {
    fatal("attempted to mmap zero bytes");
  }

  void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    fatal_errno("mmap anonymous memory");
  }

#ifdef MADV_HUGEPAGE
  // 疎なhash table/name poolにはTHPを付けず、密に触る大きなarenaだけに
  // 限定します。不要なTHP promotion/zeroingとpage-walk pressureを避けます。
  if (use_huge_pages) {
    (void)madvise(memory, bytes, MADV_HUGEPAGE);
  }
#endif
#ifdef MADV_DONTDUMP
  (void)madvise(memory, bytes, MADV_DONTDUMP);
#endif

  return memory;
}

template <class T>
T *map_zeroed_array(std::size_t count, bool use_huge_pages = false) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
    fatal("mmap size overflow");
  }
  return static_cast<T *>(
      map_zeroed_bytes(count * sizeof(T), use_huge_pages));
}

template <class T> void unmap_array(T *pointer, std::size_t count) noexcept {
  if (pointer != nullptr) {
    (void)munmap(pointer, count * sizeof(T));
  }
}

// -----------------------------------------------------------------------------
// timestamp -> 月インデックス
// -----------------------------------------------------------------------------

constexpr std::pair<unsigned, unsigned>
year_month_from_unix_day(std::int64_t unix_day) {
  std::int64_t z = unix_day + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(z - era * 146097);
  const unsigned year_of_era = (day_of_era - day_of_era / 1460 +
                                day_of_era / 36524 - day_of_era / 146096) /
                               365;
  std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned month_part = (5 * day_of_year + 2) / 153;
  const unsigned month = month_part < 10 ? month_part + 3 : month_part - 9;
  year += month <= 2;
  return {static_cast<unsigned>(year), month};
}

constexpr std::uint16_t encode_year_month(unsigned year, unsigned month) {
  return static_cast<std::uint16_t>((year << 4) | month);
}

constexpr auto kFirstYearMonth =
    year_month_from_unix_day(static_cast<std::int64_t>(kFirstDay));
constexpr auto kLastYearMonth =
    year_month_from_unix_day(static_cast<std::int64_t>(kLastDay));
constexpr unsigned kFirstAbsoluteMonth =
    kFirstYearMonth.first * 12u + (kFirstYearMonth.second - 1u);
constexpr unsigned kLastAbsoluteMonth =
    kLastYearMonth.first * 12u + (kLastYearMonth.second - 1u);
constexpr std::size_t kMonthCount =
    static_cast<std::size_t>(kLastAbsoluteMonth - kFirstAbsoluteMonth + 1u);
static_assert(kMonthCount == 12, "the contest input is restricted to 2027");

struct MonthTables {
  std::array<std::uint8_t, kDayCount> day_to_month_index{};
  std::array<std::uint16_t, kMonthCount> month_codes{};
};

consteval MonthTables make_month_tables() {
  MonthTables result{};

  for (std::size_t month_index = 0; month_index < kMonthCount; ++month_index) {
    const unsigned absolute_month =
        (kFirstYearMonth.second - 1u) + static_cast<unsigned>(month_index);
    const unsigned year = kFirstYearMonth.first + absolute_month / 12u;
    const unsigned month = absolute_month % 12u + 1u;
    result.month_codes[month_index] = encode_year_month(year, month);
  }

  for (std::size_t i = 0; i < kDayCount; ++i) {
    const auto [year, month] =
        year_month_from_unix_day(static_cast<std::int64_t>(kFirstDay + i));
    const unsigned month_index =
        (year - kFirstYearMonth.first) * 12u + (month - kFirstYearMonth.second);
    result.day_to_month_index[i] = static_cast<std::uint8_t>(month_index);
  }

  return result;
}

inline constexpr MonthTables kMonthTables = make_month_tables();

constexpr std::uint32_t kTimestampBucketSeconds = 100'000u;
constexpr std::uint32_t kFirstTimestampPrefix =
    kMinTimestamp / kTimestampBucketSeconds;
constexpr std::uint32_t kLastTimestampPrefix =
    kMaxTimestamp / kTimestampBucketSeconds;
constexpr std::size_t kTimestampPrefixCount =
    static_cast<std::size_t>(kLastTimestampPrefix - kFirstTimestampPrefix + 1u);
constexpr std::uint8_t kTimestampBoundaryBucket = 0xffu;

constexpr std::uint8_t month_index_from_timestamp(std::uint32_t timestamp) {
  const std::uint32_t unix_day = timestamp / kSecondsPerDay;
  return kMonthTables.day_to_month_index[unix_day - kFirstDay];
}

consteval std::array<std::uint8_t, kTimestampPrefixCount>
make_timestamp_prefix_table() {
  std::array<std::uint8_t, kTimestampPrefixCount> result{};
  for (std::size_t i = 0; i < kTimestampPrefixCount; ++i) {
    const std::uint32_t raw_start =
        (kFirstTimestampPrefix + static_cast<std::uint32_t>(i)) *
        kTimestampBucketSeconds;
    const std::uint32_t start =
        raw_start < kMinTimestamp ? kMinTimestamp : raw_start;
    const std::uint32_t raw_end =
        raw_start + (kTimestampBucketSeconds - 1u);
    const std::uint32_t end =
        raw_end > kMaxTimestamp ? kMaxTimestamp : raw_end;
    const std::uint8_t first_month = month_index_from_timestamp(start);
    const std::uint8_t last_month = month_index_from_timestamp(end);
    result[i] = first_month == last_month ? first_month
                                          : kTimestampBoundaryBucket;
  }
  return result;
}

inline constexpr auto kTimestampPrefixTable = make_timestamp_prefix_table();

constexpr std::uint64_t kAscii5Mask = (std::uint64_t{1} << 40) - 1u;

constexpr std::uint64_t encode_ascii5(std::uint32_t value) noexcept {
  std::uint64_t encoded = 0;
  std::uint32_t divisor = 10'000u;
  for (unsigned i = 0; i < 5; ++i) {
    encoded = (encoded << 8) | static_cast<std::uint64_t>(
                                    '0' + (value / divisor) % 10u);
    divisor /= 10u;
  }
  return encoded;
}

consteval std::array<std::uint64_t, kTimestampPrefixCount>
make_timestamp_boundary_table() {
  std::array<std::uint64_t, kTimestampPrefixCount> result{};

  for (std::size_t i = 0; i < kTimestampPrefixCount; ++i) {
    const std::uint32_t raw_start =
        (kFirstTimestampPrefix + static_cast<std::uint32_t>(i)) *
        kTimestampBucketSeconds;
    const std::uint32_t start =
        raw_start < kMinTimestamp ? kMinTimestamp : raw_start;
    const std::uint32_t raw_end =
        raw_start + (kTimestampBucketSeconds - 1u);
    const std::uint32_t end =
        raw_end > kMaxTimestamp ? kMaxTimestamp : raw_end;
    const std::uint8_t month_before = month_index_from_timestamp(start);

    if (month_before == month_index_from_timestamp(end)) {
      continue;
    }

    // この100,000秒bucket内で次月へ切り替わる最初のtimestampを求めます。
    std::uint32_t low = start;
    std::uint32_t high = end;
    while (low < high) {
      const std::uint32_t middle = low + (high - low) / 2u;
      if (month_index_from_timestamp(middle) == month_before) {
        low = middle + 1u;
      } else {
        high = middle;
      }
    }

    result[i] = encode_ascii5(low % kTimestampBucketSeconds) |
                (static_cast<std::uint64_t>(month_before) << 40);
  }
  return result;
}

inline constexpr auto kTimestampBoundaryTable =
    make_timestamp_boundary_table();

constexpr std::size_t kTimestampHashTableSize = 512;
constexpr std::uint32_t kTimestampHashMultiplier = 0x302b60dfu;
constexpr unsigned kTimestampHashShift = 23;

#if WIDE_TIMESTAMP_MONTH
using TimestampMonthIndex = std::uint32_t;
#else
using TimestampMonthIndex = std::uint8_t;
#endif

struct HashedTimestampTables {
  std::array<TimestampMonthIndex, kTimestampHashTableSize> month{};
  std::array<std::uint64_t, kTimestampHashTableSize> boundary{};
  bool collision = false;
};

consteval std::uint32_t encode_ascii4_le(std::uint32_t value) {
  return static_cast<std::uint32_t>('0' + (value / 1000u) % 10u) |
         (static_cast<std::uint32_t>('0' + (value / 100u) % 10u) << 8) |
         (static_cast<std::uint32_t>('0' + (value / 10u) % 10u) << 16) |
         (static_cast<std::uint32_t>('0' + value % 10u) << 24);
}

consteval HashedTimestampTables make_hashed_timestamp_tables() {
  HashedTimestampTables result{};
  std::array<bool, kTimestampHashTableSize> occupied{};

  for (std::size_t i = 0; i < kTimestampPrefixCount; ++i) {
    const std::uint32_t prefix =
        kFirstTimestampPrefix + static_cast<std::uint32_t>(i);
    const std::uint32_t raw_ascii = encode_ascii4_le(prefix % 10'000u);
    const std::size_t index = static_cast<std::uint32_t>(
                                  raw_ascii * kTimestampHashMultiplier) >>
                              kTimestampHashShift;
    if (occupied[index]) {
      result.collision = true;
    }
    occupied[index] = true;
    result.month[index] = kTimestampPrefixTable[i];
    result.boundary[index] = kTimestampBoundaryTable[i];
  }
  return result;
}

inline constexpr auto kHashedTimestampTables = make_hashed_timestamp_tables();
static_assert(!kHashedTimestampTables.collision,
              "timestamp prefix hash must be collision-free");

FORCE_INLINE std::uint64_t read_ascii5_be(const char *s) noexcept {
  std::uint32_t first_four;
  std::memcpy(&first_four, s, sizeof(first_four));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  first_four = __builtin_bswap32(first_four);
#endif
  return (static_cast<std::uint64_t>(first_four) << 8) |
         static_cast<unsigned char>(s[4]);
}

FORCE_INLINE TimestampMonthIndex
timestamp_to_month_index(const char *s) noexcept {
  // 10桁timestampの先頭5桁を、2027年を覆う小さな相対tableへ写像します。
  // 大半の100,000秒bucketは月境界を含まないため、全桁parseを省略できます。
  std::uint32_t raw_ascii;
  std::memcpy(&raw_ascii, s + 1, sizeof(raw_ascii));
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  raw_ascii = __builtin_bswap32(raw_ascii);
#endif
  const std::size_t prefix_index =
      static_cast<std::uint32_t>(raw_ascii * kTimestampHashMultiplier) >>
      kTimestampHashShift;
  const TimestampMonthIndex cached =
      kHashedTimestampTables.month[prefix_index];
  if (LIKELY(cached != kTimestampBoundaryBucket)) {
    return cached;
  }

  // 月境界bucketでも10桁parseと86,400除算は行わず、下位5桁を
  // 固定幅ASCIIのまま月初cutoffと比較します。
  const std::uint64_t boundary =
      kHashedTimestampTables.boundary[prefix_index];
  const TimestampMonthIndex month_before =
      static_cast<TimestampMonthIndex>(boundary >> 40);
  return read_ascii5_be(s + 5) < (boundary & kAscii5Mask)
             ? month_before
             : static_cast<TimestampMonthIndex>(month_before + 1u);
}

// 1～5桁の符号なし整数（0～99999）を解析します。
// 戻った時点でpはカンマ、CR、LFなどの区切り文字を指します。
// 入力値が必ず1～5桁であることを前提に、分岐を展開しています。
FORCE_INLINE std::uint32_t decode_uint_1_to_5_digits(const char *&p) noexcept {
  std::uint32_t value = static_cast<std::uint32_t>(*p++ - '0');

  if (*p < '0' || *p > '9')
    return value;
  value = value * 10u + static_cast<std::uint32_t>(*p++ - '0');

  if (*p < '0' || *p > '9')
    return value;
  value = value * 10u + static_cast<std::uint32_t>(*p++ - '0');

  if (*p < '0' || *p > '9')
    return value;
  value = value * 10u + static_cast<std::uint32_t>(*p++ - '0');

  if (*p < '0' || *p > '9')
    return value;

  value = value * 10u + static_cast<std::uint32_t>(*p++ - '0');
  return value;
}

// -----------------------------------------------------------------------------
// チャンネル名の1パス走査・ハッシュ
// -----------------------------------------------------------------------------

FORCE_INLINE std::uint64_t read_u64(const char *p) noexcept {
  std::uint64_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

FORCE_INLINE std::uint64_t multiply_mix(std::uint64_t a,
                                        std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
  const __uint128_t product = static_cast<__uint128_t>(a) * b;
  return static_cast<std::uint64_t>(product) ^
         static_cast<std::uint64_t>(product >> 64);
#else
  // 対象環境はx86-64を想定しているため通常はこちらには入りません。
  a ^= a >> 32;
  a *= b;
  a ^= a >> 29;
  return a;
#endif
}

FORCE_INLINE std::uint64_t hash_short_channel(std::uint64_t key0,
                                              std::uint64_t key1,
                                              std::size_t length) noexcept {
#if SHORT_HASH_MODE == 3
  (void)length;
  constexpr std::uint64_t kSecret3 = 0x8ebc6af09c88c6e3ULL;
  return multiply_mix(key0, key1 ^ kSecret3);
#elif SHORT_HASH_MODE == 4 && HAS_X86_ROW_SIMD
  (void)length;
#if CRC_RORX_FOLD
  const std::uint64_t swapped = std::rotr(key1, 32);
  const std::uint32_t seed = static_cast<std::uint32_t>(key1 ^ swapped);
#else
  const std::uint32_t seed = static_cast<std::uint32_t>(key1) ^
                             static_cast<std::uint32_t>(key1 >> 32);
#endif
  return static_cast<std::uint32_t>(_mm_crc32_u64(seed, key0));
#else
#if SHORT_HASH_MODE == 0
  constexpr std::uint64_t kSecret1 = 0xa0761d6478bd642fULL;
#endif
  constexpr std::uint64_t kSecret2 = 0xe7037ed1a0b428dbULL;
#if SHORT_HASH_MODE == 0 || SHORT_HASH_MODE == 1
  constexpr std::uint64_t kSecret3 = 0x8ebc6af09c88c6e3ULL;
#endif
#if SHORT_HASH_MODE == 1
  return multiply_mix(key0, key1 ^ (length * kSecret2) ^ kSecret3);
#elif SHORT_HASH_MODE == 2 || SHORT_HASH_MODE == 4
  return multiply_mix(key0, key1 ^ (length * kSecret2));
#else
  return multiply_mix(key0 ^ kSecret1,
                      key1 ^ (length * kSecret2) ^ kSecret3);
#endif
#endif
}

FORCE_INLINE std::uint64_t rotate_left(std::uint64_t value,
                                       unsigned shift) noexcept {
  return (value << shift) | (value >> (64u - shift));
}

FORCE_INLINE std::uint64_t hash_channel_17_to_32(
    const char *begin, std::uint64_t key0, std::uint64_t key1,
    std::size_t length) noexcept {
  constexpr std::uint64_t kSecret1 = 0xa0761d6478bd642fULL;
  constexpr std::uint64_t kSecret2 = 0xe7037ed1a0b428dbULL;
  constexpr std::uint64_t kSecret3 = 0x8ebc6af09c88c6e3ULL;
  const std::uint64_t middle = length > 24 ? read_u64(begin + 16) : 0;
  const std::uint64_t tail = read_u64(begin + length - 8);
  return multiply_mix(key0 ^ rotate_left(middle, 21) ^ kSecret1,
                      key1 ^ rotate_left(tail, 37) ^
                          (length * kSecret2) ^ kSecret3);
}

FORCE_INLINE std::uint64_t finish_channel_hash(
    const char *begin, std::uint64_t key0, std::uint64_t key1,
    std::size_t length, std::uint64_t iterative_hash,
    std::uint64_t tail) noexcept {
  constexpr std::uint64_t kSecret2 = 0xe7037ed1a0b428dbULL;
  constexpr std::uint64_t kSecret3 = 0x8ebc6af09c88c6e3ULL;
  if (length <= 16) {
    return hash_short_channel(key0, key1, length);
  }
  if (length <= 32) {
    return hash_channel_17_to_32(begin, key0, key1, length);
  }
  return multiply_mix(iterative_hash ^ tail ^ (length * kSecret2), kSecret3);
}

FORCE_INLINE std::uint64_t mix64(std::uint64_t value) noexcept {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  return value;
}

FORCE_INLINE std::uint32_t channel_fingerprint(std::uint64_t hash) noexcept {
  // exact辞書構築ではtable indexと独立した下位bitを候補filterへ使います。
  return static_cast<std::uint32_t>(hash);
}

FORCE_INLINE std::uint16_t
validated_channel_fingerprint(std::uint64_t hash) noexcept {
#if SHORT_HASH_MODE == 3 || (SHORT_HASH_MODE == 4 && HAS_X86_ROW_SIMD)
  // Candidate hashはindexと独立したlow16を使い、validated lookupの
  // fingerprint抽出shiftを省きます。
  return static_cast<std::uint16_t>(hash);
#else
  // Public辞書ではlow16に曖昧なprobeが1件あるため、index bits 32～46と
  // 重ならないupper16を使い、使用前に全channel IDをruntime検証します。
  return static_cast<std::uint16_t>(hash >> 48);
#endif
}

FORCE_INLINE std::size_t channel_initial_index(std::uint64_t hash) noexcept {
#if SHORT_HASH_MODE == 4 && HAS_X86_ROW_SIMD && CRC_RORX_INDEX
  return std::rotr(static_cast<std::uint32_t>(hash), 16) &
         (kChannelTableCapacity - 1);
#else
#if SHORT_HASH_MODE == 3
  constexpr unsigned kIndexShift = 46;
#elif SHORT_HASH_MODE == 4 && HAS_X86_ROW_SIMD
  constexpr unsigned kIndexShift = 16;
#else
  constexpr unsigned kIndexShift = 32;
#endif
  return static_cast<std::size_t>(hash >> kIndexShift) &
         (kChannelTableCapacity - 1);
#endif
}

FORCE_INLINE std::uint32_t xor_retrieval_hash(std::uint64_t hash) noexcept {
  std::uint32_t value =
      static_cast<std::uint32_t>(hash) ^ static_cast<std::uint32_t>(hash >> 32);
#if XOR_RETRIEVAL_MIX
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
#endif
  return value;
}

FORCE_INLINE void prefetch_channel_slot(const char *slot_bytes,
                                        std::uint64_t hash) noexcept {
#if defined(__GNUC__) || defined(__clang__)
#if XOR_RETRIEVAL_LOOKUP
  const std::uint32_t mixed = xor_retrieval_hash(hash);
  const std::uint16_t *const retrieval =
      reinterpret_cast<const std::uint16_t *>(slot_bytes);
#if XOR_RETRIEVAL_PREFETCH_LEFT
  __builtin_prefetch(retrieval + (mixed & kXorRetrievalLeftMask), 0, 3);
#endif
  __builtin_prefetch(retrieval + kXorRetrievalLeftSize +
                         ((mixed >> XOR_RETRIEVAL_LEFT_BITS) &
                          kXorRetrievalRightMask),
                     0, 3);
#else
  // hash確定直後にT0 hintを発行し、slot load待ちをnumeric decodeと重ねます。
  const std::size_t index = channel_initial_index(hash);
  __builtin_prefetch(slot_bytes + index * sizeof(std::uint32_t), 0, 3);
#endif
#else
  (void)slot_bytes;
  (void)hash;
#endif
}

#if WIDE_SCANNER_OFFSETS
using FastRowOffset = std::size_t;
#else
using FastRowOffset = std::uint32_t;
#endif
#if WIDE_SCANNER_OFFSETS >= 2
using ChannelScanOffset = std::size_t;
#else
using ChannelScanOffset = unsigned;
#endif

#if HAS_X86_ROW_SIMD
#if MEMORY_COMMA_COMPARE
alignas(32) constexpr std::array<unsigned char, 32> kCommaVector = {
    ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',',
    ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ',', ','};
#endif
#if LUT_SHORT_MASK_WIDTHS
alignas(64) constexpr std::array<std::uint16_t, 17> kShortMaskWidths = {
    0x0000, 0x0008, 0x0010, 0x0018, 0x0020, 0x0028,
    0x0030, 0x0038, 0x0040, 0x0840, 0x1040, 0x1840,
    0x2040, 0x2840, 0x3040, 0x3840, 0x4040};
#endif

FORCE_INLINE __m256i compare_commas(__m256i bytes) noexcept {
#if MEMORY_COMMA_COMPARE
  __m256i result;
  asm("vpcmpeqb %2, %1, %0"
      : "=x"(result)
      : "x"(bytes), "m"(kCommaVector));
  return result;
#else
  return _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(','));
#endif
}

FORCE_INLINE const char *
message_pointer(const char *row, ChannelScanOffset channel_length) noexcept {
#if DIRECT_MESSAGE_POINTER_LEA && HAS_X86_ROW_SIMD
  std::uintptr_t offset = channel_length;
  asm("leaq 1(%1,%0), %0"
      : "+r"(offset)
      : "r"(row));
  return reinterpret_cast<const char *>(offset);
#else
  return row + static_cast<std::size_t>(channel_length) + 1u;
#endif
}

FORCE_INLINE bool parse_masked_common_numeric_without_newline_scan(
    const char *row, ChannelScanOffset channel_length,
    std::uint32_t remaining_commas,
    std::uint32_t &message_length, std::uint32_t &stamp_count,
    FastRowOffset &row_bytes) noexcept {
  if (UNLIKELY(remaining_commas == 0)) {
    return false;
  }

#if WIDE_SCANNER_OFFSETS
#if TZCNT64_DELIMITER_OFFSETS
  const std::size_t message_end = static_cast<std::size_t>(
      __builtin_ctzll(static_cast<std::uint64_t>(remaining_commas)));
#else
  const std::size_t message_end =
      static_cast<unsigned>(__builtin_ctz(remaining_commas));
#endif
  const unsigned message_digits =
      static_cast<unsigned>(message_end - channel_length - 1u);
#else
  const unsigned message_end =
      static_cast<unsigned>(__builtin_ctz(remaining_commas));
  const unsigned message_digits = message_end - channel_length - 1u;
#endif

  // common形式では第2 commaの直後が1桁stamp、その次がLFです。
  if (LIKELY((message_digits == 2u || message_digits == 3u) &&
             row[message_end + 2u] == '\n')) {
    const char *const message = message_pointer(row, channel_length);
#if PACKED_COMMON_DIGITS
    std::uint32_t raw_digits;
    std::memcpy(&raw_digits, message, sizeof(raw_digits));
    const std::uint32_t first_digit =
        (raw_digits & 0xffu) - static_cast<unsigned>('0');
    const std::uint32_t second_digit =
        ((raw_digits >> 8) & 0xffu) - static_cast<unsigned>('0');
    const std::uint32_t third_digit =
        ((raw_digits >> 16) & 0xffu) - static_cast<unsigned>('0');
    const std::uint32_t first_two = first_digit * 10u + second_digit;
#else
    const std::uint32_t first_two =
        static_cast<std::uint32_t>(message[0] - '0') * 10u +
        static_cast<std::uint32_t>(message[1] - '0');
    const std::uint32_t third_digit =
        static_cast<std::uint32_t>(message[2] - '0');
#endif
    const std::uint32_t has_third = message_digits - 2u;
#if INLINE_COMMON_DIGIT_CMOV && HAS_X86_ROW_SIMD
    const std::uint32_t three_digits = first_two * 10u + third_digit;
    message_length = first_two;
    asm("testl %2, %2\n\tcmovnel %1, %0"
        : "+r"(message_length)
        : "r"(three_digits), "r"(has_third)
        : "cc");
#elif COMMON_DIGIT_CMOV
    const std::uint32_t three_digits = first_two * 10u + third_digit;
    message_length = has_third != 0 ? three_digits : first_two;
#else
    message_length =
        first_two + has_third * (first_two * 9u + third_digit);
#endif
    stamp_count =
        static_cast<std::uint32_t>(row[message_end + 1u] - '0');
    row_bytes = message_end + 3u;
    return true;
  }
  return false;
}

FORCE_INLINE void parse_masked_numeric(const char *row,
                                       ChannelScanOffset channel_length,
                                       std::uint32_t comma_mask,
                                       std::uint32_t newline_mask,
                                       std::uint32_t &message_length,
                                       std::uint32_t &stamp_count,
                                       FastRowOffset &row_bytes) noexcept {
  const std::uint32_t remaining_commas = comma_mask & (comma_mask - 1u);
#if WIDE_SCANNER_OFFSETS
#if TZCNT64_DELIMITER_OFFSETS
  const std::size_t message_end = static_cast<std::size_t>(
      __builtin_ctzll(static_cast<std::uint64_t>(remaining_commas)));
  const std::size_t row_end = static_cast<std::size_t>(
      __builtin_ctzll(static_cast<std::uint64_t>(newline_mask)));
#else
  const std::size_t message_end =
      static_cast<unsigned>(__builtin_ctz(remaining_commas));
  const std::size_t row_end =
      static_cast<unsigned>(__builtin_ctz(newline_mask));
#endif
#else
  const unsigned message_end =
      static_cast<unsigned>(__builtin_ctz(remaining_commas));
  const unsigned row_end = static_cast<unsigned>(__builtin_ctz(newline_mask));
#endif
  const unsigned message_digits =
      static_cast<unsigned>(message_end - channel_length - 1u);
  const unsigned stamp_digits =
      static_cast<unsigned>(row_end - message_end - 1u);
  const char *const message = message_pointer(row, channel_length);
  const char *const stamp = row + message_end + 1u;

  if (LIKELY(stamp_digits == 1u &&
             (message_digits == 2u || message_digits == 3u))) {
    const std::uint32_t first_two =
        static_cast<std::uint32_t>(message[0] - '0') * 10u +
        static_cast<std::uint32_t>(message[1] - '0');
    const std::uint32_t has_third = message_digits - 2u;
#if COMMON_DIGIT_CMOV
    const std::uint32_t three_digits =
        first_two * 10u + static_cast<std::uint32_t>(message[2] - '0');
    message_length = has_third != 0 ? three_digits : first_two;
#else
    message_length =
        first_two + has_third *
                        (first_two * 9u +
                         static_cast<std::uint32_t>(message[2] - '0'));
#endif
    stamp_count = static_cast<std::uint32_t>(stamp[0] - '0');
  } else {
    const char *numeric = message;
    message_length = decode_uint_1_to_5_digits(numeric);
    ++numeric;
    stamp_count = decode_uint_1_to_5_digits(numeric);
  }
  row_bytes = row_end + 1u;
}
#endif

struct ChannelScanResult {
  std::uint64_t hash;
  std::uint64_t key0;
  std::uint64_t key1;
  std::size_t length;
};
static_assert(sizeof(ChannelScanResult) == 32);

FORCE_INLINE ChannelScanResult
scan_channel_and_hash(const char *p, const char *end,
                      const char *slot_bytes,
                      std::uint32_t &fast_message_length,
                      std::uint32_t &fast_stamp_count,
                      FastRowOffset &fast_row_bytes) noexcept {
  constexpr std::uint64_t kCommaBytes = 0x2c2c2c2c2c2c2c2cULL;
  constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
  constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
  constexpr std::uint64_t kSecret1 = 0xa0761d6478bd642fULL;
  constexpr std::uint64_t kSecret2 = 0xe7037ed1a0b428dbULL;

  const char *const begin = p;
  std::uint64_t hash = kSecret1;
  std::uint64_t key0 = 0;
  std::uint64_t key1 = 0;
  fast_message_length = 0;
  fast_stamp_count = 0;
  fast_row_bytes = 0;

#if HAS_X86_ROW_SIMD
  // 32 bytesからchannel終端を取得し、短いrowではnumeric delimitersも共有します。
  // channel長16以下はone-multiply hash、17～31も専用hashを直接計算します。
  if (LIKELY(p + 32 <= end)) {
    const __m256i bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
    const __m256i commas = compare_commas(bytes);
    const std::uint32_t comma_mask =
        static_cast<std::uint32_t>(_mm256_movemask_epi8(commas));

    if (LIKELY(comma_mask != 0)) {
      const ChannelScanOffset channel_length =
          static_cast<unsigned>(__builtin_ctz(comma_mask));
#if REUSE_AVX2_CHANNEL_WORDS
      const __m128i low_bytes = _mm256_castsi256_si128(bytes);
      const std::uint64_t word0 =
          static_cast<std::uint64_t>(_mm_cvtsi128_si64(low_bytes));
      const std::uint64_t word1 =
          static_cast<std::uint64_t>(_mm_extract_epi64(low_bytes, 1));
#else
      const std::uint64_t word0 = read_u64(p);
      const std::uint64_t word1 = read_u64(p + 8);
#endif

      if (LIKELY(channel_length <= 16)) {
#if LUT_SHORT_MASK_WIDTHS
        const std::uint32_t widths = kShortMaskWidths[channel_length];
        key0 = _bzhi_u64(word0, widths);
        key1 = _bzhi_u64(word1, widths >> 8);
#else
        const ChannelScanOffset key1_bytes =
            channel_length > 8 ? channel_length - 8 : 0;
        key0 = _bzhi_u64(word0, channel_length * 8u);
        key1 = _bzhi_u64(word1, key1_bytes * 8u);
#endif
        hash = hash_short_channel(key0, key1, channel_length);
      } else {
        key0 = word0;
        key1 = word1;
        hash = hash_channel_17_to_32(p, key0, key1, channel_length);
      }

      prefetch_channel_slot(slot_bytes, hash);

      const std::uint32_t remaining_commas = comma_mask & (comma_mask - 1u);

      if (LIKELY(parse_masked_common_numeric_without_newline_scan(
              p, channel_length, remaining_commas, fast_message_length,
              fast_stamp_count, fast_row_bytes))) {
        return {hash, key0, key1, channel_length};
      }

      const __m256i newlines =
          _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\n'));
      const std::uint32_t newline_mask =
          static_cast<std::uint32_t>(_mm256_movemask_epi8(newlines));

      if (channel_length <= 16 ||
          LIKELY(remaining_commas != 0 && newline_mask != 0)) {
        parse_masked_numeric(p, channel_length, comma_mask, newline_mask,
                             fast_message_length, fast_stamp_count,
                             fast_row_bytes);
      }
      return {hash, key0, key1, channel_length};
    }
  }
#endif

  while (p + 16 <= end) {
    const std::uint64_t word0 = read_u64(p);
    const std::uint64_t word1 = read_u64(p + 8);
    const std::uint64_t x0 = word0 ^ kCommaBytes;
    const std::uint64_t x1 = word1 ^ kCommaBytes;
    const std::uint64_t comma_mask0 = (x0 - kOnes) & ~x0 & kHighBits;
    const std::uint64_t comma_mask1 = (x1 - kOnes) & ~x1 & kHighBits;

    if ((comma_mask0 | comma_mask1) != 0) {
      const std::size_t offset = static_cast<std::size_t>(p - begin);

      if (comma_mask0 != 0) {
        const unsigned byte_count =
            static_cast<unsigned>(__builtin_ctzll(comma_mask0) >> 3);
        const std::uint64_t mask =
            byte_count == 0
                ? 0
                : (std::uint64_t{1} << (byte_count * 8u)) - 1u;
        const std::uint64_t tail = word0 & mask;

        if (offset == 0) {
          key0 = tail;
        } else if (offset == 8) {
          key1 = tail;
        }

        const std::size_t length = offset + byte_count;
        hash = finish_channel_hash(begin, key0, key1, length, hash, tail);
        prefetch_channel_slot(slot_bytes, hash);
        return {hash, key0, key1, length};
      }

      if (offset == 0) {
        key0 = word0;
      } else if (offset == 8) {
        key1 = word0;
      }
      hash = multiply_mix(hash ^ word0, kSecret2);

      const unsigned byte_count =
          static_cast<unsigned>(__builtin_ctzll(comma_mask1) >> 3);
      const std::uint64_t mask =
          byte_count == 0
              ? 0
              : (std::uint64_t{1} << (byte_count * 8u)) - 1u;
      const std::uint64_t tail = word1 & mask;
      const std::size_t tail_offset = offset + 8;
      if (tail_offset == 8) {
        key1 = tail;
      }

      const std::size_t length = tail_offset + byte_count;
      hash = finish_channel_hash(begin, key0, key1, length, hash, tail);
      prefetch_channel_slot(slot_bytes, hash);
      return {hash, key0, key1, length};
    }

    const std::size_t offset = static_cast<std::size_t>(p - begin);
    if (offset == 0) {
      key0 = word0;
      key1 = word1;
    } else if (offset == 8) {
      key1 = word0;
    }
    hash = multiply_mix(hash ^ word0, kSecret2);
    hash = multiply_mix(hash ^ word1, kSecret2);
    p += 16;
  }

  while (p + 8 <= end) {
    const std::uint64_t word = read_u64(p);
    const std::uint64_t x = word ^ kCommaBytes;
    const std::uint64_t comma_mask = (x - kOnes) & ~x & kHighBits;

    if (comma_mask != 0) {
      const unsigned byte_count =
          static_cast<unsigned>(__builtin_ctzll(comma_mask) >> 3);
      std::uint64_t tail = 0;
      if (byte_count != 0) {
        const std::uint64_t mask =
            (std::uint64_t{1} << (byte_count * 8u)) - 1u;
        tail = word & mask;
      }

      const std::size_t offset = static_cast<std::size_t>(p - begin);
      if (offset == 0) {
        key0 = tail;
      } else if (offset == 8) {
        key1 = tail;
      }

      const std::size_t length = offset + byte_count;
      hash = finish_channel_hash(begin, key0, key1, length, hash, tail);
      prefetch_channel_slot(slot_bytes, hash);
      return {hash, key0, key1, length};
    }

    const std::size_t offset = static_cast<std::size_t>(p - begin);
    if (offset == 0) {
      key0 = word;
    } else if (offset == 8) {
      key1 = word;
    }

    hash = multiply_mix(hash ^ word, kSecret2);
    p += 8;
  }

  const std::size_t offset = static_cast<std::size_t>(p - begin);
  std::uint64_t tail = 0;
  unsigned tail_bytes = 0;
  while (p < end && *p != ',') {
    tail |= static_cast<std::uint64_t>(static_cast<unsigned char>(*p))
            << (tail_bytes * 8u);
    ++p;
    ++tail_bytes;
  }

  if (offset == 0) {
    key0 = tail;
  } else if (offset == 8) {
    key1 = tail;
  }

  const std::size_t length = static_cast<std::size_t>(p - begin);
  hash = finish_channel_hash(begin, key0, key1, length, hash, tail);
  prefetch_channel_slot(slot_bytes, hash);
  return {hash, key0, key1, length};
}

// -----------------------------------------------------------------------------
// チャンネル辞書
// -----------------------------------------------------------------------------

struct ChannelInfo {
  std::uint64_t hash;
  std::uint64_t key0;
  std::uint64_t key1;
  std::uint32_t name_offset;
  std::uint32_t name_length;
};
static_assert(sizeof(ChannelInfo) == 32);

struct ChannelSlot {
  std::uint16_t fingerprint;
  std::uint16_t channel_id_plus_one; // 0は空スロット
};
static_assert(sizeof(ChannelSlot) == 4);

class ChannelDictionary {
public:
  explicit ChannelDictionary(std::size_t name_pool_capacity)
      : name_pool_capacity_(name_pool_capacity) {
    if (name_pool_capacity_ == 0 ||
        name_pool_capacity_ > std::numeric_limits<std::uint32_t>::max()) {
      fatal("name pool capacity must fit in uint32_t offsets");
    }

    // name poolは巨大かつ疎なのでTHPを付けません。
    name_pool_ = static_cast<char *>(map_zeroed_bytes(name_pool_capacity_));
    infos_ = map_zeroed_array<ChannelInfo>(kMaxChannelCount);
    slots_ = map_zeroed_array<ChannelSlot>(kChannelSlotAllocation);
  }

  ChannelDictionary(const ChannelDictionary &) = delete;
  ChannelDictionary &operator=(const ChannelDictionary &) = delete;

  ~ChannelDictionary() {
    unmap_array(slots_, kChannelSlotAllocation);
    unmap_array(infos_, kMaxChannelCount);
    if (name_pool_ != nullptr) {
      (void)munmap(name_pool_, name_pool_capacity_);
    }
  }

  FORCE_INLINE const char *slot_bytes() const noexcept {
    return reinterpret_cast<const char *>(slots_);
  }

  FORCE_INLINE std::uint32_t find_or_insert(const char *name_begin,
                                            const ChannelScanResult &key) {
    if (UNLIKELY(key.length > std::numeric_limits<std::uint32_t>::max())) {
      fatal("channel name too long");
    }
    return find_or_insert_impl(name_begin, static_cast<std::uint32_t>(key.length),
                               key.hash, key.key0, key.key1);
  }

  FORCE_INLINE std::uint32_t find_or_insert(const char *name_begin,
                                            const ChannelInfo &key) {
    return find_or_insert_impl(name_begin, key.name_length, key.hash, key.key0,
                               key.key1);
  }

  FORCE_INLINE std::uint32_t
  find_validated_payload(std::uint64_t hash) const noexcept {
#if XOR_RETRIEVAL_LOOKUP
    const std::uint32_t mixed = xor_retrieval_hash(hash);
    const std::uint16_t *const retrieval =
        reinterpret_cast<const std::uint16_t *>(slots_);
    return static_cast<std::uint32_t>(
        retrieval[mixed & kXorRetrievalLeftMask] ^
        retrieval[kXorRetrievalLeftSize +
                  ((mixed >> XOR_RETRIEVAL_LEFT_BITS) &
                   kXorRetrievalRightMask)]);
#elif VALIDATED_POINTER_PROBE && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const std::uint16_t fingerprint = validated_channel_fingerprint(hash);
    const ChannelSlot *slot = slots_ + initial_index(hash);
    for (;;) {
      std::uint32_t packed_slot;
      std::memcpy(&packed_slot, slot, sizeof(packed_slot));
      if (LIKELY(static_cast<std::uint16_t>(packed_slot) == fingerprint)) {
        return packed_slot >> 16;
      }
      ++slot;
    }
#else
    std::size_t index = initial_index(hash);

#if TIGHT_CRC_CODEGEN && SHORT_HASH_MODE == 4 && HAS_X86_ROW_SIMD &&            \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    std::uint32_t packed_slot;
    std::memcpy(&packed_slot, slots_ + index, sizeof(packed_slot));
    if (LIKELY(static_cast<std::uint16_t>(packed_slot) ==
               static_cast<std::uint16_t>(hash))) {
      return packed_slot >> 16;
    }

    // 約3%のfirst-slot missが起きてからfingerprintを保存し、通常hitから
    // register copyを除きます。
    const std::uint16_t fingerprint = static_cast<std::uint16_t>(hash);
    for (;;) {
      index = (index + 1u) & (kChannelTableCapacity - 1u);
      std::memcpy(&packed_slot, slots_ + index, sizeof(packed_slot));
      if (LIKELY(static_cast<std::uint16_t>(packed_slot) == fingerprint)) {
        return packed_slot >> 16;
      }
    }
#else
    const std::uint16_t fingerprint = validated_channel_fingerprint(hash);
    for (;;) {
      std::uint32_t packed_slot;
      std::memcpy(&packed_slot, slots_ + index, sizeof(packed_slot));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      if (LIKELY(static_cast<std::uint16_t>(packed_slot) == fingerprint)) {
        return packed_slot >> 16;
      }
#else
      const ChannelSlot &slot = slots_[index];
      if (LIKELY(slot.fingerprint == fingerprint)) {
        return static_cast<std::uint32_t>(slot.channel_id_plus_one);
      }
#endif
      // exactly 10,000 channelをfull-keyで発見済みなので、後続hashには必ず
      // 対応slotがあります。empty判定を省き1回の32-bit loadだけでprobeします。
      index = (index + 1u) & (kChannelTableCapacity - 1u);
    }
#endif
#endif
  }

  FORCE_INLINE std::uint32_t
  find_validated(std::uint64_t hash) const noexcept {
    return find_validated_payload(hash) - 1u;
  }

  bool finalize_validated_fingerprints() noexcept {
    if (size_ != kMaxChannelCount) {
      return false;
    }

    // exact辞書のprobe配置とIDは維持し、fingerprintだけを書き換えます。
    for (std::size_t index = 0; index < kChannelTableCapacity; ++index) {
      ChannelSlot &slot = slots_[index];
      if (slot.channel_id_plus_one != 0) {
        const std::uint32_t channel_id = slot.channel_id_plus_one - 1u;
        if (channel_id >= size_) {
          return false;
        }
        slot.fingerprint =
            validated_channel_fingerprint(infos_[channel_id].hash);
      }
    }

#if VALIDATED_POINTER_PROBE
    refresh_validated_probe_guard();
#endif

    // workerが使うものと同じlookupを全既知channelへ適用します。
    // 先行slotのfingerprint ambiguityやfull-hash collisionもここで検出します。
    for (std::uint32_t channel_id = 0; channel_id < size_; ++channel_id) {
#if VALIDATED_POINTER_PROBE
      const bool valid = validated_pointer_probe_matches(
          infos_[channel_id].hash, channel_id + 1u);
#else
      const bool valid = find_validated(infos_[channel_id].hash) == channel_id;
#endif
      if (!valid) {
        // private exact fallbackを継続できるようlow16 fingerprintへ戻します。
        restore_exact_fingerprints();
        return false;
      }
    }
    return true;
  }

  bool finalize_private_saturated_lookup() noexcept {
#if XOR_RETRIEVAL_LOOKUP
    return size_ == kMaxChannelCount && build_xor_retrieval();
#else
    if (!finalize_validated_fingerprints()) {
      return false;
    }
    for (std::size_t index = 0; index < kChannelTableCapacity; ++index) {
      ChannelSlot &slot = slots_[index];
      if (slot.channel_id_plus_one != 0) {
        const std::uint32_t channel_id = slot.channel_id_plus_one - 1u;
        slot.channel_id_plus_one =
            static_cast<std::uint16_t>(channel_id * 3u + 1u);
      }
    }
#if VALIDATED_POINTER_PROBE
    refresh_validated_probe_guard();
#endif
    return true;
#endif
  }

  FORCE_INLINE const ChannelInfo &
  info(std::uint32_t channel_id) const noexcept {
    return infos_[channel_id];
  }

  FORCE_INLINE const char *name_data(const ChannelInfo &info) const noexcept {
    return name_pool_ + info.name_offset;
  }

  std::size_t size() const noexcept { return size_; }

private:
#if XOR_RETRIEVAL_LOOKUP
  bool build_xor_retrieval() noexcept {
    struct RetrievalEdge {
      std::uint16_t left;
      std::uint16_t right;
      std::uint16_t payload;
    };
    struct PeelEntry {
      std::uint16_t edge;
      std::uint16_t vertex;
    };

    std::vector<RetrievalEdge> edges;
    edges.reserve(size_);
    std::vector<std::uint16_t> degree(kXorRetrievalVertexCount, 0);
    std::vector<std::uint16_t> edge_xor(kXorRetrievalVertexCount, 0);

    for (std::uint32_t channel_id = 0; channel_id < size_; ++channel_id) {
      const std::uint32_t mixed = xor_retrieval_hash(infos_[channel_id].hash);
      const std::uint16_t left =
          static_cast<std::uint16_t>(mixed & kXorRetrievalLeftMask);
      const std::uint16_t right = static_cast<std::uint16_t>(
          (mixed >> XOR_RETRIEVAL_LEFT_BITS) & kXorRetrievalRightMask);
      const std::uint16_t edge_id =
          static_cast<std::uint16_t>(edges.size());
      edges.push_back(RetrievalEdge{
          left, right, static_cast<std::uint16_t>(channel_id * 3u + 1u)});
      const std::size_t left_vertex = left;
      const std::size_t right_vertex = kXorRetrievalLeftSize + right;
      ++degree[left_vertex];
      ++degree[right_vertex];
      edge_xor[left_vertex] ^= edge_id;
      edge_xor[right_vertex] ^= edge_id;
    }

    std::vector<std::uint16_t> queue;
    queue.reserve(kXorRetrievalVertexCount);
    for (std::uint32_t vertex = 0; vertex < kXorRetrievalVertexCount;
         ++vertex) {
      if (degree[vertex] == 1) {
        queue.push_back(static_cast<std::uint16_t>(vertex));
      }
    }

    std::vector<std::uint8_t> removed(edges.size(), 0);
    std::vector<PeelEntry> order;
    order.reserve(edges.size());
    std::size_t queue_index = 0;
    while (queue_index < queue.size()) {
      const std::uint16_t vertex = queue[queue_index++];
      if (degree[vertex] != 1) {
        continue;
      }
      const std::uint16_t edge_id = edge_xor[vertex];
      if (removed[edge_id]) {
        continue;
      }
      removed[edge_id] = 1;
      order.push_back(PeelEntry{edge_id, vertex});
      const RetrievalEdge &edge = edges[edge_id];
      const std::uint16_t vertices[2] = {
          edge.left,
          static_cast<std::uint16_t>(kXorRetrievalLeftSize + edge.right)};
      for (const std::uint16_t endpoint : vertices) {
        edge_xor[endpoint] ^= edge_id;
        if (degree[endpoint] != 0) {
          --degree[endpoint];
          if (degree[endpoint] == 1) {
            queue.push_back(endpoint);
          }
        }
      }
    }
    if (order.size() != edges.size()) {
      return false;
    }

    std::vector<std::uint16_t> retrieval(kXorRetrievalVertexCount, 0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const RetrievalEdge &edge = edges[it->edge];
      const std::uint16_t left_vertex = edge.left;
      const std::uint16_t right_vertex =
          static_cast<std::uint16_t>(kXorRetrievalLeftSize + edge.right);
      const std::uint16_t other =
          it->vertex == left_vertex ? right_vertex : left_vertex;
      retrieval[it->vertex] = edge.payload ^ retrieval[other];
    }

    for (std::uint32_t channel_id = 0; channel_id < size_; ++channel_id) {
      const std::uint32_t mixed = xor_retrieval_hash(infos_[channel_id].hash);
      const std::uint16_t payload =
          retrieval[mixed & kXorRetrievalLeftMask] ^
          retrieval[kXorRetrievalLeftSize +
                    ((mixed >> XOR_RETRIEVAL_LEFT_BITS) &
                     kXorRetrievalRightMask)];
      if (payload != static_cast<std::uint16_t>(channel_id * 3u + 1u)) {
        return false;
      }
    }

    static_assert(kXorRetrievalVertexCount * sizeof(std::uint16_t) <=
                  kChannelTableCapacity * sizeof(ChannelSlot));
    std::memcpy(slots_, retrieval.data(),
                kXorRetrievalVertexCount * sizeof(std::uint16_t));
    return true;
  }
#endif

#if VALIDATED_POINTER_PROBE
  void refresh_validated_probe_guard() noexcept {
    std::memcpy(slots_ + kChannelTableCapacity, slots_,
                kValidatedProbeGuard * sizeof(ChannelSlot));
  }

  bool validated_pointer_probe_matches(std::uint64_t hash,
                                       std::uint32_t expected_payload) const
      noexcept {
    const std::uint16_t fingerprint = validated_channel_fingerprint(hash);
    const ChannelSlot *slot = slots_ + initial_index(hash);
    const ChannelSlot *const end = slots_ + kChannelSlotAllocation;
    while (slot != end) {
      std::uint32_t packed_slot;
      std::memcpy(&packed_slot, slot, sizeof(packed_slot));
      if (static_cast<std::uint16_t>(packed_slot) == fingerprint) {
        return (packed_slot >> 16) == expected_payload;
      }
      ++slot;
    }
    return false;
  }
#endif

  void restore_exact_fingerprints() noexcept {
    for (std::size_t index = 0; index < kChannelTableCapacity; ++index) {
      ChannelSlot &slot = slots_[index];
      if (slot.channel_id_plus_one != 0) {
        const std::uint32_t channel_id = slot.channel_id_plus_one - 1u;
        slot.fingerprint = static_cast<std::uint16_t>(
            channel_fingerprint(infos_[channel_id].hash));
      }
    }
  }

  FORCE_INLINE std::size_t initial_index(std::uint64_t hash) const noexcept {
    // scannerのfinal hashは既にmultiply-mix済みです。
    return channel_initial_index(hash);
  }

  FORCE_INLINE bool exact_match(const ChannelInfo &info, const char *name_begin,
                                std::uint32_t length, std::uint64_t key0,
                                std::uint64_t key1) const noexcept {
#if HAS_X86_ROW_SIMD
    if (info.name_length != length || info.key0 != key0 || info.key1 != key1) {
      return false;
    }
    if (length <= 16) {
      return true;
    }
#else
    if (info.name_length != length || info.key0 != key0) {
      return false;
    }
    if (length <= 8) {
      return true;
    }
    if (info.key1 != key1) {
      return false;
    }
    if (length <= 16) {
      return true;
    }
#endif
    // Public分布の長いchannel（17～32 bytes）では、既に一致済みの先頭
    // 16 bytesをmemcmpで再比較せず、channel内に収まる64-bit loadだけで
    // suffixを完全比較します。mapping末尾をoverreadしません。
    const char *const stored = name_pool_ + info.name_offset;
    if (read_u64(stored + length - 8u) !=
        read_u64(name_begin + length - 8u)) {
      return false;
    }
    if (length <= 24u) {
      return true;
    }
    if (read_u64(stored + 16u) != read_u64(name_begin + 16u)) {
      return false;
    }
    if (length <= 32u) {
      return true;
    }

    // 先頭24 bytesと末尾8 bytesは比較済みなので、残る中央部だけを比較します。
    return std::memcmp(stored + 24u, name_begin + 24u, length - 32u) == 0;
  }

  FORCE_INLINE std::uint32_t
  find_or_insert_impl(const char *name_begin, std::uint32_t length,
                      std::uint64_t hash, std::uint64_t key0,
                      std::uint64_t key1) {
    const std::uint16_t fingerprint =
        static_cast<std::uint16_t>(channel_fingerprint(hash));
    std::size_t index = initial_index(hash);

    for (;;) {
      ChannelSlot &slot = slots_[index];

      if (slot.channel_id_plus_one == 0) {
        return insert_new(slot, index, name_begin, length, hash, key0, key1,
                          fingerprint);
      }

      if (slot.fingerprint == fingerprint) {
        const std::uint32_t channel_id = slot.channel_id_plus_one - 1;
        const ChannelInfo &info = infos_[channel_id];
        if (exact_match(info, name_begin, length, key0, key1)) {
          return channel_id;
        }
      }

      index = (index + 1) & (kChannelTableCapacity - 1);
    }
  }

  std::uint32_t insert_new(ChannelSlot &slot, std::size_t slot_index,
                           const char *name_begin, std::uint32_t length,
                           std::uint64_t hash, std::uint64_t key0,
                           std::uint64_t key1, std::uint16_t fingerprint) {
    if (UNLIKELY(size_ >= kMaxChannelCount)) {
      fatal("channel count exceeded 10,000");
    }
    if (UNLIKELY(name_pool_used_ + length > name_pool_capacity_)) {
      fatal("channel name pool capacity exceeded");
    }

    const std::uint32_t offset = static_cast<std::uint32_t>(name_pool_used_);
    std::memcpy(name_pool_ + offset, name_begin, length);
    name_pool_used_ += length;

    const std::uint32_t channel_id = static_cast<std::uint32_t>(size_);
    infos_[channel_id] = ChannelInfo{hash, key0, key1, offset, length};

    (void)slot_index;
    slot.fingerprint = fingerprint;
    slot.channel_id_plus_one = static_cast<std::uint16_t>(channel_id + 1);
    ++size_;
    return channel_id;
  }

  ChannelSlot *slots_ = nullptr;
  std::size_t size_ = 0;

  char *name_pool_ = nullptr;
  std::size_t name_pool_capacity_ = 0;
  std::size_t name_pool_used_ = 0;

  ChannelInfo *infos_ = nullptr;
};

// -----------------------------------------------------------------------------
// 月スロット・集計値
// -----------------------------------------------------------------------------

struct AggregateValue {
  // 追加の入力保証により、各channel-monthの合計は32ビットに収まります。
  // 正の値しか加算しないため途中経過も32 bitsを超えません。carryなしで
  // 2系列を1回の64-bit加算にまとめられます。
  std::uint64_t packed_totals;

  // [63:47] max、[46:30] min、[29:0] count。
  std::uint64_t count_min_max;

  static constexpr unsigned kCountBits = 30;
  static constexpr unsigned kLengthBits = 17;
  static constexpr unsigned kMinShift = kCountBits;
  static constexpr unsigned kMaxShift = kCountBits + kLengthBits;
  static constexpr std::uint64_t kCountMask =
      (std::uint64_t{1} << kCountBits) - 1u;
  static constexpr std::uint64_t kLengthMask =
      (std::uint64_t{1} << kLengthBits) - 1u;
  static constexpr std::uint64_t kMinFieldMask = kLengthMask << kMinShift;
  static constexpr std::uint64_t kMaxFieldMask = kLengthMask << kMaxShift;

  FORCE_INLINE std::uint32_t total_message_length_value() const noexcept {
    return static_cast<std::uint32_t>(packed_totals);
  }

  FORCE_INLINE std::uint32_t total_stamp_count_value() const noexcept {
    return static_cast<std::uint32_t>(packed_totals >> 32);
  }

  FORCE_INLINE std::uint32_t message_count() const noexcept {
    return static_cast<std::uint32_t>(count_min_max & kCountMask);
  }

  FORCE_INLINE std::uint32_t min_message_length() const noexcept {
    return static_cast<std::uint32_t>((count_min_max >> kMinShift) &
                                      kLengthMask);
  }

  FORCE_INLINE std::uint32_t max_message_length() const noexcept {
    return static_cast<std::uint32_t>(count_min_max >> kMaxShift);
  }

  FORCE_INLINE static std::uint64_t pack(std::uint32_t count,
                                         std::uint32_t min_length,
                                         std::uint32_t max_length) noexcept {
    return static_cast<std::uint64_t>(count) |
           (static_cast<std::uint64_t>(min_length) << kMinShift) |
           (static_cast<std::uint64_t>(max_length) << kMaxShift);
  }

#if COLD_REEXTRACT_EXTREMA
  [[gnu::noinline, gnu::cold]] static std::uint64_t
  replace_extrema(std::uint64_t updated,
                  std::uint32_t message_length) noexcept {
    const std::uint32_t min_length =
        static_cast<std::uint32_t>((updated >> kMinShift) & kLengthMask);
    const std::uint32_t max_length =
        static_cast<std::uint32_t>(updated >> kMaxShift);
#else
  [[gnu::noinline, gnu::cold]] static std::uint64_t replace_extrema(
      std::uint64_t updated, std::uint32_t message_length,
      std::uint32_t min_length, std::uint32_t max_length) noexcept {
#endif
#if COLD_FIRST_AGGREGATE
    if ((updated & kCountMask) == 1u) {
      return pack(1u, message_length, message_length);
    }
#endif
    if (message_length < min_length) {
      updated = (updated & ~kMinFieldMask) |
                (static_cast<std::uint64_t>(message_length) << kMinShift);
    }
    if (message_length > max_length) {
      updated = (updated & ~kMaxFieldMask) |
                (static_cast<std::uint64_t>(message_length) << kMaxShift);
    }
    return updated;
  }

  FORCE_INLINE void add(std::uint32_t message_length,
                        std::uint32_t stamp_count) noexcept {
    const std::uint64_t metadata = count_min_max;
#if !COLD_FIRST_AGGREGATE
    const std::uint32_t count =
        static_cast<std::uint32_t>(metadata & kCountMask);
#endif

    packed_totals += static_cast<std::uint64_t>(message_length) |
                     (static_cast<std::uint64_t>(stamp_count) << 32);

#if !COLD_FIRST_AGGREGATE
    if (UNLIKELY(count == 0)) {
      count_min_max = pack(1u, message_length, message_length);
      return;
    }
#endif

    // count fieldは30 bitsで、全入力行数1Bより大きいためcarryしません。
    std::uint64_t updated = metadata + 1u;
    const std::uint32_t min_length =
        static_cast<std::uint32_t>((metadata >> kMinShift) & kLengthMask);
    const std::uint32_t max_length =
        static_cast<std::uint32_t>(metadata >> kMaxShift);

    // random sequenceでは新しいextremeはO(log n)回だけです。cold helperを
    // 条件付きcallにして、compilerがhot pathへcmovを戻すことを防ぎます。
    if (UNLIKELY(message_length < min_length ||
                 message_length > max_length)) {
#if COLD_REEXTRACT_EXTREMA
      updated = replace_extrema(updated, message_length);
#else
      updated =
          replace_extrema(updated, message_length, min_length, max_length);
#endif
    }
    count_min_max = updated;
  }

  FORCE_INLINE void merge(const AggregateValue &other) noexcept {
    const std::uint32_t other_count = other.message_count();
    if (other_count == 0) {
      return;
    }

    const std::uint32_t count = message_count();
    if (count == 0) {
      *this = other;
      return;
    }

    packed_totals += other.packed_totals;

    const std::uint32_t min_length =
        other.min_message_length() < min_message_length()
            ? other.min_message_length()
            : min_message_length();
    const std::uint32_t max_length =
        other.max_message_length() > max_message_length()
            ? other.max_message_length()
            : max_message_length();

    count_min_max = pack(count + other_count, min_length, max_length);
  }
};
static_assert(sizeof(AggregateValue) == 16);
static_assert(1'000'000'000u <
              (std::uint64_t{1} << AggregateValue::kCountBits));
static_assert(99'999u <= AggregateValue::kLengthMask);

class DenseAggregateArena {
public:
  static constexpr std::size_t kValueCount =
      kDenseChannelCapacity * kMonthCount;

  DenseAggregateArena()
      : values_(map_zeroed_array<AggregateValue>(kValueCount, true)) {}
  DenseAggregateArena(const DenseAggregateArena &) = delete;
  DenseAggregateArena &operator=(const DenseAggregateArena &) = delete;
  ~DenseAggregateArena() { unmap_array(values_, kValueCount); }

  FORCE_INLINE AggregateValue &value(
      std::uint32_t channel_id, TimestampMonthIndex month_index) noexcept {
    return values_[static_cast<std::size_t>(channel_id) * kMonthCount +
                   month_index];
  }

  FORCE_INLINE AggregateValue &value_from_scaled_payload(
      std::uint32_t payload, TimestampMonthIndex month_index) noexcept {
    return values_[(static_cast<std::size_t>(payload) - 1u) * 4u +
                   month_index];
  }

  FORCE_INLINE const AggregateValue &value(
      std::uint32_t channel_id, TimestampMonthIndex month_index) const noexcept {
    return values_[static_cast<std::size_t>(channel_id) * kMonthCount +
                   month_index];
  }

private:
  AggregateValue *values_;
};

struct alignas(64) WorkerState {
  WorkerState() : channels(kLocalNamePoolCapacity) {}

  ChannelDictionary channels;
  DenseAggregateArena aggregates;
};

// -----------------------------------------------------------------------------
// 入力mmap・スレッド分割
// -----------------------------------------------------------------------------

struct MappedInput {
  const char *data = nullptr;
  std::size_t size = 0;
};

MappedInput map_input_file(const char *path) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fatal_errno("open input");
  }

  struct stat st{};
  if (fstat(fd, &st) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    fatal_errno("fstat input");
  }
  if (st.st_size <= 0) {
    close(fd);
    fatal("input file is empty");
  }

#if defined(POSIX_FADV_SEQUENTIAL)
  (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

  const std::size_t size = static_cast<std::size_t>(st.st_size);
  void *mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  const int saved = errno;
  close(fd);

  if (mapped == MAP_FAILED) {
    errno = saved;
    fatal_errno("mmap input");
  }

#ifdef MADV_DONTDUMP
  (void)madvise(mapped, size, MADV_DONTDUMP);
#endif

  return {static_cast<const char *>(mapped), size};
}

std::vector<const char *> make_boundaries(const char *payload_begin,
                                          const char *end,
                                          std::size_t worker_count) {
  std::vector<const char *> boundaries(worker_count + 1);
  boundaries.front() = payload_begin;
  boundaries.back() = end;

  const std::size_t payload_size =
      static_cast<std::size_t>(end - payload_begin);

  for (std::size_t i = 1; i < worker_count; ++i) {
    const char *p = payload_begin + payload_size * i / worker_count;
    while (p < end && *p != '\n') {
      ++p;
    }
    if (p < end) {
      ++p;
    }
    boundaries[i] = p;
  }

  return boundaries;
}

std::vector<int> available_cpus() {
  std::vector<int> cpus;
  cpu_set_t set;
  CPU_ZERO(&set);

  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &set)) {
        cpus.push_back(cpu);
      }
    }
  }

  return cpus;
}

void pin_current_thread(int cpu) noexcept {
  if (cpu < 0) {
    return;
  }

  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

struct ParsedNumericFields {
  std::uint32_t message_length;
  std::uint32_t stamp_count;
  const char *next;
};

FORCE_INLINE ParsedNumericFields parse_numeric_fields(const char *p,
                                                      const char *end) noexcept {
  // 最頻の「message_length=2〜3桁、stamp_count=1桁、LF」を先に処理します。
  // 2/3桁の選択は分岐せず、予測困難な桁数branchをhot pathから除きます。
  if (LIKELY(static_cast<std::size_t>(end - p) >= 6)) {
    const std::uint32_t first_two =
        static_cast<std::uint32_t>(p[0] - '0') * 10u +
        static_cast<std::uint32_t>(p[1] - '0');
    const bool common_digits = p[2] == ',' || p[3] == ',';
    const std::uint32_t has_third_digit = static_cast<std::uint32_t>(p[2] != ',');
    const std::uint32_t message_length =
        first_two + has_third_digit *
                        (first_two * 9u + static_cast<std::uint32_t>(p[2] - '0'));
    const char *const stamp = p + 3 + has_third_digit;

    if (LIKELY(common_digits && stamp[1] == '\n')) {
      return {message_length, static_cast<std::uint32_t>(stamp[0] - '0'),
              stamp + 2};
    }

  }

  const std::uint32_t message_length = decode_uint_1_to_5_digits(p);
  ++p; // comma
  const std::uint32_t stamp_count = decode_uint_1_to_5_digits(p);
  ++p; // LF
  return {message_length, stamp_count, p};
}

[[gnu::noinline, gnu::hot]] void
parse_range_private_validated(const char *pos, const char *end,
                              WorkerState &state) {
#if DEFER_VALIDATED_AGGREGATE == 1
  AggregateValue *pending_value = nullptr;
  std::uint64_t pending_numeric = 0;
#elif DEFER_VALIDATED_AGGREGATE > 1
  static_assert((DEFER_VALIDATED_AGGREGATE &
                 (DEFER_VALIDATED_AGGREGATE - 1)) == 0,
                "deferred aggregate distance must be a power of two");
#if DEFER_AGGREGATE_SOA_RING
  constexpr std::size_t kPendingRingSize =
      DEFER_VALIDATED_AGGREGATE *
      (DEFER_AGGREGATE_EARLY_PREFETCH ? 2u : 1u);
  std::array<AggregateValue *, kPendingRingSize> pending_values{};
  std::array<std::uint64_t, kPendingRingSize> pending_numerics{};
  std::size_t pending_index = 0;
#else
  struct PendingAggregateUpdate {
    AggregateValue *value = nullptr;
    std::uint64_t numeric = 0;
  };
  std::array<PendingAggregateUpdate, DEFER_VALIDATED_AGGREGATE>
      pending_updates{};
#if DEFER_AGGREGATE_BYTE_OFFSET
  static_assert(sizeof(PendingAggregateUpdate) == 16);
  std::uint32_t pending_byte_offset = 0;
#else
  std::size_t pending_index = 0;
#endif
#endif
#endif

  while (pos < end) {
    pos += kTimestampLength + 1;

    const char *const channel_begin = pos;
    std::uint32_t fast_message_length;
    std::uint32_t fast_stamp_count;
    FastRowOffset fast_row_bytes;
    const ChannelScanResult channel = scan_channel_and_hash(
        pos, end, state.channels.slot_bytes(),
        fast_message_length, fast_stamp_count, fast_row_bytes);
    const char *const numeric_begin = channel_begin + channel.length + 1;

    ParsedNumericFields fields;
#if HAS_X86_ROW_SIMD
    if (LIKELY(fast_row_bytes != 0)) {
      fields.message_length = fast_message_length;
      fields.stamp_count = fast_stamp_count;
      fields.next = channel_begin + fast_row_bytes;
    } else {
      fields = parse_numeric_fields(numeric_begin, end);
    }
#else
    fields = parse_numeric_fields(numeric_begin, end);
#endif
    pos = fields.next;

#if DEFER_VALIDATED_AGGREGATE == 1
    if (LIKELY(pending_value != nullptr)) {
      pending_value->add(static_cast<std::uint32_t>(pending_numeric),
                         static_cast<std::uint32_t>(pending_numeric >> 32));
    }
#elif DEFER_VALIDATED_AGGREGATE > 1
#if DEFER_AGGREGATE_SOA_RING
#if !DEFER_AGGREGATE_EARLY_PREFETCH
    AggregateValue *const pending_value = pending_values[pending_index];
    if (LIKELY(pending_value != nullptr)) {
      const std::uint64_t pending_numeric = pending_numerics[pending_index];
      pending_value->add(static_cast<std::uint32_t>(pending_numeric),
                         static_cast<std::uint32_t>(pending_numeric >> 32));
    }
#endif
#else
#if DEFER_AGGREGATE_BYTE_OFFSET
    PendingAggregateUpdate &pending_read =
        *reinterpret_cast<PendingAggregateUpdate *>(
            reinterpret_cast<unsigned char *>(pending_updates.data()) +
            pending_byte_offset);
#else
    PendingAggregateUpdate &pending_read = pending_updates[pending_index];
#endif
    if (LIKELY(pending_read.value != nullptr)) {
      pending_read.value->add(
          static_cast<std::uint32_t>(pending_read.numeric),
          static_cast<std::uint32_t>(pending_read.numeric >> 32));
    }
#endif
#endif

    const TimestampMonthIndex month_index = timestamp_to_month_index(
        channel_begin - (kTimestampLength + 1u));

    const std::uint32_t payload =
        state.channels.find_validated_payload(channel.hash);
    AggregateValue &value =
        state.aggregates.value_from_scaled_payload(payload, month_index);
#if DEFER_VALIDATED_AGGREGATE == 1
    __builtin_prefetch(&value, 1, 3);
    pending_value = &value;
    pending_numeric = static_cast<std::uint64_t>(fields.message_length) |
                      (static_cast<std::uint64_t>(fields.stamp_count) << 32);
#elif DEFER_VALIDATED_AGGREGATE > 1
    __builtin_prefetch(&value, 1, 3);
    const std::uint64_t packed_numeric =
        static_cast<std::uint64_t>(fields.message_length) |
        (static_cast<std::uint64_t>(fields.stamp_count) << 32);
#if DEFER_AGGREGATE_SOA_RING
    pending_values[pending_index] = &value;
    pending_numerics[pending_index] = packed_numeric;
#if DEFER_AGGREGATE_EARLY_PREFETCH
    const std::size_t retire_index =
        pending_index ^ DEFER_VALIDATED_AGGREGATE;
    AggregateValue *const retire_value = pending_values[retire_index];
    if (LIKELY(retire_value != nullptr)) {
      const std::uint64_t retire_numeric = pending_numerics[retire_index];
      retire_value->add(static_cast<std::uint32_t>(retire_numeric),
                        static_cast<std::uint32_t>(retire_numeric >> 32));
    }
#endif
    pending_index = (pending_index + 1u) & (kPendingRingSize - 1u);
#else
#if DEFER_AGGREGATE_BYTE_OFFSET
    PendingAggregateUpdate &pending_write =
        *reinterpret_cast<PendingAggregateUpdate *>(
            reinterpret_cast<unsigned char *>(pending_updates.data()) +
            pending_byte_offset);
#else
    PendingAggregateUpdate &pending_write = pending_updates[pending_index];
#endif
    pending_write.value = &value;
    pending_write.numeric = packed_numeric;
#if DEFER_AGGREGATE_BYTE_OFFSET
    pending_byte_offset =
        (pending_byte_offset + sizeof(PendingAggregateUpdate)) &
        (sizeof(PendingAggregateUpdate) * DEFER_VALIDATED_AGGREGATE - 1u);
#else
    pending_index =
        (pending_index + 1u) & (DEFER_VALIDATED_AGGREGATE - 1u);
#endif
#endif
#else
    value.add(fields.message_length, fields.stamp_count);
#endif
  }

#if DEFER_VALIDATED_AGGREGATE == 1
  if (pending_value != nullptr) {
    pending_value->add(static_cast<std::uint32_t>(pending_numeric),
                       static_cast<std::uint32_t>(pending_numeric >> 32));
  }
#elif DEFER_VALIDATED_AGGREGATE > 1
#if DEFER_AGGREGATE_SOA_RING
#if DEFER_AGGREGATE_EARLY_PREFETCH
  for (std::size_t age = 1; age <= DEFER_VALIDATED_AGGREGATE; ++age) {
    const std::size_t i = (pending_index - age) & (kPendingRingSize - 1u);
    if (pending_values[i] != nullptr) {
      pending_values[i]->add(static_cast<std::uint32_t>(pending_numerics[i]),
                             static_cast<std::uint32_t>(pending_numerics[i] >> 32));
    }
  }
#else
  for (std::size_t i = 0; i < DEFER_VALIDATED_AGGREGATE; ++i) {
    if (pending_values[i] != nullptr) {
      pending_values[i]->add(static_cast<std::uint32_t>(pending_numerics[i]),
                             static_cast<std::uint32_t>(pending_numerics[i] >> 32));
    }
  }
#endif
#else
  for (const PendingAggregateUpdate &pending : pending_updates) {
    if (pending.value != nullptr) {
      pending.value->add(static_cast<std::uint32_t>(pending.numeric),
                         static_cast<std::uint32_t>(pending.numeric >> 32));
    }
  }
#endif
#endif
}

[[gnu::noinline]] void parse_range_private_exact(const char *pos,
                                                 const char *end,
                                                 WorkerState &state) {
  bool allow_saturated_transition = true;
  while (pos < end) {
    pos += kTimestampLength + 1;

    const char *const channel_begin = pos;
    std::uint32_t fast_message_length;
    std::uint32_t fast_stamp_count;
    FastRowOffset fast_row_bytes;
    const ChannelScanResult channel = scan_channel_and_hash(
        pos, end, state.channels.slot_bytes(),
        fast_message_length, fast_stamp_count, fast_row_bytes);
    const char *const numeric_begin = channel_begin + channel.length + 1;

    ParsedNumericFields fields;
#if HAS_X86_ROW_SIMD
    if (LIKELY(fast_row_bytes != 0)) {
      fields.message_length = fast_message_length;
      fields.stamp_count = fast_stamp_count;
      fields.next = channel_begin + fast_row_bytes;
    } else {
      fields = parse_numeric_fields(numeric_begin, end);
    }
#else
    fields = parse_numeric_fields(numeric_begin, end);
#endif
    pos = fields.next;

    const TimestampMonthIndex month_index = timestamp_to_month_index(
        channel_begin - (kTimestampLength + 1u));

    const std::uint32_t channel_id =
        state.channels.find_or_insert(channel_begin, channel);
    state.aggregates.value(channel_id, month_index)
        .add(fields.message_length, fields.stamp_count);

    if (allow_saturated_transition &&
        UNLIKELY(state.channels.size() == kMaxChannelCount)) {
      if (state.channels.finalize_private_saturated_lookup()) {
        parse_range_private_validated(pos, end, state);
        return;
      }
      allow_saturated_transition = false;
    }
  }
}

void parse_range(const char *pos, const char *end, WorkerState &state) {
  parse_range_private_exact(pos, end, state);
}

// -----------------------------------------------------------------------------
// 最終統合
// -----------------------------------------------------------------------------

struct GlobalState {
  GlobalState() : channels(kGlobalNamePoolCapacity) {}

  ChannelDictionary channels;
  DenseAggregateArena aggregates;
};

void merge_worker(GlobalState &global, const WorkerState &local) {
  if (local.channels.size() > std::numeric_limits<std::uint32_t>::max()) {
    fatal("local channel count exceeded uint32_t range");
  }

  for (std::uint32_t local_id = 0;
       local_id < static_cast<std::uint32_t>(local.channels.size());
       ++local_id) {
    const ChannelInfo &info = local.channels.info(local_id);
    const char *name = local.channels.name_data(info);
    const std::uint32_t global_id = global.channels.find_or_insert(name, info);

    for (std::uint8_t month = 0; month < kMonthCount; ++month) {
      const AggregateValue &source = local.aggregates.value(local_id, month);
      if (source.message_count() != 0) {
        global.aggregates.value(global_id, month).merge(source);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// 高速出力
// -----------------------------------------------------------------------------

class FastFdOutput {
public:
  explicit FastFdOutput(int fd) : fd_(fd), buffer_(kOutputBufferSize) {}

  FastFdOutput(const FastFdOutput &) = delete;
  FastFdOutput &operator=(const FastFdOutput &) = delete;

  ~FastFdOutput() { (void)flush(); }

  bool flush() noexcept {
    if (failed_ || used_ == 0) {
      return !failed_;
    }

    std::size_t written = 0;
    while (written < used_) {
      const ssize_t result =
          ::write(fd_, buffer_.data() + written, used_ - written);
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        failed_ = true;
        return false;
      }
      written += static_cast<std::size_t>(result);
    }

    used_ = 0;
    return true;
  }

  void write_bytes(const char *data, std::size_t length) noexcept {
    if (failed_ || length == 0) {
      return;
    }

    if (length > buffer_.size() - used_) {
      if (!flush()) {
        return;
      }
      if (length >= buffer_.size()) {
        std::size_t written = 0;
        while (written < length) {
          const ssize_t result = ::write(fd_, data + written, length - written);
          if (result < 0) {
            if (errno == EINTR) {
              continue;
            }
            failed_ = true;
            return;
          }
          written += static_cast<std::size_t>(result);
        }
        return;
      }
    }

    std::memcpy(buffer_.data() + used_, data, length);
    used_ += length;
  }

  FORCE_INLINE void put(char c) noexcept {
    if (UNLIKELY(used_ == buffer_.size())) {
      if (!flush()) {
        return;
      }
    }
    buffer_[used_++] = c;
  }

  void write_u64(std::uint64_t value) noexcept {
    char temporary[32];
    char *const end = temporary + sizeof(temporary);
    char *p = end;

    do {
      *--p = static_cast<char>('0' + value % 10u);
      value /= 10u;
    } while (value != 0);

    write_bytes(p, static_cast<std::size_t>(end - p));
  }

  void write_year_month(std::uint8_t month_index) noexcept {
    const std::uint16_t encoded = kMonthTables.month_codes[month_index];
    const unsigned year = static_cast<unsigned>(encoded >> 4);
    const unsigned month = static_cast<unsigned>(encoded & 0x0f);

    write_u64(year);
    put('-');
    put(static_cast<char>('0' + month / 10));
    put(static_cast<char>('0' + month % 10));
  }

  void write_average_2dp(std::uint64_t total, std::uint32_t count) noexcept {
    const double average = static_cast<double>(total) / count;
#if FAST_AVERAGE_FORMAT
    // to_chars(fixed, 2)と同様に、計算済みdoubleの正確な値を
    // nearest-evenで小数第2位へ丸めます。入力保証からaverageはnormalかつ
    // [1, UINT32_MAX]なので、100倍した仮数はuint64_tへ収まります。
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(average);
    const std::uint64_t mantissa =
        (bits & ((std::uint64_t{1} << 52) - 1u)) |
        (std::uint64_t{1} << 52);
    const unsigned exponent =
        static_cast<unsigned>((bits >> 52) & 0x7ffu) - 1023u;
    const unsigned shift = 52u - exponent;
    const std::uint64_t numerator = mantissa * 100u;
    std::uint64_t scaled = numerator >> shift;
    const std::uint64_t remainder =
        numerator & ((std::uint64_t{1} << shift) - 1u);
    const std::uint64_t halfway = std::uint64_t{1} << (shift - 1u);
    if (remainder > halfway ||
        (remainder == halfway && (scaled & 1u) != 0)) {
      ++scaled;
    }

    write_u64(scaled / 100u);
    put('.');
    const unsigned fraction = static_cast<unsigned>(scaled % 100u);
    put(static_cast<char>('0' + fraction / 10u));
    put(static_cast<char>('0' + fraction % 10u));
#else
    char temporary[64];
    const auto result = std::to_chars(temporary, temporary + sizeof(temporary),
                                      average, std::chars_format::fixed, 2);
    if (UNLIKELY(result.ec != std::errc{})) {
      failed_ = true;
      return;
    }
    write_bytes(temporary, static_cast<std::size_t>(result.ptr - temporary));
#endif
  }

  bool failed() const noexcept { return failed_; }

private:
  int fd_;
  std::vector<char> buffer_;
  std::size_t used_ = 0;
  bool failed_ = false;
};

void write_output(int fd, const GlobalState &global) {
  FastFdOutput out(fd);

  for (std::uint32_t channel_id = 0;
       channel_id < static_cast<std::uint32_t>(global.channels.size());
       ++channel_id) {
    const ChannelInfo &channel = global.channels.info(channel_id);

    for (std::uint8_t month = 0; month < kMonthCount; ++month) {
      const AggregateValue &value = global.aggregates.value(channel_id, month);
      const std::uint32_t count = value.message_count();
      if (count == 0) {
        continue;
      }

      out.write_bytes(global.channels.name_data(channel), channel.name_length);
      out.put(',');
      out.write_year_month(month);
      out.put('=');
      out.write_u64(value.min_message_length());
      out.put('/');
      out.write_average_2dp(value.total_message_length_value(), count);
      out.put('/');
      out.write_u64(value.max_message_length());
      out.put('/');
      out.write_u64(count);
      out.put('/');
      out.write_u64(value.total_stamp_count_value());
      out.put('\n');
    }
  }

  if (!out.flush() || out.failed()) {
    fatal_errno("write output");
  }
}

} // namespace

int main(int argc, char **argv) {
#if PROFILE_PHASES
  const std::uint64_t profile_program_begin = profile_now_ns();
#endif
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s INPUT.csv [OUTPUT.txt]\n", argv[0]);
    return EXIT_FAILURE;
  }

  MappedInput input = map_input_file(argv[1]);
  if (input.size < kHeaderLength ||
      std::memcmp(input.data, kHeader, kHeaderLength) != 0) {
    (void)munmap(const_cast<char *>(input.data), input.size);
    fatal("unexpected CSV header");
  }
  if (input.data[input.size - 1] != '\n') {
    (void)munmap(const_cast<char *>(input.data), input.size);
    fatal("input must end with a newline");
  }
#if PROFILE_PHASES
  const std::uint64_t profile_input_ready = profile_now_ns();
#endif

  const std::vector<int> cpus = available_cpus();
  std::size_t worker_count = kRequestedWorkerCount;
  if (!cpus.empty() && worker_count > cpus.size()) {
    worker_count = cpus.size();
  }
  if (worker_count == 0) {
    worker_count = 1;
  }

  const char *const payload_begin = input.data + kHeaderLength;
  const char *const input_end = input.data + input.size;
  const std::vector<const char *> boundaries =
      make_boundaries(payload_begin, input_end, worker_count);

  auto global = std::make_unique<GlobalState>();
  std::vector<std::unique_ptr<WorkerState>> workers(worker_count);
  std::vector<std::thread> threads;
  threads.reserve(worker_count);
#if PROFILE_PHASES
  std::vector<std::uint64_t> worker_begin(worker_count);
  std::vector<std::uint64_t> worker_parse_begin(worker_count);
  std::vector<std::uint64_t> worker_parse_end(worker_count);
  const std::uint64_t profile_workers_begin = profile_now_ns();
#endif

  for (std::size_t i = 0; i < worker_count; ++i) {
    threads.emplace_back([&, i] {
#if PROFILE_PHASES
      worker_begin[i] = profile_now_ns();
#endif
      if (!cpus.empty()) {
        pin_current_thread(cpus[i % cpus.size()]);
      }

      auto state = std::make_unique<WorkerState>();
#if PROFILE_PHASES
      worker_parse_begin[i] = profile_now_ns();
#endif
      parse_range(boundaries[i], boundaries[i + 1], *state);
#if PROFILE_PHASES
      worker_parse_end[i] = profile_now_ns();
#endif
      workers[i] = std::move(state);
    });
  }

  for (std::thread &thread : threads) {
    thread.join();
  }
#if PROFILE_PHASES
  const std::uint64_t profile_workers_end = profile_now_ns();
  const std::uint64_t profile_merge_begin = profile_workers_end;
#endif

  for (std::size_t i = 0; i < worker_count; ++i) {
    merge_worker(*global, *workers[i]);
    workers[i].reset();
  }
#if PROFILE_PHASES
  const std::uint64_t profile_merge_end = profile_now_ns();
#endif

  if (munmap(const_cast<char *>(input.data), input.size) != 0) {
    fatal_errno("munmap input");
  }
#if PROFILE_PHASES
  const std::uint64_t profile_unmap_end = profile_now_ns();
  const std::uint64_t profile_output_begin = profile_unmap_end;
#endif

  int output_fd = STDOUT_FILENO;
  bool close_output = false;
  if (argc == 3) {
    output_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
      fatal_errno("open output");
    }
    close_output = true;
  }

  write_output(output_fd, *global);

  if (close_output && close(output_fd) != 0) {
    fatal_errno("close output");
  }

#if PROFILE_PHASES
  const std::uint64_t profile_output_end = profile_now_ns();
  print_profile_duration("input", profile_program_begin, profile_input_ready);
  print_profile_duration("setup", profile_input_ready, profile_workers_begin);
  print_profile_duration("workers", profile_workers_begin, profile_workers_end);
  print_profile_duration("merge+free", profile_merge_begin, profile_merge_end);
  print_profile_duration("unmap", profile_merge_end, profile_unmap_end);
  print_profile_duration("output", profile_output_begin, profile_output_end);
  print_profile_duration("measured", profile_program_begin, profile_output_end);

  std::uint64_t first_parse_begin = worker_parse_begin[0];
  std::uint64_t first_parse_end = worker_parse_end[0];
  std::uint64_t last_parse_end = worker_parse_end[0];
  for (std::size_t i = 0; i < worker_count; ++i) {
    if (worker_parse_begin[i] < first_parse_begin) {
      first_parse_begin = worker_parse_begin[i];
    }
    if (worker_parse_end[i] < first_parse_end) {
      first_parse_end = worker_parse_end[i];
    }
    if (worker_parse_end[i] > last_parse_end) {
      last_parse_end = worker_parse_end[i];
    }
    std::fprintf(
        stderr,
        "worker %zu init=%7.3f ms parse=%9.3f ms finish=%9.3f ms bytes=%zu\n",
        i, static_cast<double>(worker_parse_begin[i] - worker_begin[i]) /
               1'000'000.0,
        static_cast<double>(worker_parse_end[i] - worker_parse_begin[i]) /
            1'000'000.0,
        static_cast<double>(worker_parse_end[i] - first_parse_begin) /
            1'000'000.0,
        static_cast<std::size_t>(boundaries[i + 1] - boundaries[i]));
  }
  std::fprintf(stderr,
               "worker tail first_finish=%9.3f ms last_finish=%9.3f ms "
               "spread=%7.3f ms\n",
               static_cast<double>(first_parse_end - first_parse_begin) /
                   1'000'000.0,
               static_cast<double>(last_parse_end - first_parse_begin) /
                   1'000'000.0,
               static_cast<double>(last_parse_end - first_parse_end) /
                   1'000'000.0);
#endif

  return EXIT_SUCCESS;
}
