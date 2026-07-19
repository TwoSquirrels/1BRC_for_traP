#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

#ifndef ONEBRC_THREADS
#define ONEBRC_THREADS 8
#endif
#ifndef ONEBRC_PIPE_DEPTH
#define ONEBRC_PIPE_DEPTH 2
#endif
constexpr unsigned kThreads = ONEBRC_THREADS;
constexpr unsigned kMonths = 12;
constexpr unsigned kMaxChannels = 10000;
#ifndef ONEBRC_TABLE_SIZE
#define ONEBRC_TABLE_SIZE 16384
#endif
constexpr unsigned kTableSize = ONEBRC_TABLE_SIZE;
constexpr unsigned kCacheSize = 256;
#ifndef ONEBRC_AES_SALT
#define ONEBRC_AES_SALT 0
#endif

struct Stat {
  uint32_t sum;
  uint32_t stamps;
  uint32_t count;
#ifdef ONEBRC_PACKED_EXTREMA16
  uint16_t min;
  uint16_t max;
#else
  uint32_t min;
  uint32_t max;
#endif
};
#ifdef ONEBRC_PACKED_EXTREMA16
static_assert(sizeof(Stat) == 16);
#else
static_assert(sizeof(Stat) == 20);
#endif

struct ChannelStats {
  Stat month[kMonths];
#if !defined(ONEBRC_COMPACT_CHANNEL_STATS) && !defined(ONEBRC_PACKED_EXTREMA16)
  uint8_t padding[16];
#endif
};
#ifdef ONEBRC_PACKED_EXTREMA16
static_assert(sizeof(ChannelStats) == 192);
#elif defined(ONEBRC_COMPACT_CHANNEL_STATS)
static_assert(sizeof(ChannelStats) == 240);
#else
static_assert(sizeof(ChannelStats) == 256);
#endif

struct Slot {
#ifdef ONEBRC_COMPACT_SLOTS
  uint32_t fingerprint;
  uint32_t stats_offset;
#else
  uint64_t fingerprint;
  ChannelStats* stats;
#endif
};
#ifdef ONEBRC_COMPACT_SLOTS
static_assert(sizeof(Slot) == 8);
#else
static_assert(sizeof(Slot) == 16);
#endif

struct alignas(64) Worker {
  const char* begin;
  const char* end;
  Slot* table;
  Slot* cache;
  ChannelStats* stats;
  char* names;
  uint32_t channels;
  int error;
#ifdef ONEBRC_COUNT_PROBES
  uint64_t probes;
#endif
#ifdef ONEBRC_AFFINITY
  int cpu;
#endif
};

inline unsigned decode_month(const char* p) {
  static constexpr uint32_t boundaries[13] = {
      1798761600U, 1801440000U, 1803859200U, 1806537600U,
      1809129600U, 1811808000U, 1814400000U, 1817078400U,
      1819756800U, 1822348800U, 1825027200U, 1827619200U,
      1830297600U,
  };
  static constexpr auto table = [] {
    std::array<uint8_t, 65536> result{};
    for (unsigned i = 0; i < 316; ++i) {
      const uint32_t lo = (17987U + i) * 100000U;
      const uint32_t hi = lo + 99999U;
      unsigned month = 0;
      while (month + 1 < 13 && lo >= boundaries[month + 1]) ++month;
      const unsigned digits = 7987U + i;
      const unsigned bcd = digits / 1000U | digits / 100U % 10U << 4 |
                           digits / 10U % 10U << 8 | digits % 10U << 12;
      result[bcd] = static_cast<uint8_t>(month);
      if (month + 1 < 13 && hi >= boundaries[month + 1])
        result[bcd] = static_cast<uint8_t>(0x80U | month);
    }
    return result;
  }();
  static constexpr int64_t limits[12] = {
      0x3138303134343030LL - 1, 0x3138303338353932LL - 1,
      0x3138303635333736LL - 1, 0x3138303931323936LL - 1,
      0x3138313138303830LL - 1, 0x3138313434303030LL - 1,
      0x3138313730373834LL - 1, 0x3138313937353638LL - 1,
      0x3138323233343838LL - 1, 0x3138323530323732LL - 1,
      0x3138323736313932LL - 1, 0x3138333032393736LL - 1,
  };
  uint32_t raw4;
  std::memcpy(&raw4, p + 1, 4);
  const unsigned bcd = _pext_u32(raw4, 0x0f0f0f0fU);
  const unsigned code = table[bcd];
  if (__builtin_expect(code < 0x80U, 1)) return code;
  uint64_t raw;
  std::memcpy(&raw, p, 8);
  asm("bswap %0" : "+r"(raw));
  const unsigned month = code & 0x7fU;
  return month + (static_cast<int64_t>(raw) > limits[month]);
}

inline unsigned channel_length(__m256i bytes) {
  const __m256i commas = _mm256_set1_epi8(',');
  const uint32_t mask = static_cast<uint32_t>(
      _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, commas)));
  return _tzcnt_u32(mask);
}

inline uint64_t fingerprint_name(__m256i bytes, unsigned length) {
  alignas(32) static constexpr auto masks = [] {
    std::array<std::array<uint8_t, 32>, 33> result{};
    for (unsigned length = 0; length <= 32; ++length)
      for (unsigned i = 0; i < length; ++i) result[length][i] = 0xff;
    return result;
  }();
  const __m256i mask = _mm256_load_si256(
      reinterpret_cast<const __m256i*>(masks[length].data()));
  const __m256i name = _mm256_and_si256(bytes, mask);
#ifdef ONEBRC_CRC_HASH
  const uint64_t a = static_cast<uint64_t>(_mm256_extract_epi64(name, 0));
  const uint64_t b = static_cast<uint64_t>(_mm256_extract_epi64(name, 1));
  const uint64_t c = static_cast<uint64_t>(_mm256_extract_epi64(name, 2));
  const uint64_t d = static_cast<uint64_t>(_mm256_extract_epi64(name, 3));
  const uint32_t lo =
      static_cast<uint32_t>(_mm_crc32_u64(_mm_crc32_u64(0, a), b));
  const uint32_t hi =
      static_cast<uint32_t>(_mm_crc32_u64(_mm_crc32_u64(0, c), d));
  return static_cast<uint64_t>(hi) << 32 | lo;
#elif defined(ONEBRC_XOR_CRC_HASH)
  const __m128i lo = _mm256_castsi256_si128(name);
  const __m128i hi = _mm256_extracti128_si256(name, 1);
  const __m128i pair = _mm_xor_si128(lo, hi);
  const __m128i folded = _mm_xor_si128(pair, _mm_shuffle_epi32(pair, 0x4e));
  return _mm_crc32_u64(0x9e3779b9U,
                       static_cast<uint64_t>(_mm_cvtsi128_si64(folded)));
#elif defined(ONEBRC_ONE_AES_FOLD)
  const __m128i lo = _mm256_castsi256_si128(name);
  const __m128i hi = _mm256_extracti128_si256(name, 1);
  const __m128i mixed = _mm_xor_si128(_mm_aesenc_si128(
      lo, _mm_set_epi64x(0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL)), hi);
  const __m128i folded =
      _mm_xor_si128(mixed, _mm_shuffle_epi32(mixed, 0x4e));
  return static_cast<uint64_t>(_mm_cvtsi128_si64(folded));
#elif defined(ONEBRC_ONE_AES_SHUFFLE)
  const __m128i lo = _mm256_castsi256_si128(name);
  const __m128i hi = _mm256_extracti128_si256(name, 1);
  const __m128i shuffled = _mm_shuffle_epi8(
      hi, _mm_setr_epi8(5, 12, 3, 10, 1, 8, 15, 6,
                        13, 4, 11, 2, 9, 0, 7, 14));
  const __m128i mixed = _mm_aesenc_si128(
      _mm_xor_si128(lo, shuffled),
      _mm_set_epi64x(0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL));
  return static_cast<uint64_t>(_mm_cvtsi128_si64(mixed));
#elif defined(ONEBRC_PARALLEL_AES)
  const __m128i lo = _mm256_castsi256_si128(name);
  const __m128i hi = _mm256_extracti128_si256(name, 1);
  const __m128i a = _mm_aesenc_si128(
      lo, _mm_set_epi64x(0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL));
  const __m128i b = _mm_aesenc_si128(
      hi, _mm_set_epi64x(0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL));
  const uint64_t x = static_cast<uint64_t>(_mm_cvtsi128_si64(a)) ^
                     static_cast<uint64_t>(_mm_extract_epi64(a, 1));
  const uint64_t y = static_cast<uint64_t>(_mm_cvtsi128_si64(b)) ^
                     static_cast<uint64_t>(_mm_extract_epi64(b, 1));
  return x ^ ((y << 23) | (y >> 41));
#else
  const __m128i lo = _mm256_castsi256_si128(name);
#ifndef ONEBRC_SHORT_ONE_AES
  const __m128i hi = _mm256_extracti128_si256(name, 1);
#endif
  const __m128i a = _mm_aesenc_si128(
      lo, _mm_set_epi64x(0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL));
#ifdef ONEBRC_SHORT_ONE_AES
  if (__builtin_expect(length <= 16, 1)) {
    const __m128i folded = _mm_xor_si128(a, _mm_shuffle_epi32(a, 0x4e));
    return static_cast<uint64_t>(_mm_cvtsi128_si64(folded));
  }
#else
  (void)length;
#endif
#ifdef ONEBRC_SHORT_ONE_AES
  const __m128i hi = _mm256_extracti128_si256(name, 1);
#endif
  const __m128i mixed = _mm_aesenc_si128(
      _mm_xor_si128(a, hi),
      _mm_set_epi64x(0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL));
  return static_cast<uint64_t>(_mm_cvtsi128_si64(mixed));
#endif
}

inline Slot* lookup(Slot* table, Slot* slot,
#ifdef ONEBRC_COMPACT_SLOTS
                    uint32_t fingerprint) {
#else
                    uint64_t fingerprint) {
#endif
  Slot* const end = table + kTableSize;
  for (;;) {
    if (slot->fingerprint == 0 || slot->fingerprint == fingerprint) return slot;
    if (++slot == end) slot = table;
  }
}

inline uint32_t parse_until_comma(const char*& p) {
  const auto* s = reinterpret_cast<const unsigned char*>(p);
  if (__builtin_expect(s[3] == ',', 1)) {
    p += 4;
#ifdef ONEBRC_TABLE4_3DIGIT
    static constexpr auto values = [] {
      std::array<uint16_t, 65536> result{};
      for (unsigned a = 0; a < 10; ++a)
        for (unsigned b = 0; b < 10; ++b)
          for (unsigned c = 0; c < 10; ++c)
            result[a | b << 4 | c << 8 | 12 << 12] =
                static_cast<uint16_t>(a * 100 + b * 10 + c);
      return result;
    }();
    uint32_t raw;
    std::memcpy(&raw, s, 4);
    return values[_pext_u32(raw, 0x0f0f0f0fU)];
#elif defined(ONEBRC_DIRECT_3DIGIT)
    static constexpr auto values = [] {
      std::array<uint16_t, 0x09090a> result{};
      for (unsigned a = 0; a < 10; ++a)
        for (unsigned b = 0; b < 10; ++b)
          for (unsigned c = 0; c < 10; ++c)
            result[a | (b << 8) | (c << 16)] =
                static_cast<uint16_t>(a * 100 + b * 10 + c);
      return result;
    }();
    uint32_t raw;
    std::memcpy(&raw, s, 4);
    return values[raw - 0x2c303030U];
#elif defined(ONEBRC_TABLE_3DIGIT)
    static constexpr auto values = [] {
      std::array<uint16_t, 4096> result{};
      for (unsigned a = 0; a < 10; ++a)
        for (unsigned b = 0; b < 10; ++b)
          for (unsigned c = 0; c < 10; ++c)
            result[a | b << 4 | c << 8] =
                static_cast<uint16_t>(a * 100 + b * 10 + c);
      return result;
    }();
    uint32_t raw;
    std::memcpy(&raw, s, 4);
    return values[_pext_u32(raw, 0x000f0f0fU)];
#elif defined(ONEBRC_SIMD_3DIGIT)
    uint32_t raw;
    std::memcpy(&raw, s, 4);
    const __m128i ascii = _mm_cvtsi32_si128(static_cast<int>(raw));
    const __m128i pairs = _mm_maddubs_epi16(
        ascii, _mm_setr_epi8(100, 10, 1, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0));
    const __m128i sum = _mm_madd_epi16(pairs, _mm_set1_epi16(1));
    return static_cast<uint32_t>(_mm_cvtsi128_si32(sum) - 48 * 111);
#else
    return static_cast<uint32_t>(s[0] - '0') * 100U +
           static_cast<uint32_t>(s[1] - '0') * 10U +
           static_cast<uint32_t>(s[2] - '0');
#endif
  }
  if (__builtin_expect(s[2] == ',', 1)) {
    p += 3;
    return static_cast<uint32_t>(s[0] - '0') * 10U +
           static_cast<uint32_t>(s[1] - '0');
  }
  if (s[1] == ',') {
    p += 2;
    return static_cast<uint32_t>(s[0] - '0');
  }
  uint32_t value = 0;
  do {
    value = value * 10U + static_cast<uint32_t>(*p++ - '0');
  } while (*p != ',');
  ++p;
  return value;
}

inline uint32_t parse_until_newline(const char*& p) {
  const auto* s = reinterpret_cast<const unsigned char*>(p);
  if (__builtin_expect(s[1] == '\n', 1)) {
    p += 2;
    return static_cast<uint32_t>(s[0] - '0');
  }
  if (s[2] == '\n') {
    p += 3;
    return static_cast<uint32_t>(s[0] - '0') * 10U +
           static_cast<uint32_t>(s[1] - '0');
  }
  if (s[3] == '\n') {
    p += 4;
    return static_cast<uint32_t>(s[0] - '0') * 100U +
           static_cast<uint32_t>(s[1] - '0') * 10U +
           static_cast<uint32_t>(s[2] - '0');
  }
  uint32_t value = 0;
  do {
    value = value * 10U + static_cast<uint32_t>(*p++ - '0');
  } while (*p != '\n');
  ++p;
  return value;
}

#ifdef ONEBRC_INDEX_ROWS
inline uint32_t parse_until_end(const char* p, const char* end) {
  const size_t digits = static_cast<size_t>(end - p);
  if (__builtin_expect(digits == 1, 1))
    return static_cast<uint32_t>(p[0] - '0');
  if (digits == 2)
    return static_cast<uint32_t>(p[0] - '0') * 10U +
           static_cast<uint32_t>(p[1] - '0');
  if (digits == 3)
    return static_cast<uint32_t>(p[0] - '0') * 100U +
           static_cast<uint32_t>(p[1] - '0') * 10U +
           static_cast<uint32_t>(p[2] - '0');
  uint32_t value = 0;
  while (p != end)
    value = value * 10U + static_cast<uint32_t>(*p++ - '0');
  return value;
}
#endif

#ifdef ONEBRC_XMM_VALUES
struct Pending {
  Stat* stat;
  __m128i values;
};
#elif defined(ONEBRC_PACKED_VALUES)
struct Pending {
  Stat* stat;
  uint64_t values;
};
#else
struct Pending {
  Stat* stat;
  uint32_t message_length;
  uint32_t stamps;
};
#endif

__attribute__((always_inline)) inline Pending parse_row(Worker& worker,
                                                        const char*& p
#ifdef ONEBRC_INDEX_ROWS
                                                        , const char* row_end
#endif
                                                        ) {
    const unsigned month = decode_month(p);
    const char* name = p + 11;
    const __m256i name_bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(name));
    const unsigned length = channel_length(name_bytes);
    uint64_t fingerprint = fingerprint_name(name_bytes, length);
    asm volatile("" : "+r"(fingerprint) : : "memory");
#ifdef ONEBRC_COMPACT_SLOTS
    const uint32_t stored_fingerprint = static_cast<uint32_t>(fingerprint);
#else
    const uint64_t stored_fingerprint = fingerprint;
#endif
    Slot* first = worker.table +
                  (static_cast<uint32_t>(fingerprint) & (kTableSize - 1));
#ifdef ONEBRC_L1_CACHE
    Slot* const cache_slot = worker.cache +
        (static_cast<uint32_t>(fingerprint >> 32) & (kCacheSize - 1));
    const uint64_t cached_fingerprint = cache_slot->fingerprint;
    const bool cache_hit = cached_fingerprint == stored_fingerprint;
    if (!cache_hit)
      _mm_prefetch(reinterpret_cast<const char*>(first), _MM_HINT_T0);
#else
    _mm_prefetch(reinterpret_cast<const char*>(first), _MM_HINT_T0);
#endif

    p = name + length + 1;
    const uint32_t message_length = parse_until_comma(p);
#ifdef ONEBRC_INDEX_ROWS
    const uint32_t stamps = parse_until_end(p, row_end);
    p = row_end + 1;
#else
    const uint32_t stamps = parse_until_newline(p);
#endif

    Slot* slot;
    Slot* const table_end = worker.table + kTableSize;
#ifdef ONEBRC_L1_CACHE
    if (cache_hit) {
      slot = cache_slot;
      goto slot_found;
    }
#endif
    slot = first;
    for (;;) {
      if (slot->fingerprint == stored_fingerprint) goto cache_fill;
      if (slot->fingerprint == 0) break;
#ifdef ONEBRC_COUNT_PROBES
      ++worker.probes;
#endif
      if (++slot == table_end) slot = worker.table;
    }
    {
      const uint32_t id = worker.channels++;
      char* stored_name = worker.names + id * 32;
      std::memcpy(stored_name, name, 32);
      slot->fingerprint = stored_fingerprint;
#ifdef ONEBRC_COMPACT_SLOTS
      slot->stats_offset = id * sizeof(ChannelStats);
#else
      slot->stats = worker.stats + id;
#endif
    }

#ifdef ONEBRC_L1_CACHE
  cache_fill:
    *cache_slot = *slot;
#else
  cache_fill:
#endif
  slot_found:
#ifdef ONEBRC_COMPACT_SLOTS
    auto* channel_stats = reinterpret_cast<ChannelStats*>(
        reinterpret_cast<char*>(worker.stats) + slot->stats_offset);
    Stat* stat = &channel_stats->month[month];
#else
    Stat* stat = &slot->stats->month[month];
#endif
#if ONEBRC_PIPE_DEPTH > 1
    _mm_prefetch(reinterpret_cast<const char*>(stat), _MM_HINT_T0);
#endif
#ifdef ONEBRC_XMM_VALUES
    const uint64_t values = static_cast<uint64_t>(stamps) << 32 | message_length;
    return {stat, _mm_cvtsi64_si128(static_cast<int64_t>(values))};
#elif defined(ONEBRC_PACKED_VALUES)
    return {stat, static_cast<uint64_t>(stamps) << 32 | message_length};
#else
    return {stat, message_length, stamps};
#endif
}

__attribute__((always_inline)) inline void apply_pending(const Pending& pending) {
    Stat& stat = *pending.stat;
#ifdef ONEBRC_XMM_VALUES
    const uint64_t values = static_cast<uint64_t>(_mm_cvtsi128_si64(pending.values));
    const uint32_t message_length = static_cast<uint32_t>(values);
    const uint32_t pending_stamps = static_cast<uint32_t>(values >> 32);
#elif defined(ONEBRC_PACKED_VALUES)
    const uint64_t values = pending.values;
    const uint32_t message_length = static_cast<uint32_t>(values);
    const uint32_t pending_stamps = static_cast<uint32_t>(values >> 32);
#else
    const uint32_t message_length = pending.message_length;
    const uint32_t pending_stamps = pending.stamps;
#endif
#ifdef ONEBRC_PACKED_EXTREMA16
    const __m128i old = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(&stat.sum));
    const __m128i delta = _mm_unpacklo_epi64(
        _mm_cvtsi64_si128(static_cast<int64_t>(values)), _mm_cvtsi32_si128(1));
    const __m128i messages = _mm_set1_epi16(static_cast<uint16_t>(message_length));
    const __m128i minmax_messages = _mm_xor_si128(
        messages, _mm_setr_epi16(0, 0, 0, 0, 0, 0, -1, 0));
    __m128i updated = _mm_add_epi32(old, delta);
#if defined(ONEBRC_QEMU_SAFE) || defined(ONEBRC_PACKED_BLEND)
    updated = _mm_blend_epi16(updated, _mm_max_epu16(old, minmax_messages), 0xc0);
#else
    asm("vpmaxuw %2, %1, %0 %{%%k4%}"
        : "+v"(updated)
        : "v"(old), "v"(minmax_messages));
#endif
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&stat.sum), updated);
#else
#if !defined(ONEBRC_PREINIT_MIN) && !defined(ONEBRC_INVERTED_MIN)
    if (__builtin_expect(stat.count == 0, 0))
      stat.min = stat.max = message_length;
#endif
#ifdef ONEBRC_BRANCHED_STATS
    const __m128i old = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(&stat.sum));
    const __m128i delta = _mm_setr_epi32(
        static_cast<int>(message_length), static_cast<int>(pending_stamps), 1, 0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&stat.sum),
                     _mm_add_epi32(old, delta));
    if (__builtin_expect(message_length < stat.min, 0)) stat.min = message_length;
    if (__builtin_expect(message_length > stat.max, 0)) stat.max = message_length;
#elif defined(ONEBRC_NARROW_STATS)
    const __m128i old = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(&stat.sum));
    const __m128i delta = _mm_setr_epi32(
        static_cast<int>(message_length), static_cast<int>(pending_stamps), 1, 0);
    const __m128i messages = _mm_set1_epi32(message_length);
    __m128i updated = _mm_add_epi32(old, delta);
    updated = _mm_blend_epi32(updated, _mm_min_epu32(old, messages), 1 << 3);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&stat.sum), updated);
    stat.max = std::max(stat.max, message_length);
#else
    const __m256i old = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&stat.sum));
#ifdef ONEBRC_PACKED_VALUES
    const __m128i packed_delta = _mm_unpacklo_epi64(
        _mm_cvtsi64_si128(static_cast<int64_t>(values)), _mm_cvtsi32_si128(1));
    const __m256i delta = _mm256_castsi128_si256(packed_delta);
#else
    const __m256i delta = _mm256_setr_epi32(
        static_cast<int>(message_length), static_cast<int>(pending_stamps), 1,
        0, 0, 0, 0, 0);
#endif
#ifdef ONEBRC_FORCE_BLEND
    const __m128i message_xmm = _mm_cvtsi32_si128(message_length);
    __m256i messages;
    asm("vpbroadcastd %1, %0" : "=x"(messages) : "x"(message_xmm));
#else
    const __m256i messages = _mm256_set1_epi32(message_length);
#endif
#ifdef ONEBRC_INVERTED_MIN
    const __m256i minmax_messages = _mm256_xor_si256(
        messages, _mm256_setr_epi32(0, 0, 0, -1, 0, 0, 0, 0));
#endif
#if defined(ONEBRC_PACKED_VALUES) && defined(ONEBRC_MASKED_ADD_ASM)
    __m256i updated;
    asm("vpaddd %t2, %1, %0"
        : "=&v"(updated)
        : "v"(old), "x"(packed_delta));
#else
    __m256i updated = _mm256_add_epi32(old, delta);
#endif
#ifdef ONEBRC_MASKED_STATS_ASM
#ifdef ONEBRC_INVERTED_MIN
    asm("vpmaxud %2, %1, %0 %{%%k4%}"
        : "+v"(updated)
        : "v"(old), "v"(minmax_messages));
#else
    asm("vpminud %2, %1, %0 %{%%k1%}\n\t"
        "vpmaxud %2, %1, %0 %{%%k2%}"
        : "+v"(updated)
        : "v"(old), "v"(messages));
#endif
#elif defined(__AVX512VL__) && !defined(ONEBRC_FORCE_BLEND)
    updated = _mm256_mask_min_epu32(updated, 1 << 3, old, messages);
    updated = _mm256_mask_max_epu32(updated, 1 << 4, old, messages);
#else
#ifdef ONEBRC_INVERTED_MIN
    const __m256i extrema = _mm256_max_epu32(old, minmax_messages);
    updated = _mm256_blend_epi32(updated, extrema, (1 << 3) | (1 << 4));
#else
    updated = _mm256_blend_epi32(updated, _mm256_min_epu32(old, messages), 1 << 3);
    updated = _mm256_blend_epi32(updated, _mm256_max_epu32(old, messages), 1 << 4);
#endif
#endif
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&stat.sum), updated);
#endif
#endif
}

template <class T>
T* zeroed(size_t count);

void* run_worker(void* opaque) {
  auto& worker = *static_cast<Worker*>(opaque);
#ifdef ONEBRC_WORKER_ALLOCATE
  worker.table = zeroed<Slot>(kTableSize);
  worker.cache = zeroed<Slot>(kCacheSize);
  worker.stats = zeroed<ChannelStats>(kMaxChannels + 1);
  worker.names = zeroed<char>(static_cast<size_t>(kMaxChannels) * 32);
  if (worker.table == nullptr || worker.cache == nullptr ||
      worker.stats == nullptr || worker.names == nullptr) {
    worker.error = 1;
    return nullptr;
  }
#endif
#ifdef ONEBRC_MASKED_STATS_ASM
  asm volatile("movl $8, %%eax\n\t"
               "kmovd %%eax, %%k1\n\t"
               "movl $16, %%eax\n\t"
               "kmovd %%eax, %%k2"
               : : : "eax", "k1", "k2");
#ifdef ONEBRC_INVERTED_MIN
#ifdef ONEBRC_PACKED_EXTREMA16
  asm volatile("movl $192, %%eax\n\t"
               "kmovd %%eax, %%k4"
               : : : "eax", "k4");
#else
  asm volatile("movl $24, %%eax\n\t"
               "kmovd %%eax, %%k4"
               : : : "eax", "k4");
#endif
#endif
#endif
#ifdef ONEBRC_AFFINITY
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(worker.cpu, &cpu_set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
#endif
  const char* p = worker.begin;
#ifdef ONEBRC_INDEX_ROWS
#if ONEBRC_PIPE_DEPTH != 4
#error ONEBRC_INDEX_ROWS currently requires pipeline depth 4
#endif
  const size_t input_bytes = static_cast<size_t>(worker.end - worker.begin);
  const size_t capacity = input_bytes / 16 + 1;
  auto* row_ends = static_cast<uint32_t*>(std::malloc(capacity * sizeof(uint32_t)));
  if (row_ends == nullptr) {
    worker.error = 1;
    return nullptr;
  }
  size_t row_count = 0;
  const char* scan = worker.begin;
  const __m256i newline = _mm256_set1_epi8('\n');
  while (scan + 32 <= worker.end) {
    const __m256i bytes = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(scan));
    uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(bytes, newline)));
    while (mask != 0) {
      row_ends[row_count++] = static_cast<uint32_t>(
          scan - worker.begin + _tzcnt_u32(mask));
      mask &= mask - 1;
    }
    scan += 32;
  }
  while (scan != worker.end) {
    if (*scan == '\n')
      row_ends[row_count++] = static_cast<uint32_t>(scan - worker.begin);
    ++scan;
  }
  auto parse_index = [&](size_t index) __attribute__((always_inline)) {
    const uint32_t start = index == 0 ? 0 : row_ends[index - 1] + 1;
    const char* row = worker.begin + start;
    return parse_row(worker, row, worker.begin + row_ends[index]);
  };
  size_t row_index = 0;
  Pending first = parse_index(row_index++);
  Pending second = parse_index(row_index++);
  Pending third = parse_index(row_index++);
  while (row_index < row_count) {
    const Pending next = parse_index(row_index++);
    apply_pending(first);
    first = second;
    second = third;
    third = next;
  }
  apply_pending(first);
  apply_pending(second);
  apply_pending(third);
  std::free(row_ends);
  return nullptr;
#else
#ifdef ONEBRC_RING_PIPE
  Pending queue[ONEBRC_PIPE_DEPTH - 1];
#pragma clang loop unroll(disable)
  for (unsigned i = 0; i < ONEBRC_PIPE_DEPTH - 1; ++i)
    queue[i] = parse_row(worker, p);
  unsigned head = 0;
#pragma clang loop unroll(disable)
  while (p < worker.end) {
    const Pending next = parse_row(worker, p);
    apply_pending(queue[head]);
    queue[head] = next;
    if (++head == ONEBRC_PIPE_DEPTH - 1) head = 0;
  }
#pragma clang loop unroll(disable)
  for (unsigned i = 0; i < ONEBRC_PIPE_DEPTH - 1; ++i) {
    apply_pending(queue[head]);
    if (++head == ONEBRC_PIPE_DEPTH - 1) head = 0;
  }
#elif ONEBRC_PIPE_DEPTH == 1
  while (p < worker.end) {
    const Pending pending = parse_row(worker, p);
    apply_pending(pending);
  }
#elif ONEBRC_PIPE_DEPTH == 2
  Pending pending = parse_row(worker, p);
  while (p < worker.end) {
    const Pending next = parse_row(worker, p);
    apply_pending(pending);
    pending = next;
  }
  apply_pending(pending);
#elif ONEBRC_PIPE_DEPTH == 3
  Pending first = parse_row(worker, p);
  Pending second = parse_row(worker, p);
  while (p < worker.end) {
    const Pending next = parse_row(worker, p);
    apply_pending(first);
    first = second;
    second = next;
  }
  apply_pending(first);
  apply_pending(second);
#elif ONEBRC_PIPE_DEPTH == 4
  Pending first = parse_row(worker, p);
  Pending second = parse_row(worker, p);
  Pending third = parse_row(worker, p);
#ifdef ONEBRC_ALIGN_HOT_LOOP
  asm volatile(".p2align 6" : : : "memory");
#endif
#ifdef ONEBRC_PAD_HOT_32
  asm volatile(".rept 32\n\tnop\n\t.endr" : : : "memory");
#endif
#ifdef ONEBRC_PAD_HOT_48
  asm volatile(".rept 48\n\tnop\n\t.endr" : : : "memory");
#endif
  while (p < worker.end) {
    const Pending next = parse_row(worker, p);
    apply_pending(first);
    first = second;
    second = third;
    third = next;
  }
  apply_pending(first);
  apply_pending(second);
  apply_pending(third);
#elif ONEBRC_PIPE_DEPTH == 5
  Pending first = parse_row(worker, p);
  Pending second = parse_row(worker, p);
  Pending third = parse_row(worker, p);
  Pending fourth = parse_row(worker, p);
  while (p < worker.end) {
    const Pending next = parse_row(worker, p);
    apply_pending(first);
    first = second;
    second = third;
    third = fourth;
    fourth = next;
  }
  apply_pending(first);
  apply_pending(second);
  apply_pending(third);
  apply_pending(fourth);
#elif ONEBRC_PIPE_DEPTH == 6
  Pending first = parse_row(worker, p);
  Pending second = parse_row(worker, p);
  Pending third = parse_row(worker, p);
  Pending fourth = parse_row(worker, p);
  Pending fifth = parse_row(worker, p);
  while (p < worker.end) {
    const Pending next = parse_row(worker, p);
    apply_pending(first);
    first = second;
    second = third;
    third = fourth;
    fourth = fifth;
    fifth = next;
  }
  apply_pending(first);
  apply_pending(second);
  apply_pending(third);
  apply_pending(fourth);
  apply_pending(fifth);
#else
#error unsupported ONEBRC_PIPE_DEPTH
#endif
  return nullptr;
#endif
}

template <class T>
T* zeroed(size_t count) {
  const size_t bytes = count * sizeof(T);
#ifdef ONEBRC_LAZY_ZERO
  void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) return nullptr;
#ifdef MADV_HUGEPAGE
  if (bytes >= 2U * 1024U * 1024U)
    (void)madvise(memory, bytes, MADV_HUGEPAGE);
#endif
  return static_cast<T*>(memory);
#else
  void* memory = nullptr;
  constexpr size_t kHugePage = 2U * 1024U * 1024U;
  const size_t alignment = bytes >= kHugePage ? kHugePage : 64;
  if (posix_memalign(&memory, alignment, bytes) != 0) return nullptr;
  std::memset(memory, 0, bytes);
#ifdef MADV_HUGEPAGE
  if (bytes >= kHugePage)
    (void)madvise(memory, bytes, MADV_HUGEPAGE);
#endif
  return static_cast<T*>(memory);
#endif
}

#ifdef ONEBRC_PREINIT_MIN
__attribute__((noinline)) void initialize_mins(ChannelStats* stats) {
  for (unsigned channel = 0; channel < kMaxChannels; ++channel)
    for (unsigned month = 0; month < kMonths; ++month)
      stats[channel].month[month].min = UINT32_MAX;
}
#endif

char* put_u64(char* p, uint64_t value) {
  auto result = std::to_chars(p, p + 24, value);
  return result.ptr;
}

#ifdef ONEBRC_INTEGER_AVERAGE
inline char* put_average(char* out, uint32_t sum, uint32_t count) {
  const uint64_t numerator = static_cast<uint64_t>(sum) * 100U;
  uint64_t scaled = numerator / count;
  const uint64_t remainder = numerator % count;
  const uint64_t twice = remainder * 2U;
  scaled += twice > count || (twice == count && (scaled & 1U));
  out = put_u64(out, scaled / 100U);
  *out++ = '.';
  const unsigned fraction = static_cast<unsigned>(scaled % 100U);
  *out++ = static_cast<char>('0' + fraction / 10U);
  *out++ = static_cast<char>('0' + fraction % 10U);
  return out;
}
#endif

#ifdef ONEBRC_FAST_AVERAGE
inline char* put_average_fast(char* out, uint32_t sum, uint32_t count) {
  const double average = static_cast<double>(sum) / count;
  uint64_t bits;
  std::memcpy(&bits, &average, sizeof(bits));
  const unsigned exponent = static_cast<unsigned>(bits >> 52) & 0x7ffU;
  const uint64_t significand = (bits & ((UINT64_C(1) << 52) - 1)) |
                               (UINT64_C(1) << 52);
  const unsigned shift = 1075U - exponent;
  const uint64_t numerator = significand * 100U;
  uint64_t scaled = numerator >> shift;
  const uint64_t remainder = numerator & ((UINT64_C(1) << shift) - 1);
  const uint64_t half = UINT64_C(1) << (shift - 1);
  scaled += remainder > half || (remainder == half && (scaled & 1U));
  out = put_u64(out, scaled / 100U);
  *out++ = '.';
  const unsigned fraction = static_cast<unsigned>(scaled % 100U);
  *out++ = static_cast<char>('0' + fraction / 10U);
  *out++ = static_cast<char>('0' + fraction % 10U);
  return out;
}
#endif

struct FinalizeTask {
  ChannelStats** sources;
  char* names;
  uint32_t begin;
  uint32_t end;
  char* output;
  size_t output_size;
};

#ifdef ONEBRC_QEMU_SAFE
__attribute__((target("avx2,no-avx512f,no-avx512vl")))
#endif
void* run_finalize(void* opaque) {
  auto& task = *static_cast<FinalizeTask*>(opaque);
  char* out = task.output;
  for (uint32_t id = task.begin; id < task.end; ++id) {
    const char* name = task.names + static_cast<size_t>(id) * 32;
#ifdef ONEBRC_QEMU_SAFE
    unsigned name_length = 0;
    while (name[name_length] != ',') ++name_length;
#else
    const unsigned name_length = channel_length(_mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(name)));
#endif
    for (unsigned month = 0; month < kMonths; ++month) {
      Stat merged{};
      for (unsigned worker = 0; worker < kThreads; ++worker) {
        ChannelStats* const source =
            task.sources[static_cast<size_t>(id) * kThreads + worker];
#ifndef ONEBRC_DENSE_SOURCES
        if (source == nullptr) continue;
#endif
        const Stat& from = source->month[month];
#if defined(ONEBRC_INVERTED_MIN) && defined(ONEBRC_BRANCHLESS_MERGE)
        merged.sum += from.sum;
        merged.stamps += from.stamps;
        merged.count += from.count;
        merged.min = std::max(merged.min, from.min);
        merged.max = std::max(merged.max, from.max);
#else
        if (from.count == 0) continue;
        if (merged.count == 0) {
          merged = from;
        } else {
          merged.sum += from.sum;
          merged.stamps += from.stamps;
          merged.count += from.count;
#ifdef ONEBRC_INVERTED_MIN
          merged.min = std::max(merged.min, from.min);
#else
          merged.min = std::min(merged.min, from.min);
#endif
          merged.max = std::max(merged.max, from.max);
        }
#endif
      }
      if (merged.count == 0) continue;
      std::memcpy(out, name, name_length);
      out += name_length;
      *out++ = ',';
      std::memcpy(out, "2027-", 5);
      out += 5;
      *out++ = static_cast<char>('0' + (month + 1) / 10);
      *out++ = static_cast<char>('0' + (month + 1) % 10);
      *out++ = '=';
#ifdef ONEBRC_INVERTED_MIN
#ifdef ONEBRC_PACKED_EXTREMA16
      out = put_u64(out, static_cast<uint16_t>(~merged.min));
#else
      out = put_u64(out, ~merged.min);
#endif
#else
      out = put_u64(out, merged.min);
#endif
      *out++ = '/';
#ifdef ONEBRC_QEMU_SAFE
      int64_t average_sum = merged.sum, average_count = merged.count;
      asm volatile("" : "+r"(average_sum), "+r"(average_count));
      out = std::to_chars(out, out + 32,
                          static_cast<double>(average_sum) / average_count,
                          std::chars_format::fixed, 2).ptr;
#else
#ifdef ONEBRC_FAST_AVERAGE
      out = put_average_fast(out, merged.sum, merged.count);
#elif defined(ONEBRC_INTEGER_AVERAGE)
      out = put_average(out, merged.sum, merged.count);
#else
      out = std::to_chars(out, out + 32,
                          static_cast<double>(merged.sum) / merged.count,
                          std::chars_format::fixed, 2).ptr;
#endif
#endif
      *out++ = '/';
      out = put_u64(out, merged.max);
      *out++ = '/';
      out = put_u64(out, merged.count);
      *out++ = '/';
      out = put_u64(out, merged.stamps);
      *out++ = '\n';
    }
  }
  task.output_size = static_cast<size_t>(out - task.output);
  return nullptr;
}

bool write_all(int fd, const char* p, size_t size) {
  while (size != 0) {
    const ssize_t written = write(fd, p, size);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

#ifdef ONEBRC_QEMU_SAFE
__attribute__((noinline, target("avx2,no-avx512f,no-avx512vl")))
const char* find_newline_scalar(const char* p, const char* end) {
  while (p != end && *p != '\n') ++p;
  return p == end ? nullptr : p;
}
#endif

}  // namespace

#ifdef ONEBRC_QEMU_SAFE
__attribute__((target("avx2,no-avx512f,no-avx512vl")))
#endif
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const int input_fd = open(argv[1], O_RDONLY);
  if (input_fd < 0) return 1;
  struct stat file_stat {};
  if (fstat(input_fd, &file_stat) != 0 || file_stat.st_size == 0) return 1;
  const size_t file_size = static_cast<size_t>(file_stat.st_size);
  const char* mapping = static_cast<const char*>(
      mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, input_fd, 0));
  if (mapping == MAP_FAILED) return 1;
  madvise(const_cast<char*>(mapping), file_size, MADV_SEQUENTIAL);

#ifdef ONEBRC_QEMU_SAFE
  const char* header = find_newline_scalar(mapping, mapping + file_size);
#else
  const char* header = static_cast<const char*>(std::memchr(mapping, '\n', file_size));
#endif
  if (header == nullptr) return 2;
  const char* begin = header + 1;
  const char* end = mapping + file_size;

  const char* bounds[kThreads + 1];
  bounds[0] = begin;
  bounds[kThreads] = end;
#ifdef ONEBRC_QEMU_SAFE
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
#endif
  for (unsigned i = 1; i < kThreads; ++i) {
    const char* cut = begin + static_cast<size_t>(end - begin) * i / kThreads;
#ifdef ONEBRC_QEMU_SAFE
    const char* newline = find_newline_scalar(cut, end);
#else
    const char* newline = static_cast<const char*>(std::memchr(cut, '\n', end - cut));
#endif
    bounds[i] = newline == nullptr ? end : newline + 1;
  }

  Worker workers[kThreads] {};
  pthread_t threads[kThreads];
#ifdef ONEBRC_AFFINITY
  cpu_set_t allowed_cpus;
  CPU_ZERO(&allowed_cpus);
  (void)sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus);
  int worker_cpus[kThreads] {};
  unsigned found_cpus = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE && found_cpus < kThreads; ++cpu)
    if (CPU_ISSET(cpu, &allowed_cpus)) worker_cpus[found_cpus++] = cpu;
#endif
  for (unsigned i = 0; i < kThreads; ++i) {
    workers[i].begin = bounds[i];
    workers[i].end = bounds[i + 1];
#ifndef ONEBRC_WORKER_ALLOCATE
    workers[i].table = zeroed<Slot>(kTableSize);
    workers[i].cache = zeroed<Slot>(kCacheSize);
    workers[i].stats = zeroed<ChannelStats>(kMaxChannels + 1);
#ifdef ONEBRC_UNINITIALIZED_AUX
    workers[i].names = static_cast<char*>(
        std::malloc(static_cast<size_t>(kMaxChannels) * 32));
#else
    workers[i].names = zeroed<char>(static_cast<size_t>(kMaxChannels) * 32);
#endif
#ifdef ONEBRC_AFFINITY
    workers[i].cpu = worker_cpus[i];
#endif
    if (workers[i].table == nullptr || workers[i].cache == nullptr ||
        workers[i].stats == nullptr ||
        workers[i].names == nullptr) return 1;
#endif
#ifdef ONEBRC_PREINIT_MIN
    initialize_mins(workers[i].stats);
#endif
    if (pthread_create(&threads[i], nullptr, run_worker, workers + i) != 0) return 1;
  }
#ifdef ONEBRC_OVERLAP_FINAL_ALLOC
  Slot* channels = zeroed<Slot>(kTableSize);
  ChannelStats* stats = zeroed<ChannelStats>(kMaxChannels + 1);
  char* channel_names = zeroed<char>(static_cast<size_t>(kMaxChannels) * 32);
  ChannelStats** sources = zeroed<ChannelStats*>(
      static_cast<size_t>(kMaxChannels) * kThreads);
  if (channels == nullptr || stats == nullptr || channel_names == nullptr ||
      sources == nullptr) return 1;
#endif
  for (unsigned i = 0; i < kThreads; ++i)
    if (pthread_join(threads[i], nullptr) != 0 || workers[i].error) return 1;
#ifdef ONEBRC_COUNT_PROBES
  uint64_t total_probes = 0;
  for (const Worker& worker : workers) total_probes += worker.probes;
  std::fprintf(stderr, "probes=%llu\n",
               static_cast<unsigned long long>(total_probes));
#endif

#ifndef ONEBRC_OVERLAP_FINAL_ALLOC
  Slot* channels = zeroed<Slot>(kTableSize);
#ifdef ONEBRC_UNINITIALIZED_AUX
  ChannelStats* stats = static_cast<ChannelStats*>(
      std::malloc(static_cast<size_t>(kMaxChannels + 1) * sizeof(ChannelStats)));
  char* channel_names = static_cast<char*>(
      std::malloc(static_cast<size_t>(kMaxChannels) * 32));
#else
  ChannelStats* stats = zeroed<ChannelStats>(kMaxChannels + 1);
  char* channel_names = zeroed<char>(static_cast<size_t>(kMaxChannels) * 32);
#endif
  ChannelStats** sources = zeroed<ChannelStats*>(
      static_cast<size_t>(kMaxChannels) * kThreads);
  if (channels == nullptr || stats == nullptr || channel_names == nullptr ||
      sources == nullptr) return 1;
#endif
#ifdef ONEBRC_DENSE_SOURCES
  std::fill_n(sources, static_cast<size_t>(kMaxChannels) * kThreads, stats);
#endif
  uint32_t channel_count = 0;
  for (unsigned worker_index = 0; worker_index < kThreads; ++worker_index) {
    const Worker& worker = workers[worker_index];
    for (unsigned i = 0; i < kTableSize; ++i) {
      const Slot& source = worker.table[i];
      if (source.fingerprint == 0) continue;
      Slot* first = channels +
                    (static_cast<uint32_t>(source.fingerprint) &
                     (kTableSize - 1));
      Slot* destination = lookup(channels, first, source.fingerprint);
      if (destination->fingerprint == 0) {
        *destination = source;
#ifdef ONEBRC_COMPACT_SLOTS
        destination->stats_offset = channel_count * sizeof(ChannelStats);
        const size_t source_id = source.stats_offset / sizeof(ChannelStats);
#else
        destination->stats = stats + channel_count;
        const size_t source_id = static_cast<size_t>(source.stats - worker.stats);
#endif
        std::memcpy(channel_names + channel_count * 32,
                    worker.names + source_id * 32, 32);
        ++channel_count;
      }
      const size_t destination_id =
#ifdef ONEBRC_COMPACT_SLOTS
          destination->stats_offset / sizeof(ChannelStats);
      sources[destination_id * kThreads + worker_index] =
          reinterpret_cast<ChannelStats*>(
              reinterpret_cast<char*>(worker.stats) + source.stats_offset);
#else
          static_cast<size_t>(destination->stats - stats);
      sources[destination_id * kThreads + worker_index] = source.stats;
#endif
    }
  }

  FinalizeTask finalize[kThreads] {};
  for (unsigned i = 0; i < kThreads; ++i) {
    finalize[i].sources = sources;
    finalize[i].names = channel_names;
    finalize[i].begin = static_cast<uint32_t>(
        static_cast<uint64_t>(channel_count) * i / kThreads);
    finalize[i].end = static_cast<uint32_t>(
        static_cast<uint64_t>(channel_count) * (i + 1) / kThreads);
    const size_t capacity = static_cast<size_t>(finalize[i].end - finalize[i].begin) *
                            kMonths * 96;
    finalize[i].output = static_cast<char*>(std::malloc(capacity));
    if (finalize[i].output == nullptr ||
        pthread_create(&threads[i], nullptr, run_finalize, finalize + i) != 0)
      return 1;
  }
  for (unsigned i = 0; i < kThreads; ++i)
    if (pthread_join(threads[i], nullptr) != 0) return 1;

  const int output_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (output_fd < 0) return 1;
  for (unsigned i = 0; i < kThreads; ++i)
    if (!write_all(output_fd, finalize[i].output, finalize[i].output_size)) return 1;
  close(output_fd);
  munmap(const_cast<char*>(mapping), file_size);
  close(input_fd);
  return 0;
}
