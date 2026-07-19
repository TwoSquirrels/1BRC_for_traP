#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <immintrin.h>

// 入力契約:
// - ASCII、引用符・escape・field内comma/newlineなし、headerはちょうど1行
// - 空行なし、全行LF終端、Unix秒は2027年内なので常に10桁
// - チャンネルの文字列はtraQと同じ制約（^[0-9A-Za-z_-]{1,20}(/[0-9A-Za-z_-]{1,20}){0,4}$）、チャンネルは最大10,000種類
// - message_length>=1。channel×monthのtotal_lengthがuint32_t以内なので、
//   個々のmessage_lengthもuint32_t以内
// - channel×monthのcount/total_length/total_stampsは最終値もuint32_t以内
// channel parserの16-byte load用にmapping末尾へguard pageを置き、commaを越える
// 先読みをfile sizeがpage境界に一致する場合も安全にする。

// R7i.2xlarge は8 vCPU。測定対象に合わせ、実行時の分岐を持たせない。
static constexpr unsigned kThreadCount = 8;
static constexpr std::size_t kMonthsPerYear = 12;
static constexpr std::size_t kMaxChannels = 10000;
static constexpr std::size_t kMaxAggregates =
    kMaxChannels * kMonthsPerYear;
static constexpr std::size_t kHugePageBytes = 2 * 1024 * 1024;
static_assert(kMaxChannels <= std::numeric_limits<std::uint16_t>::max());

// thread-localの頻出集計を16 byteに保つ。65,535超のmin/maxは遅延確保する
// 別表へ逃がし、通常行のcache footprintを増やさない。
struct CompactStats {
  // low32=total_length, high32=total_stamp_count。両最終値がuint32_t以内で
  // message_length>=1なので、low側からhigh側へのcarryは起きない。
  std::uint64_t length_and_stamps;
  std::uint32_t message_count_value;
  std::uint16_t min_length;
  // UINT16_MAX-max_length。UINT16_MAXはsmall lengthなしのsentinel。
  std::uint16_t inverted_max_length;

  std::uint32_t total_length() const noexcept {
    return static_cast<std::uint32_t>(length_and_stamps);
  }
  std::uint32_t total_stamp_count() const noexcept {
    return static_cast<std::uint32_t>(length_and_stamps >> 32);
  }
  std::uint32_t message_count() const noexcept {
    return message_count_value;
  }
};
static_assert(sizeof(CompactStats) == 16);
static_assert(offsetof(CompactStats, message_count_value) == 8);
static_assert(offsetof(CompactStats, min_length) == 12);
static_assert(offsetof(CompactStats, inverted_max_length) ==
              offsetof(CompactStats, min_length) + sizeof(std::uint16_t));

// merge後は出力用なので、min/maxも完全な32 bitで保持する。
struct Stats {
  std::uint64_t length_and_count = 0;
  std::uint32_t total_stamp_count = 0;
  std::uint32_t min_length = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t max_length = 0;

  std::uint32_t total_length() const noexcept {
    return static_cast<std::uint32_t>(length_and_count);
  }
  std::uint32_t message_count() const noexcept {
    return static_cast<std::uint32_t>(length_and_count >> 32);
  }
};
static_assert(sizeof(Stats) == 24);

struct MonthSlot {
  CompactStats stats;
};

// -----------------------------------------------------------------------------
// ハッシュとチャンネル照合
// -----------------------------------------------------------------------------

static inline bool equal_channel(const char *left, const char *right,
                                 std::uint32_t length) noexcept {
  while (length >= 8) {
    std::uint64_t left_word;
    std::uint64_t right_word;
    std::memcpy(&left_word, left, sizeof(left_word));
    std::memcpy(&right_word, right, sizeof(right_word));
    if (left_word != right_word) {
      return false;
    }
    left += 8;
    right += 8;
    length -= 8;
  }
  while (length != 0) {
    if (*left++ != *right++) {
      return false;
    }
    --length;
  }
  return true;
}

static inline std::uint64_t channel_hash_step(std::uint64_t hash,
                                              std::uint64_t word) noexcept {
  return hash ^ word;
}

static constexpr std::uint32_t kChannelBucketCount = 32768;
static constexpr unsigned kChannelIdBits = 14;
static constexpr std::uint64_t kChannelIdMask =
    (UINT64_C(1) << kChannelIdBits) - 1;
static constexpr unsigned kSteadySlotCodeBits = 15;
static constexpr std::uint32_t kSteadySlotCodeMask =
    (UINT32_C(1) << kSteadySlotCodeBits) - 1;
static_assert(std::has_single_bit(kChannelBucketCount));
static_assert(kMaxChannels <= kChannelIdMask);
static_assert((kMaxChannels - 1) * 3 <= kSteadySlotCodeMask);

using ChannelLookup = std::uint32_t;

enum class HashOnlyLookupMode : std::uint8_t {
  none,
  folded,
  suffix8,
  aes2_folded,
};

static inline ChannelLookup channel_lookup(std::uint64_t hash,
                                           std::uint32_t tail_index) noexcept {
  return static_cast<std::uint32_t>(
      _mm_crc32_u64(tail_index, hash));
}

static inline std::uint64_t
short_channel_high(const char *text, std::uint32_t length) noexcept {
  if (length < 8) {
    return 0;
  }
  std::uint64_t high;
  std::memcpy(&high, text + 8, sizeof(high));
  return _bzhi_u64(high, (length - 7) * 8);
}

struct ChannelRef {
  const char *text;
  std::uint64_t hash;
  std::uint64_t short_high;
  std::uint32_t length;
};
static_assert(sizeof(ChannelRef) == 32);

static inline ChannelLookup suffix8_channel_lookup(
    const ChannelRef &channel) noexcept {
  if (channel.length < 8) {
    return channel_lookup(channel.hash, channel.length & 15U);
  }
  std::uint64_t suffix;
  std::memcpy(&suffix, channel.text + channel.length - 8, sizeof(suffix));
  return static_cast<ChannelLookup>(
      _mm_crc32_u64(channel.length & 15U, suffix));
}

static inline __m128i folded_channel_state(
    const ChannelRef &channel) noexcept {
  const char *cursor = channel.text;
  std::size_t remaining = static_cast<std::size_t>(channel.length) + 1;
  std::uint64_t low = 0;
  std::uint64_t high = 0;
  while (remaining >= 16) {
    std::uint64_t word0;
    std::uint64_t word1;
    std::memcpy(&word0, cursor, sizeof(word0));
    std::memcpy(&word1, cursor + 8, sizeof(word1));
    low ^= word0;
    high ^= word1;
    cursor += 16;
    remaining -= 16;
  }
  if (remaining != 0) {
    std::uint64_t word0 = 0;
    std::uint64_t word1 = 0;
    const std::size_t low_bytes = std::min<std::size_t>(remaining, 8);
    std::memcpy(&word0, cursor, low_bytes);
    if (remaining > 8) {
      std::memcpy(&word1, cursor + 8, remaining - 8);
    }
    low ^= word0;
    high ^= word1;
  }
  return _mm_set_epi64x(static_cast<long long>(high),
                        static_cast<long long>(low));
}

static inline ChannelLookup aes2_state_lookup(__m128i state,
                                              __m128i round_key) noexcept {
  const __m128i round1 = _mm_aesenc_si128(state, round_key);
  const __m128i round2 = _mm_aesenc_si128(round1, round_key);
  return static_cast<ChannelLookup>(_mm_cvtsi128_si32(round2));
}

static inline ChannelLookup aes2_folded_channel_lookup(
    const ChannelRef &channel) noexcept {
  const __m128i comma_key = _mm_set1_epi8(',');
  return aes2_state_lookup(folded_channel_state(channel), comma_key);
}

// channel table 128 KiBと月別集計1.92 MBを1枚の2 MiB pageへ収める。
struct alignas(kHugePageBytes) HotStorage {
  std::array<std::uint32_t, kChannelBucketCount> channel_entries{};
  std::array<MonthSlot, kMaxAggregates> month_slots;
};
static_assert(sizeof(HotStorage) == kHugePageBytes);
static_assert(offsetof(HotStorage, month_slots) % 32 == 0);

struct HotStorageDeleter {
  void operator()(HotStorage *storage) const noexcept {
    std::destroy_at(storage);
    std::free(storage);
  }
};

using HotStoragePtr = std::unique_ptr<HotStorage, HotStorageDeleter>;

static HotStoragePtr make_hot_storage() {
  void *const memory = std::aligned_alloc(kHugePageBytes, kHugePageBytes);
  if (memory == nullptr) {
    std::abort();
  }
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  (void)::madvise(memory, kHugePageBytes, MADV_HUGEPAGE);
#endif
  // value-initによる2 MiB全体のzero後にmember初期値を再書込みしない。
  HotStorage *const storage = ::new (memory) HotStorage;
  // min=UINT16_MAX, inverted-max=UINT16_MAX is the empty sentinel.
  const __m256i empty_slots =
      _mm256_setr_epi32(0, 0, 0, -1, 0, 0, 0, -1);
  auto *cursor =
      reinterpret_cast<std::byte *>(storage->month_slots.data());
  auto *const end = cursor + sizeof(storage->month_slots);
  static_assert(sizeof(storage->month_slots) % (sizeof(__m256i) * 4) == 0);
  do {
    std::memcpy(cursor, &empty_slots, sizeof(empty_slots));
    std::memcpy(cursor + 32, &empty_slots, sizeof(empty_slots));
    std::memcpy(cursor + 64, &empty_slots, sizeof(empty_slots));
    std::memcpy(cursor + 96, &empty_slots, sizeof(empty_slots));
    cursor += 128;
  } while (cursor != end);
  return HotStoragePtr(storage);
}

// 行ごとのチャンネル文字列を密なIDへ変換するホットパス用テーブル。
// 初出期間は18-bit tag一致後に文字列も比較するため、衝突時も正しい。
// CRC32Cはbucket分散とtag生成に使う。
class ChannelTable {
public:
  ChannelTable() : storage_(make_hot_storage()) {
    channels_.reserve(kMaxChannels);
  }

  __attribute__((always_inline)) std::uint32_t
  get(const char *text, std::uint32_t length, std::uint64_t hash,
      ChannelLookup lookup) {
    std::size_t index = lookup & (kBucketCount - 1);
    for (;;) {
      const std::uint32_t entry = storage_->channel_entries[index];
      const std::uint16_t encoded_id =
          static_cast<std::uint16_t>(entry & kChannelIdMask);
      if (encoded_id == 0) {
        return insert_new(index, text, length, hash,
                          lookup & ~static_cast<std::uint32_t>(kChannelIdMask));
      }
      if (((entry ^ lookup) &
           ~static_cast<std::uint32_t>(kChannelIdMask)) == 0) [[likely]] {
        const std::uint32_t channel_id = encoded_id - 1;
        const ChannelRef &known = channels_[channel_id];
        if (known.length == length) {
          // comma込みのmasked 128 bitをL/Hとすると、hash=L^HとHで
          // 全128 bitを厳密に照合できる。
          if (length <= 15) {
            if (known.hash == hash &&
                (length <= 7 ||
                 known.short_high == short_channel_high(text, length))) {
              return channel_id * static_cast<std::uint32_t>(kMonthsPerYear);
            }
          } else if (equal_channel(known.text, text, length)) {
            return channel_id * static_cast<std::uint32_t>(kMonthsPerYear);
          }
        }
        // 同じfingerprintを持つ別channelもlinear probingで正しく保持する。
      }
      index = (index + 1) & (kBucketCount - 1);
    }
  }

  __attribute__((always_inline)) ChannelLookup
  prefetch(std::uint64_t hash, std::uint32_t tail_index) noexcept {
    const ChannelLookup lookup = channel_lookup(hash, tail_index);
    prefetch_lookup(lookup);
    return lookup;
  }

  __attribute__((always_inline)) void
  prefetch_lookup(ChannelLookup lookup) noexcept {
    __builtin_prefetch(storage_->channel_entries.data() +
                           (lookup & (kBucketCount - 1)),
                       0, 3);
  }

  __attribute__((always_inline)) std::uint64_t
  get_existing(std::uint64_t lookup, HotStorage *hot_storage) noexcept {
    // max種類到達時に全probe列を検証してsteady形式へrewriteした後だけ呼ばれる。
    std::size_t index =
        lookup & (kBucketCount - 1);
    std::uint64_t entry = hot_storage->channel_entries[index];
    std::uint64_t difference = lookup;

    // home hitでは比較とslot-code化まで同じraw entry registerで行う。
    // collisionへのjumpはANDより前なので、その経路のentryはrawのまま。
    __asm__ goto(
        "{xorl %k1, %k0|xor %k0, %k1}\n\t"
        "{cmpl $%c[slot_mask], %k0|cmp %k0, %c[slot_mask]}\n\t"
        "{ja %l[collision]|ja %l[collision]}\n\t"
        "{andl $%c[slot_mask], %k1|and %k1, %c[slot_mask]}"
        : "+r"(difference), "+r"(entry)
        : [slot_mask] "i"(kSteadySlotCodeMask)
        : "cc"
        : collision);
    return entry;

  collision:
    // failed comparison left difference=lookup^entry; restore lookup before
    // probing, avoiding a second live copy of lookup on the home path.
    __asm__("{xorl %k1, %k0|xor %k0, %k1}"
            : "+r"(difference)
            : "r"(entry)
            : "cc");
    index = (index + 1) & (kBucketCount - 1);
    entry = hot_storage->channel_entries[index];
    __asm__ goto(
        "{xorl %k1, %k0|xor %k0, %k1}\n\t"
        "{cmpl $%c[slot_mask], %k0|cmp %k0, %c[slot_mask]}\n\t"
        "{ja %l[collision]|ja %l[collision]}\n\t"
        "{andl $%c[slot_mask], %k1|and %k1, %c[slot_mask]}"
        : "+r"(difference), "+r"(entry)
        : [slot_mask] "i"(kSteadySlotCodeMask)
        : "cc"
        : collision);
    return entry;
  }

  bool can_use_hash_only_lookup() const noexcept {
    return hash_only_lookup_ready_;
  }

  HashOnlyLookupMode hash_only_lookup_mode() const noexcept {
    return hash_only_lookup_mode_;
  }

  const std::vector<ChannelRef> &channels() const noexcept { return channels_; }
  std::uint16_t channel_count() const noexcept { return channel_count_; }
  MonthSlot *month_slots() noexcept { return storage_->month_slots.data(); }
  const MonthSlot *month_slots() const noexcept {
    return storage_->month_slots.data();
  }
  HotStorage *hot_storage() noexcept { return storage_.get(); }

private:
  // mmap上の文字列参照のvectorへの追加を含む初出処理を分離し、通常の
  // 検索を小さく保って呼び出し元の行ループへインライン化する。
  __attribute__((noinline)) std::uint32_t
  insert_new(std::size_t index, const char *text, std::uint32_t length,
             std::uint64_t hash, std::uint32_t fingerprint) {
    const std::uint16_t channel_id = channel_count_++;
    // entryの下位14 bitへid+1を埋め、0をempty sentinelにする。
    storage_->channel_entries[index] =
        fingerprint | static_cast<std::uint32_t>(channel_id + 1);
    const std::uint64_t short_high =
        length <= 15 ? short_channel_high(text, length) : 0;
    channels_.push_back({text, hash, short_high, length});
    if (channel_count_ == kMaxChannels) {
      validate_unique_fingerprints();
    }
    return static_cast<std::uint32_t>(channel_id) *
           static_cast<std::uint32_t>(kMonthsPerYear);
  }

  // 初出期間の18-bit tagは文字列比較で補う。steady形式ではlow15をslot codeへ
  // 使うためtagは17 bitになる。全channelのprobe列を事前検証し、曖昧さが
  // 一つでもあればrewriteせず文字列比較経路を継続する。
  __attribute__((noinline)) void validate_unique_fingerprints() {
    if (try_rebuild_suffix8_table()) {
      hash_only_lookup_ready_ = true;
      hash_only_lookup_mode_ = HashOnlyLookupMode::suffix8;
      return;
    }
    if (try_rebuild_aes2_folded_table()) {
      hash_only_lookup_ready_ = true;
      hash_only_lookup_mode_ = HashOnlyLookupMode::aes2_folded;
      return;
    }

    hash_only_lookup_ready_ = true;
    for (std::uint32_t channel_id = 0; channel_id < channel_count_;
         ++channel_id) {
      const ChannelLookup lookup = channel_lookup(
          channels_[channel_id].hash, channels_[channel_id].length & 15U);
      std::size_t index = lookup & (kBucketCount - 1);
      for (;;) {
        const std::uint32_t entry = storage_->channel_entries[index];
        const std::uint16_t encoded_id =
            static_cast<std::uint16_t>(entry & kChannelIdMask);
        if (encoded_id == 0) {
          hash_only_lookup_ready_ = false;
          return;
        }
        if (((entry ^ lookup) &
             ~static_cast<std::uint32_t>(kSteadySlotCodeMask)) == 0) {
          if (encoded_id != channel_id + 1) {
            hash_only_lookup_ready_ = false;
          }
          break;
        }
        index = (index + 1) & (kBucketCount - 1);
      }
      if (!hash_only_lookup_ready_) {
        return;
      }
    }

    // generic形式のlow14=id+1を、steady形式のlow15=3*idへ一度だけ変換する。
    for (std::uint32_t &entry : storage_->channel_entries) {
      const std::uint32_t encoded_id = entry & kChannelIdMask;
      if (encoded_id != 0) {
        const std::uint32_t channel_id = encoded_id - 1;
        entry = (entry & ~kSteadySlotCodeMask) | channel_id * 3;
      }
    }
    hash_only_lookup_mode_ = HashOnlyLookupMode::folded;
  }

  // 8文字以上は末尾8 byteだけをCRC32Cへ入れるsteady専用表をcoldに試す。
  // 曖昧なprobe列が一つでもあれば既存のfold hash表を変更せずfallbackする。
  __attribute__((noinline)) bool try_rebuild_suffix8_table() {
    std::array<std::uint32_t, kBucketCount> candidate{};
    for (std::uint32_t channel_id = 0; channel_id < channel_count_;
         ++channel_id) {
      const ChannelLookup lookup =
          suffix8_channel_lookup(channels_[channel_id]);
      std::size_t index = lookup & (kBucketCount - 1);
      while (candidate[index] != 0) {
        index = (index + 1) & (kBucketCount - 1);
      }
      candidate[index] =
          (lookup & ~static_cast<std::uint32_t>(kChannelIdMask)) |
          (channel_id + 1);
    }

    for (std::uint32_t channel_id = 0; channel_id < channel_count_;
         ++channel_id) {
      const ChannelLookup lookup =
          suffix8_channel_lookup(channels_[channel_id]);
      std::size_t index = lookup & (kBucketCount - 1);
      for (;;) {
        const std::uint32_t entry = candidate[index];
        if (entry == 0) {
          return false;
        }
        if (((entry ^ lookup) &
             ~static_cast<std::uint32_t>(kSteadySlotCodeMask)) == 0) {
          if ((entry & kChannelIdMask) != channel_id + 1) {
            return false;
          }
          break;
        }
        index = (index + 1) & (kBucketCount - 1);
      }
    }

    for (std::uint32_t &entry : candidate) {
      const std::uint32_t encoded_id = entry & kChannelIdMask;
      if (encoded_id != 0) {
        entry = (entry & ~kSteadySlotCodeMask) | (encoded_id - 1) * 3;
      }
    }
    storage_->channel_entries = candidate;
    return true;
  }

  // XOR-fold前の完全な128-bit stateをAESENC 2 roundで十分に拡散する。
  // suffix8で曖昧な分布だけこの候補を試し、失敗時は従来foldへ戻す。
  __attribute__((noinline)) bool try_rebuild_aes2_folded_table() {
    std::array<std::uint32_t, kBucketCount> candidate{};
    for (std::uint32_t channel_id = 0; channel_id < channel_count_;
         ++channel_id) {
      const ChannelLookup lookup =
          aes2_folded_channel_lookup(channels_[channel_id]);
      std::size_t index = lookup & (kBucketCount - 1);
      while (candidate[index] != 0) {
        index = (index + 1) & (kBucketCount - 1);
      }
      candidate[index] =
          (lookup & ~static_cast<std::uint32_t>(kChannelIdMask)) |
          (channel_id + 1);
    }

    for (std::uint32_t channel_id = 0; channel_id < channel_count_;
         ++channel_id) {
      const ChannelLookup lookup =
          aes2_folded_channel_lookup(channels_[channel_id]);
      std::size_t index = lookup & (kBucketCount - 1);
      for (;;) {
        const std::uint32_t entry = candidate[index];
        if (entry == 0) {
          return false;
        }
        if (((entry ^ lookup) &
             ~static_cast<std::uint32_t>(kSteadySlotCodeMask)) == 0) {
          if ((entry & kChannelIdMask) != channel_id + 1) {
            return false;
          }
          break;
        }
        index = (index + 1) & (kBucketCount - 1);
      }
    }

    for (std::uint32_t &entry : candidate) {
      const std::uint32_t encoded_id = entry & kChannelIdMask;
      if (encoded_id != 0) {
        entry = (entry & ~kSteadySlotCodeMask) | (encoded_id - 1) * 3;
      }
    }
    storage_->channel_entries = candidate;
    return true;
  }

  // 最大10,000種類に対してload factor 31%。18-bit fingerprintと14-bit IDを
  // 1 wordへpackingし、表全体を128 KiB/threadに収める。
  static constexpr std::size_t kBucketCount = kChannelBucketCount;
  HotStoragePtr storage_;
  std::vector<ChannelRef> channels_;
  std::uint16_t channel_count_ = 0;
  bool hash_only_lookup_ready_ = false;
  HashOnlyLookupMode hash_only_lookup_mode_ = HashOnlyLookupMode::none;
};

// -----------------------------------------------------------------------------
// スレッド内集約とUTC月変換
// -----------------------------------------------------------------------------

struct WideLengthRange {
  // max_length==0は未使用。message_length>=1なので値と衝突しない。
  std::uint32_t min_length;
  std::uint32_t max_length;
  std::uint32_t message_count;
};

struct WideLengthDeleter {
  void operator()(WideLengthRange *ranges) const noexcept {
    std::free(ranges);
  }
};

using WideLengthPtr =
    std::unique_ptr<WideLengthRange[], WideLengthDeleter>;

struct DeferredWideLength {
  const char *numeric_text;
  std::uint32_t message_length;
};

struct PartialResult {
  ChannelTable channel_table;
  WideLengthPtr wide_lengths;
  std::vector<DeferredWideLength> deferred_wide_lengths;
};

// slow numeric parserからのみ参照し、hot parserの引数は増やさない。
static thread_local std::vector<DeferredWideLength> *
    active_deferred_wide_lengths = nullptr;

// 入力仕様は2027年限定。Unix秒の上4桁が表す100万秒区間には
// 月境界が高々1個しかない。全境界は末尾2桁が00なので、先頭8桁を
// big-endian ASCIIのまま比較すれば、10桁の整数変換を省ける。
static constexpr std::int32_t kFirst2027MonthKey = 2027 * 12;
static constexpr std::array<std::uint64_t, 33> k2027PrefixBoundaries = {
    UINT64_MAX, UINT64_MAX, UINT64_MAX, 0x3138303134343030ULL,
    UINT64_MAX, 0x3138303338353932ULL, UINT64_MAX, UINT64_MAX,
    0x3138303635333736ULL, UINT64_MAX, UINT64_MAX,
    0x3138303931323936ULL, UINT64_MAX, 0x3138313138303830ULL,
    UINT64_MAX, UINT64_MAX, 0x3138313434303030ULL, UINT64_MAX,
    UINT64_MAX, 0x3138313730373834ULL, UINT64_MAX,
    0x3138313937353638ULL, UINT64_MAX, UINT64_MAX,
    0x3138323233343838ULL, UINT64_MAX, UINT64_MAX,
    0x3138323530323732ULL, UINT64_MAX, 0x3138323736313932ULL,
    UINT64_MAX, UINT64_MAX, 0x3138333032393736ULL,
};
static constexpr std::array<std::uint8_t, 33> k2027PrefixBaseMonths = {
    0, 0, 0, 0, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7,
    8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11,
};

// timestamp先頭4桁の末尾2桁をASCII low nibbleのままsparse indexにする。
// (base+1)*2^32-boundaryを格納し、suffix加算時のcarryで月を得る。
static constexpr auto k2027PackedMonthsByAsciiNibbles = [] {
  std::array<std::uint64_t, 4096> result{};
  for (std::size_t index = 0; index < k2027PrefixBaseMonths.size(); ++index) {
    const std::size_t last_two = index < 2 ? index + 98 : index - 2;
    const std::size_t sparse_index =
        (last_two / 10) | ((last_two % 10) << 8);
    const std::uint32_t boundary =
        static_cast<std::uint32_t>(k2027PrefixBoundaries[index]);
    result[sparse_index] =
        ((static_cast<std::uint64_t>(k2027PrefixBaseMonths[index]) + 1)
         << 32) - boundary;
  }
  return result;
}();

static inline std::uint64_t month_word_2027(const char *text) noexcept {
  std::uint16_t segment_ascii;
  std::memcpy(&segment_ascii, text + 2, sizeof(segment_ascii));
  std::uint32_t suffix_ascii;
  std::memcpy(&suffix_ascii, text + 4, sizeof(suffix_ascii));
  const std::uint32_t suffix32 = std::byteswap(suffix_ascii);
  // x86の32-bit register writeは上位32 bitをzeroにする。tied operandで
  // MOVBE直後の冗長なself-moveをGCCに再生成させず、その64-bit値を使う。
  std::uint64_t suffix;
  __asm__("" : "=r"(suffix) : "0"(suffix32));

  // 各byteがASCII digitなのでborrowは起きず、segment-0x3030は
  // segment&0x0f0fと同値。biasをaddress displacementへfoldさせる。
  const std::uintptr_t packed_address =
      reinterpret_cast<std::uintptr_t>(
          k2027PackedMonthsByAsciiNibbles.data()) +
      static_cast<std::uintptr_t>(segment_ascii) * sizeof(std::uint64_t) -
      UINT64_C(0x3030) * sizeof(std::uint64_t);
  std::uint64_t packed;
  std::memcpy(&packed, reinterpret_cast<const void *>(packed_address),
              sizeof(packed));
  return packed + suffix;
}

static inline std::uint32_t month_slot_2027(const char *text) noexcept {
  return static_cast<std::uint32_t>(month_word_2027(text) >> 32);
}

static inline std::uint64_t
pack_lookup_and_month_2027(const char *text, std::uint64_t lookup) noexcept {
  std::uint64_t result = month_word_2027(text);
  // high32=lookup, low32=monthへSHLD一発でpackingする。
  __asm__("{shldq $32, %1, %0|shld %0, %1, 32}"
          : "+r"(lookup)
          : "r"(result)
          : "cc");
  return lookup;
}

[[gnu::cold, gnu::noinline]] static void
add_wide_message_length(PartialResult &result, const MonthSlot &slot,
                        std::uint32_t message_length) {
  if (!result.wide_lengths) {
    auto *const ranges = static_cast<WideLengthRange *>(
        std::calloc(kMaxAggregates, sizeof(WideLengthRange)));
    if (ranges == nullptr) {
      std::abort();
    }
    result.wide_lengths.reset(ranges);
  }
  const std::size_t slot_index =
      static_cast<std::size_t>(&slot - result.channel_table.month_slots());
  WideLengthRange &range = result.wide_lengths[slot_index];
  if (range.max_length == 0) {
    range.min_length = message_length;
    range.max_length = message_length;
    range.message_count = 1;
    return;
  }
  range.min_length = std::min(range.min_length, message_length);
  range.max_length = std::max(range.max_length, message_length);
  ++range.message_count;
}

static inline void add_message(PartialResult &result, MonthSlot &slot,
                               __m128i counters) noexcept {
  (void)result;
  CompactStats &stats = slot.stats;
#if defined(__AVX512F__) && defined(__AVX512VL__)
  const __m128i old_stats = _mm_load_si128(
      reinterpret_cast<const __m128i *>(&stats));
  __m128i new_stats = _mm_add_epi32(old_stats, counters);
  __m128i candidate = _mm_broadcastw_epi16(counters);
  // [ffff x6, length, ~length] = invert_word7 ^
  //                                (broadcast_length | prefix_ones)
  candidate = _mm_ternarylogic_epi32(
      candidate, _mm_setr_epi32(-1, -1, -1, 0),
      _mm_setr_epi32(0, 0, 0, static_cast<int>(0xffff0000U)), 0x56);
  new_stats = _mm_min_epu16(new_stats, candidate);
  _mm_store_si128(reinterpret_cast<__m128i *>(&stats), new_stats);
#else
  const std::uint64_t length_and_stamps =
      static_cast<std::uint64_t>(_mm_cvtsi128_si64(counters));
  const std::uint16_t message_length =
      static_cast<std::uint16_t>(length_and_stamps);
  stats.length_and_stamps += length_and_stamps;
  ++stats.message_count_value;
  stats.min_length = std::min(stats.min_length, message_length);
  const std::uint16_t old_max =
      static_cast<std::uint16_t>(~stats.inverted_max_length);
  stats.inverted_max_length = static_cast<std::uint16_t>(
      ~std::max(old_max, message_length));
#endif
}

// -----------------------------------------------------------------------------
// CSV行のホットパス
// -----------------------------------------------------------------------------

struct ParsedChannel {
  const char *text;
  std::uint64_t hash;
  std::uint32_t length;
};
static_assert(sizeof(ParsedChannel) == 24);

#if !defined(DISABLE_SIMD_CHANNEL) && defined(__AVX512BW__) && \
    defined(__AVX512VL__)
static inline std::uint64_t reduce_channel_hash(__m128i value) noexcept {
  value = _mm_xor_si128(value, _mm_srli_si128(value, 8));
  return static_cast<std::uint64_t>(_mm_cvtsi128_si64(value));
}
#endif

// 16-byte内にcommaがある行が約89%なので、先頭blockをpeelする。
// full blockはXMMのままXORし、最後にlow/high laneを畳むことで従来の
// 2xuint64 XOR hashと同じ値を返す。
struct [[gnu::packed, gnu::may_alias]] UnalignedU64 {
  std::uint64_t value;
};

template <bool ReturnLookup, bool Suffix8Lookup = false,
          bool Aes2Lookup = false>
static inline std::uint64_t
scan_channel_hash_impl(const char *&cursor,
                       std::uint64_t *numeric_word = nullptr) noexcept {
  static_assert(!(Suffix8Lookup && Aes2Lookup));
#if !defined(DISABLE_SIMD_CHANNEL) && defined(__AVX512BW__) && \
    defined(__AVX512VL__)
  const __m128i comma = _mm_set1_epi8(',');
  const char *scan = cursor;
  __m128i bytes =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(scan));
  std::uint32_t comma_mask = static_cast<std::uint32_t>(
      _mm_movemask_epi8(_mm_cmpeq_epi8(bytes, comma)));

  std::uint64_t comma_bits;
  __asm__("" : "=r"(comma_bits) : "0"(comma_mask));
  std::uint32_t keep_bits;
  std::uint64_t comma_index;
  // BLSMSKは入力0のときだけCF=1。通常のcommaありをfall-throughさせ、
  // TESTを省いたままmaskとcomma位置を得る。
  if constexpr (ReturnLookup && Suffix8Lookup) {
    __asm__ goto(
        "{testl %k1, %k1|test %k1, %k1}\n\t"
        "jz %l[long_channel]\n\t"
        "{tzcntq %1, %0|tzcnt %0, %1}"
        : "=&r"(comma_index)
        : "r"(comma_bits)
        : "cc"
        : long_channel);
  } else {
    __asm__ goto(
        "{blsmskl %k2, %0|blsmsk %0, %k2}\n\t"
        "jc %l[long_channel]\n\t"
        "{tzcntq %2, %1|tzcnt %1, %2}"
        : "=&r"(keep_bits), "=&r"(comma_index)
        : "r"(comma_bits)
        : "cc"
        : long_channel);
  }
  {
    cursor = scan + comma_index + 1;
    if constexpr (ReturnLookup && Suffix8Lookup) {
      std::memcpy(numeric_word, cursor, sizeof(*numeric_word));
      if (comma_index >= 8) [[likely]] {
        const auto &suffix_memory =
            *reinterpret_cast<const UnalignedU64 *>(cursor - 9);
        __asm__("{crc32q -9(%2), %0|crc32 %0, QWORD PTR [%2-9]}"
                : "+r"(comma_index)
                : "r"(*numeric_word), "r"(cursor), "m"(suffix_memory)
                : "cc");
        return comma_index;
      }
      keep_bits = _blsmsk_u32(static_cast<std::uint32_t>(comma_bits));
    }
    const __mmask16 keep = static_cast<__mmask16>(keep_bits);
    bytes = _mm_maskz_mov_epi8(keep, bytes);
    if constexpr (ReturnLookup) {
      if constexpr (!Suffix8Lookup) {
        std::memcpy(numeric_word, cursor, sizeof(*numeric_word));
      }
      if constexpr (Aes2Lookup) {
        return aes2_state_lookup(bytes, comma);
      }
      const std::uint64_t hash = reduce_channel_hash(bytes);
      // comma_indexはchannel length mod 16で、同じchannelなら安定する。
      // 第2の未参照asm inputはnumeric loadをCRCより前へ配置するためだけに
      // 使い、実命令のCRC依存鎖は増やさない。
      __asm__("{crc32q %1, %0|crc32 %0, %1}"
              : "+r"(comma_index)
              : "r"(hash), "r"(*numeric_word)
              : "cc");
      return comma_index;
    }
    return reduce_channel_hash(bytes);
  }

long_channel:
  __m128i state = bytes;
  scan += 16;
  for (;;) {
    bytes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(scan));
    comma_mask = static_cast<std::uint32_t>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(bytes, comma)));
    __asm__("" : "=r"(comma_bits) : "0"(comma_mask));
    if constexpr (ReturnLookup && Suffix8Lookup) {
      __asm__ goto(
          "{testl %k1, %k1|test %k1, %k1}\n\t"
          "jz %l[full_block]\n\t"
          "{tzcntq %1, %0|tzcnt %0, %1}"
          : "=&r"(comma_index)
          : "r"(comma_bits)
          : "cc"
          : full_block);
    } else {
      __asm__ goto(
          "{blsmskl %k2, %0|blsmsk %0, %k2}\n\t"
          "jc %l[full_block]\n\t"
          "{tzcntq %2, %1|tzcnt %1, %2}"
          : "=&r"(keep_bits), "=&r"(comma_index)
          : "r"(comma_bits)
          : "cc"
          : full_block);
    }
    {
      cursor = scan + comma_index + 1;
      if constexpr (ReturnLookup && Suffix8Lookup) {
        std::memcpy(numeric_word, cursor, sizeof(*numeric_word));
        const auto &suffix_memory =
            *reinterpret_cast<const UnalignedU64 *>(cursor - 9);
        __asm__("{crc32q -9(%2), %0|crc32 %0, QWORD PTR [%2-9]}"
                : "+r"(comma_index)
                : "r"(*numeric_word), "r"(cursor), "m"(suffix_memory)
                : "cc");
        return comma_index;
      }
      const __mmask16 keep = static_cast<__mmask16>(keep_bits);
      bytes = _mm_maskz_mov_epi8(keep, bytes);
      state = _mm_xor_si128(state, bytes);
      if constexpr (ReturnLookup) {
        std::memcpy(numeric_word, cursor, sizeof(*numeric_word));
        if constexpr (Aes2Lookup) {
          return aes2_state_lookup(state, comma);
        }
        const std::uint64_t hash = reduce_channel_hash(state);
        __asm__("{crc32q %1, %0|crc32 %0, %1}"
                : "+r"(comma_index)
                : "r"(hash), "r"(*numeric_word)
                : "cc");
        return comma_index;
      }
      return reduce_channel_hash(state);
    }

  full_block:
    {
      if constexpr (!Suffix8Lookup) {
        state = _mm_xor_si128(state, bytes);
      }
      scan += 16;
      continue;
    }
  }
#else
  constexpr std::uint64_t kCommas = 0x2c2c2c2c2c2c2c2cULL;
  constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
  constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
  const char *const channel_start = cursor;
  std::uint64_t hash = 0;
  std::uint64_t aes_low = 0;
  std::uint64_t aes_high = 0;
  for (;;) {
    std::uint64_t word0;
    std::uint64_t word1;
    std::memcpy(&word0, cursor, sizeof(word0));
    std::memcpy(&word1, cursor + 8, sizeof(word1));
    const std::uint64_t mask0 = ((word0 ^ kCommas) - kOnes) & kHighBits;
    const std::uint64_t mask1 = ((word1 ^ kCommas) - kOnes) & kHighBits;
    if ((mask0 | mask1) == 0) {
      hash ^= word0 ^ word1;
      if constexpr (Aes2Lookup) {
        aes_low ^= word0;
        aes_high ^= word1;
      }
      cursor += 16;
      continue;
    }

    const std::uint64_t second = static_cast<std::uint64_t>(mask0 == 0);
    const std::uint64_t select_second = UINT64_C(0) - second;
    hash ^= word0 & select_second;
    const std::uint64_t word =
        word0 ^ ((word0 ^ word1) & select_second);
    const std::uint64_t comma_mask = mask0 | (mask1 & select_second);
    cursor += second * 8;
    const unsigned comma_bit = std::countr_zero(comma_mask);
    const std::uint64_t keep_mask = _blsmsk_u64(comma_mask);
    hash ^= word & keep_mask;
    if constexpr (Aes2Lookup) {
      if (second != 0) {
        aes_low ^= word0;
        aes_high ^= word & keep_mask;
      } else {
        aes_low ^= word & keep_mask;
      }
    }
    cursor += comma_bit / 8 + 1;
    if constexpr (ReturnLookup) {
      std::memcpy(numeric_word, cursor, sizeof(*numeric_word));
      std::uint64_t tail = second * 8 + comma_bit / 8;
      if constexpr (Suffix8Lookup) {
        if (static_cast<std::size_t>(cursor - channel_start - 1) >= 8) {
          std::uint64_t suffix;
          std::memcpy(&suffix, cursor - 9, sizeof(suffix));
          __asm__("{crc32q %1, %0|crc32 %0, %1}"
                  : "+r"(tail)
                  : "r"(suffix), "r"(*numeric_word)
                  : "cc");
          return tail;
        }
      }
      if constexpr (Aes2Lookup) {
        const __m128i state =
            _mm_set_epi64x(static_cast<long long>(aes_high),
                           static_cast<long long>(aes_low));
        const __m128i comma_key = _mm_set1_epi8(',');
        return aes2_state_lookup(state, comma_key);
      }
      __asm__("{crc32q %1, %0|crc32 %0, %1}"
              : "+r"(tail)
              : "r"(hash), "r"(*numeric_word)
              : "cc");
      return tail;
    }
    return hash;
  }
#endif
}

static inline ParsedChannel parse_channel(const char *&cursor) noexcept {
  const char *const start = cursor;
  const std::uint64_t hash = scan_channel_hash_impl<false>(cursor, nullptr);
  return {start, hash,
          static_cast<std::uint32_t>(cursor - start - 1)};
}

// 各スレッドで全10,000チャンネルを登録した後は、初出処理にしか要らない
// 文字列先頭と長さを保持しない。
struct ParsedExistingChannelLookup {
  std::uint64_t lookup;
  std::uint64_t numeric_word;
};

template <bool Suffix8Lookup, bool Aes2Lookup = false>
static inline ParsedExistingChannelLookup
parse_existing_channel_lookup(const char *&cursor) noexcept {
  std::uint64_t numeric_word;
  const std::uint64_t lookup =
      scan_channel_hash_impl<true, Suffix8Lookup, Aes2Lookup>(
          cursor, &numeric_word);
  return {lookup, numeric_word};
}

struct ParsedStampCount {
  const char *next;
  std::uint32_t value;
};
static_assert(sizeof(ParsedStampCount) == 16);

struct ParsedMessageLength {
  const char *next;
  std::uint32_t value;
};
static_assert(sizeof(ParsedMessageLength) == 16);

// 6〜10桁だけが来る分離経路。先頭5桁に続く最大5桁を展開し、
// 汎用loopの分岐とincrementを省く。
[[gnu::noinline]] static ParsedMessageLength
parse_message_length_wide(const char *cursor) noexcept {
  std::uint32_t value =
      static_cast<std::uint32_t>((cursor[0] - '0') * 10000 +
                                 (cursor[1] - '0') * 1000 +
                                 (cursor[2] - '0') * 100 +
                                 (cursor[3] - '0') * 10 +
                                 (cursor[4] - '0'));
  value = value * 10 + static_cast<std::uint32_t>(cursor[5] - '0');
  if (cursor[6] == ',') {
    return {cursor + 7, value};
  }
  value = value * 10 + static_cast<std::uint32_t>(cursor[6] - '0');
  if (cursor[7] == ',') {
    return {cursor + 8, value};
  }
  value = value * 10 + static_cast<std::uint32_t>(cursor[7] - '0');
  if (cursor[8] == ',') {
    return {cursor + 9, value};
  }
  value = value * 10 + static_cast<std::uint32_t>(cursor[8] - '0');
  if (cursor[9] == ',') {
    return {cursor + 10, value};
  }
  value = value * 10 + static_cast<std::uint32_t>(cursor[9] - '0');
  return {cursor + 11, value};
}

static inline std::uint32_t
parse_message_length(const char *&cursor) noexcept {
  // 頻出する3桁、2桁を先に処理する。
  if (cursor[3] == ',') [[likely]] {
    std::uint32_t packed_ascii;
    std::memcpy(&packed_ascii, cursor, sizeof(packed_ascii));
    const __m128i digits = _mm_sub_epi8(
        _mm_cvtsi32_si128(static_cast<int>(packed_ascii)),
        _mm_set1_epi8('0'));
    const __m128i pair_values = _mm_maddubs_epi16(
        digits, _mm_setr_epi8(10, 1, 1, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0));
    const __m128i value = _mm_madd_epi16(
        pair_values, _mm_setr_epi16(10, 1, 0, 0, 0, 0, 0, 0));
    const std::uint32_t length =
        static_cast<std::uint32_t>(_mm_cvtsi128_si32(value));
    cursor += 4;
    return length;
  }
  if (cursor[2] == ',') {
    const std::uint32_t length = static_cast<std::uint32_t>(
        (cursor[0] - '0') * 10 + (cursor[1] - '0'));
    cursor += 3;
    return length;
  }
  if (cursor[1] == ',') {
    const std::uint32_t length = static_cast<std::uint32_t>(cursor[0] - '0');
    cursor += 2;
    return length;
  }
  if (cursor[4] == ',') {
    const std::uint32_t length = static_cast<std::uint32_t>(
        (cursor[0] - '0') * 1000 + (cursor[1] - '0') * 100 +
        (cursor[2] - '0') * 10 + (cursor[3] - '0'));
    cursor += 5;
    return length;
  }
  if (cursor[5] == ',') {
    const std::uint32_t length = static_cast<std::uint32_t>(
        (cursor[0] - '0') * 10000 + (cursor[1] - '0') * 1000 +
        (cursor[2] - '0') * 100 + (cursor[3] - '0') * 10 +
        (cursor[4] - '0'));
    cursor += 6;
    return length;
  }
  const ParsedMessageLength parsed = parse_message_length_wide(cursor);
  cursor = parsed.next;
  return parsed.value;
}

[[gnu::cold, gnu::noinline]] static ParsedStampCount
parse_stamp_count_slow(const char *cursor) noexcept {
  std::uint32_t stamps = *cursor++ - '0';
  if (*cursor != '\n') {
    do {
      stamps = stamps * 10 + (*cursor++ - '0');
    } while (*cursor != '\n');
  }
  return {cursor + 1, stamps};
}

static inline ParsedStampCount parse_stamp_count(const char *cursor) noexcept {
  // 入力は必ずLF終端なので、通常の1桁値＋LFをまとめて処理する。
  if (cursor[1] == '\n') [[likely]] {
    const std::uint32_t stamps = cursor[0] - '0';
    return {cursor + 2, stamps};
  }
  return parse_stamp_count_slow(cursor);
}

static inline __m128i
counter_vector(std::uint64_t length_and_stamps) noexcept {
  return _mm_set_epi64x(1, static_cast<long long>(length_and_stamps));
}

struct SlowLengthAndStamps {
  const char *next;
  std::uint64_t value;
};
static_assert(sizeof(SlowLengthAndStamps) == 16);

[[gnu::cold, gnu::noinline]] static SlowLengthAndStamps
parse_length_and_stamps_deferred_slow(const char *cursor) noexcept {
  const char *const numeric_text = cursor;
  const std::uint32_t message_length = parse_message_length(cursor);
  const ParsedStampCount stamp_count = parse_stamp_count(cursor);
  std::uint32_t compact_length = message_length;
  if (message_length > std::numeric_limits<std::uint16_t>::max() &&
      active_deferred_wide_lengths != nullptr) [[unlikely]] {
    active_deferred_wide_lengths->push_back(
        {numeric_text, message_length});
    compact_length = std::numeric_limits<std::uint16_t>::max();
  }
  return {stamp_count.next,
          static_cast<std::uint64_t>(compact_length) |
              (static_cast<std::uint64_t>(stamp_count.value) << 32)};
}

[[gnu::always_inline]] static inline __m128i
parse_length_and_stamps_preloaded(const char *&cursor,
                                  std::uint64_t word) noexcept {
#if defined(__AVXVNNI__) || defined(__AVX512VNNI__)
  // byte3=commaかつbyte5=LFなら、3桁lengthと1桁stampの通常形。
  // PEXT maskはloop外へhoistされ、hot pathではPEXT+CMPだけになる。
  if (_pext_u64(word, UINT64_C(0x0000ff00ff000000)) ==
      UINT32_C(0x00000a2c)) [[likely]] {
    // dword0: 100*b0+10*b1+b2-48*111 = message_length
    // dword1: b4-48 = stamp_count
    const __m128i ascii =
        _mm_cvtsi64_si128(std::bit_cast<long long>(word));
    const __m128i bias = _mm_setr_epi32(-5328, -48, 1, 0);
    const __m128i weights =
        _mm_setr_epi8(100, 10, 1, 0, 1, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0);
#if defined(__AVXVNNI__)
    const __m128i parsed =
        _mm_dpbusd_avx_epi32(bias, ascii, weights);
#else
    const __m128i parsed = _mm_dpbusd_epi32(bias, ascii, weights);
#endif
    cursor += 6;
    return parsed;
  }

  // byte2=commaかつbyte4=LFなら、2桁lengthと1桁stamp。
  if ((std::rotr(word, 16) & UINT32_C(0x00ff00ff)) ==
      UINT32_C(0x000a002c)) {
    const std::uint32_t first = static_cast<std::uint32_t>(word) & 0xffU;
    const std::uint32_t second =
        static_cast<std::uint32_t>(word >> 8) & 0xffU;
    const std::uint32_t stamps =
        (static_cast<std::uint32_t>(word >> 24) & 0xffU) - '0';
    const std::uint32_t length = first * 10 + second - 48 * 11;
    cursor += 5;
    return counter_vector(static_cast<std::uint64_t>(length) |
                          (static_cast<std::uint64_t>(stamps) << 32));
  }

  // byte1=commaかつbyte3=LFなら、1桁lengthと1桁stamp。
  if ((std::rotr(word, 8) & UINT32_C(0x00ff00ff)) ==
      UINT32_C(0x000a002c)) {
    const std::uint64_t digits =
        static_cast<std::uint32_t>(word) & UINT32_C(0x000f000f);
    cursor += 4;
    return counter_vector(
        digits + (digits & UINT32_C(0x000f0000)) * UINT32_C(65535));
  }
#endif

  const SlowLengthAndStamps parsed =
      parse_length_and_stamps_deferred_slow(cursor);
  cursor = parsed.next;
  return counter_vector(parsed.value);
}

static inline std::uint64_t
parse_length_and_stamps(const char *&cursor) noexcept {
  std::uint64_t word;
  std::memcpy(&word, cursor, sizeof(word));
  return static_cast<std::uint64_t>(_mm_cvtsi128_si64(
      parse_length_and_stamps_preloaded(cursor, word)));
}

struct ParsedExistingRow {
  std::uint64_t lookup;
  std::uint64_t month;
  // dword0=message_length, dword1=stamp_count, dword2=message_count(1)
  __m128i counters;
};
static_assert(sizeof(ParsedExistingRow) == 32);

struct ResolvedExistingRow {
  MonthSlot *slot;
  __m128i counters;
};
static_assert(sizeof(ResolvedExistingRow) == 32);

template <bool Suffix8Lookup, bool Aes2Lookup = false>
[[gnu::always_inline]] static inline ParsedExistingRow
parse_existing_row(HotStorage *storage, const char *&cursor) noexcept {
  const char *const timestamp_text = cursor;
  cursor += 11;
  const ParsedExistingChannelLookup parsed_channel =
      parse_existing_channel_lookup<Suffix8Lookup, Aes2Lookup>(cursor);
  const std::uint64_t lookup = parsed_channel.lookup;
  __builtin_prefetch(storage->channel_entries.data() +
                         (static_cast<ChannelLookup>(lookup) &
                          (kChannelBucketCount - 1)),
                     0, 3);
  const __m128i counters =
      parse_length_and_stamps_preloaded(cursor, parsed_channel.numeric_word);
  const std::uint32_t month = month_slot_2027(timestamp_text);
  return {lookup, static_cast<std::uint64_t>(month), counters};
}

static inline ResolvedExistingRow
resolve_existing_row(ChannelTable &channel_table, HotStorage *storage,
                     const ParsedExistingRow &row) noexcept {
  const std::uint64_t slot_code =
      channel_table.get_existing(row.lookup, storage);
  const std::uint64_t slot_index =
      static_cast<std::uint64_t>(slot_code) * 4 + row.month;
  std::uintptr_t slot_offset = slot_index;
  MonthSlot *slot;
  __asm__("{shlq $4, %[offset]|shl %[offset], 4}\n\t"
          "{leaq %c[month_base](%[base],%[offset]), %[slot]|"
          "lea %[slot], [%[base]+%[offset]+%c[month_base]]}"
          : [slot] "=&r"(slot), [offset] "+&r"(slot_offset)
          : [base] "r"(storage),
            [month_base] "i"(offsetof(HotStorage, month_slots))
          : "cc");
  __builtin_prefetch(slot, 1, 3);
  return {slot, row.counters};
}

static inline void add_resolved_row(PartialResult &result,
                                    const ResolvedExistingRow &row) noexcept {
  add_message(result, *row.slot, row.counters);
}

template <bool Suffix8Lookup, bool Aes2Lookup = false>
[[gnu::cold, gnu::noinline]] static void
apply_deferred_wide_lengths(PartialResult &result,
                            HotStorage *storage) noexcept {
  for (const DeferredWideLength &deferred : result.deferred_wide_lengths) {
    // input mappingにheaderがあるため、先頭data行にも直前LFがある。
    const char *line = deferred.numeric_text;
    while (line[-1] != '\n') {
      --line;
    }
    const char *channel_text = line + 11;
    const ParsedExistingChannelLookup parsed_channel =
        parse_existing_channel_lookup<Suffix8Lookup, Aes2Lookup>(
            channel_text);
    const ParsedExistingRow row{
        parsed_channel.lookup,
        static_cast<std::uint64_t>(month_slot_2027(line)),
        _mm_setzero_si128()};
    MonthSlot &slot =
        *resolve_existing_row(result.channel_table, storage, row).slot;

    // hot loopは65535で加算済み。差分と完全なmin/maxをcoldに戻す。
    slot.stats.length_and_stamps +=
        deferred.message_length -
        std::numeric_limits<std::uint16_t>::max();
    add_wide_message_length(result, slot, deferred.message_length);
  }
  result.deferred_wide_lengths.clear();
}

[[gnu::cold, gnu::noinline]] static void
apply_deferred_wide_lengths_generic(PartialResult &result) noexcept {
  for (const DeferredWideLength &deferred : result.deferred_wide_lengths) {
    const char *line = deferred.numeric_text;
    while (line[-1] != '\n') {
      --line;
    }
    const char *channel_text = line + 11;
    const ParsedChannel channel = parse_channel(channel_text);
    const ChannelLookup lookup =
        channel_lookup(channel.hash, channel.length & 15U);
    const std::uint32_t first_slot = result.channel_table.get(
        channel.text, channel.length, channel.hash, lookup);
    MonthSlot &slot = result.channel_table.month_slots()[
        first_slot + month_slot_2027(line)];
    slot.stats.length_and_stamps +=
        deferred.message_length -
        std::numeric_limits<std::uint16_t>::max();
    add_wide_message_length(result, slot, deferred.message_length);
  }
  result.deferred_wide_lengths.clear();
}

// parse直後にchannel tableをresolveし、Resolved rowを2件queueする。
// MonthSlotのprefetchwを実際の更新より約2 iteration先行させる。
template <bool Suffix8Lookup, bool Aes2Lookup = false,
          bool PairUnroll = false>
__attribute__((noinline, hot)) static void
aggregate_existing_rows(PartialResult &result, const char *cursor,
                        const char *end, HotStorage *storage) {
  if (cursor == end) {
    return;
  }

  ResolvedExistingRow current = resolve_existing_row(
      result.channel_table, storage,
      parse_existing_row<Suffix8Lookup, Aes2Lookup>(storage, cursor));
  if (cursor == end) [[unlikely]] {
    add_resolved_row(result, current);
    return;
  }

  ResolvedExistingRow pending = resolve_existing_row(
      result.channel_table, storage,
      parse_existing_row<Suffix8Lookup, Aes2Lookup>(storage, cursor));
  if constexpr (!PairUnroll) {
    while (cursor != end) {
      const ResolvedExistingRow lookahead = resolve_existing_row(
          result.channel_table, storage,
          parse_existing_row<Suffix8Lookup, Aes2Lookup>(storage, cursor));
      add_resolved_row(result, current);
      current = pending;
      pending = lookahead;
    }

    add_resolved_row(result, current);
    add_resolved_row(result, pending);
  } else {
    if (cursor == end) {
      add_resolved_row(result, current);
      add_resolved_row(result, pending);
      return;
    }
    for (;;) {
      const ResolvedExistingRow lookahead_a = resolve_existing_row(
          result.channel_table, storage,
          parse_existing_row<Suffix8Lookup, Aes2Lookup>(storage, cursor));
      add_resolved_row(result, current);
      current = pending;
      pending = lookahead_a;
      const bool ended_after_a = cursor == end;

      // 内部range境界では次rangeの先頭を1行だけ投機parseし、集計しない。
      const ResolvedExistingRow lookahead_b = resolve_existing_row(
          result.channel_table, storage,
          parse_existing_row<Suffix8Lookup, Aes2Lookup>(storage, cursor));
      add_resolved_row(result, current);
      current = pending;
      pending = lookahead_b;

      if (cursor >= end) {
        // 投機行がwide lengthならcold補正queueへの副作用も取り消す。
        if (ended_after_a &&
            !result.deferred_wide_lengths.empty() &&
            result.deferred_wide_lengths.back().numeric_text >= end) {
          result.deferred_wide_lengths.pop_back();
        }
        add_resolved_row(result, current);
        if (!ended_after_a) {
          add_resolved_row(result, pending);
        }
        return;
      }
    }
  }
}

static inline void
aggregate_message(PartialResult &result, const ParsedChannel &channel,
                  ChannelLookup channel_lookup, const char *timestamp_text,
                  std::uint32_t message_length, std::uint32_t stamp_count) {
  const std::uint32_t first_slot = result.channel_table.get(
      channel.text, channel.length, channel.hash, channel_lookup);
  const std::uint32_t month_slot = month_slot_2027(timestamp_text);
  MonthSlot &slot =
      result.channel_table.month_slots()[first_slot + month_slot];
  add_message(result, slot,
              counter_vector(
                  static_cast<std::uint64_t>(message_length) |
                  (static_cast<std::uint64_t>(stamp_count) << 32)));
}

template <bool PairUnroll>
static void aggregate_rows(PartialResult &result, const char *begin,
                           const char *end) {
  const char *cursor = begin;
  active_deferred_wide_lengths = &result.deferred_wide_lengths;

  while (cursor < end && !result.channel_table.can_use_hash_only_lookup()) {
    const char *const timestamp_text = cursor;
    cursor += 11;
    const ParsedChannel channel = parse_channel(cursor);
    const ChannelLookup lookup =
        result.channel_table.prefetch(channel.hash, channel.length & 15U);
    const std::uint64_t length_and_stamps =
        parse_length_and_stamps(cursor);
    aggregate_message(result, channel, lookup, timestamp_text,
                      static_cast<std::uint32_t>(length_and_stamps),
                      static_cast<std::uint32_t>(length_and_stamps >> 32));
  }

  HotStorage *const storage = result.channel_table.hot_storage();
  if (result.channel_table.hash_only_lookup_mode() ==
      HashOnlyLookupMode::suffix8) {
    aggregate_existing_rows<true, false, PairUnroll>(result, cursor, end,
                                                     storage);
  } else if (result.channel_table.hash_only_lookup_mode() ==
             HashOnlyLookupMode::aes2_folded) {
    aggregate_existing_rows<false, true, PairUnroll>(result, cursor, end,
                                                     storage);
  } else {
    aggregate_existing_rows<false, false, PairUnroll>(result, cursor, end,
                                                      storage);
  }
  active_deferred_wide_lengths = nullptr;

  if (!result.deferred_wide_lengths.empty()) [[unlikely]] {
    const HashOnlyLookupMode mode =
        result.channel_table.hash_only_lookup_mode();
    if (mode == HashOnlyLookupMode::suffix8) {
      apply_deferred_wide_lengths<true>(result, storage);
    } else if (mode == HashOnlyLookupMode::aes2_folded) {
      apply_deferred_wide_lengths<false, true>(result, storage);
    } else if (mode == HashOnlyLookupMode::folded) {
      apply_deferred_wide_lengths<false>(result, storage);
    } else {
      apply_deferred_wide_lengths_generic(result);
    }
  }
}

static std::unique_ptr<PartialResult>
aggregate_range(const char *begin, const char *end, bool pair_unroll) {
  auto result = std::make_unique<PartialResult>();
  if (pair_unroll) {
    aggregate_rows<true>(*result, begin, end);
  } else {
    aggregate_rows<false>(*result, begin, end);
  }
  return result;
}

// -----------------------------------------------------------------------------
// mmap、並列実行、スレッド間マージ
// -----------------------------------------------------------------------------

static std::int64_t next_line_start(int input_fd, std::int64_t offset,
                                    std::int64_t file_size) {
  std::array<char, 4096> buffer;
  while (offset < file_size) {
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::int64_t>(buffer.size(), file_size - offset));
    ssize_t bytes_read;
    do {
      bytes_read = ::pread(input_fd, buffer.data(), wanted, offset);
    } while (bytes_read < 0 && errno == EINTR);
    if (bytes_read <= 0) {
      return file_size;
    }
    const void *newline = std::memchr(buffer.data(), '\n', bytes_read);
    if (newline != nullptr) {
      return offset + (static_cast<const char *>(newline) - buffer.data()) + 1;
    }
    offset += bytes_read;
  }
  return file_size;
}

// 入力ファイルのopen/mmap/解放をまとめ、mainのエラー処理を単純にする。
class MappedInput {
public:
  explicit MappedInput(const char *path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
      return;
    }

    struct stat file_info {};
    if (::fstat(fd_, &file_info) != 0 || file_info.st_size == 0) {
      return;
    }
    file_size_ = file_info.st_size;
    rows_begin_ = next_line_start(fd_, 0, file_size_);
    if (rows_begin_ == file_size_) {
      valid_ = true;
      return;
    }

    // 先頭から末尾まで1回だけ走査する。OSへreadahead方針を伝え、
    // mmapのpage fault待ちを減らす（失敗しても正しさには影響しない）。
#if !defined(__APPLE__)
    (void)::posix_fadvise(fd_, rows_begin_, file_size_ - rows_begin_,
                         POSIX_FADV_SEQUENTIAL);
#endif

    // file mappingの直後にzero-filled guard pageを予約する。これにより
    // 16-byte channel scannerは、file sizeがpage境界ちょうどでも安全に先読みできる。
    const long page_size_long = ::sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
      return;
    }
    const std::size_t page_size = static_cast<std::size_t>(page_size_long);
    const std::size_t file_bytes = static_cast<std::size_t>(file_size_);
    const std::size_t rounded_file_bytes =
        (file_bytes + page_size - 1) & ~(page_size - 1);
    mapping_size_ = rounded_file_bytes + page_size;
    void *const reservation =
        ::mmap(nullptr, mapping_size_, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
      mapping_size_ = 0;
      return;
    }
    void *const file_mapping =
        ::mmap(reservation, file_bytes, PROT_READ,
               MAP_PRIVATE | MAP_FIXED, fd_, 0);
    if (file_mapping == MAP_FAILED) {
      ::munmap(reservation, mapping_size_);
      mapping_size_ = 0;
      mapped_ = nullptr;
      return;
    }
    mapped_ = static_cast<const char *>(file_mapping);
    (void)::madvise(const_cast<char *>(mapped_),
                    static_cast<std::size_t>(file_size_), MADV_SEQUENTIAL);
    valid_ = true;
  }

  MappedInput(const MappedInput &) = delete;
  MappedInput &operator=(const MappedInput &) = delete;

  ~MappedInput() { release(); }

  bool valid() const noexcept { return valid_; }
  bool has_rows() const noexcept { return rows_begin_ != file_size_; }
  int fd() const noexcept { return fd_; }
  std::int64_t file_size() const noexcept { return file_size_; }
  std::int64_t rows_begin() const noexcept { return rows_begin_; }
  const char *data() const noexcept { return mapped_; }

  void release() noexcept {
    if (mapped_ != nullptr) {
      ::munmap(const_cast<char *>(mapped_), mapping_size_);
      mapped_ = nullptr;
      mapping_size_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
  std::int64_t file_size_ = 0;
  std::int64_t rows_begin_ = 0;
  const char *mapped_ = nullptr;
  std::size_t mapping_size_ = 0;
  bool valid_ = false;
};

using RangeBoundaries = std::array<std::int64_t, kThreadCount + 1>;
using PartialResults = std::array<std::unique_ptr<PartialResult>, kThreadCount>;

static std::int64_t next_mapped_line_start(const char *mapped,
                                           std::int64_t offset,
                                           std::int64_t file_size) noexcept {
  const void *const newline = std::memchr(
      mapped + offset, '\n', static_cast<std::size_t>(file_size - offset));
  return newline == nullptr
             ? file_size
             : static_cast<const char *>(newline) - mapped + 1;
}

// バイト数でほぼ均等に8分割し、各境界を次の行頭まで進める。
static RangeBoundaries split_input_ranges(const char *mapped,
                                          std::int64_t rows_begin,
                                          std::int64_t file_size) {
  const std::int64_t data_size = file_size - rows_begin;
  RangeBoundaries boundaries;
  boundaries.front() = rows_begin;
  boundaries.back() = file_size;
  for (unsigned index = 1; index < kThreadCount; ++index) {
    const std::int64_t approximate =
        rows_begin + data_size * index / kThreadCount;
    boundaries[index] =
        next_mapped_line_start(mapped, approximate, file_size);
  }
  return boundaries;
}

// 呼び出し元threadもrange 0を処理し、追加生成は7本に抑える。
static PartialResults
aggregate_ranges_in_parallel(const char *mapped,
                             const RangeBoundaries &boundaries) {
  PartialResults results;
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount - 1);
  for (unsigned index = 1; index < kThreadCount; ++index) {
    workers.emplace_back([&, index] {
      results[index] = aggregate_range(mapped + boundaries[index],
                                       mapped + boundaries[index + 1],
                                       index + 1 < kThreadCount);
    });
  }
  results[0] = aggregate_range(mapped + boundaries[0],
                               mapped + boundaries[1], true);
  for (std::thread &worker : workers) {
    worker.join();
  }
  return results;
}

struct FinalChannel {
  explicit FinalChannel(const ChannelRef &channel) noexcept
      : text(channel.text), hash(channel.hash), length(channel.length),
        months{} {}

  const char *text = nullptr;
  std::uint64_t hash = 0;
  std::uint32_t length = 0;
  std::array<Stats, kMonthsPerYear> months;
};

struct FinalChannelBucket {
  std::uint64_t hash = 0;
  std::uint32_t length = 0;
  std::uint32_t id = 0;
};
static_assert(sizeof(FinalChannelBucket) == 16);

class FinalChannelTable {
public:
  FinalChannelTable() : buckets_(kBucketCount) {
    channels_.reserve(kMaxChannels);
  }

  FinalChannel &get(const ChannelRef &channel) {
    FinalChannelBucket *bucket = find_slot(channel);
    if (bucket->id != 0) {
      return channels_[bucket->id - 1];
    }
    bucket->hash = channel.hash;
    bucket->length = channel.length;
    bucket->id = channel_count_ + 1;
    FinalChannel &inserted = channels_.emplace_back(channel);
    ++channel_count_;
    return inserted;
  }

  const std::vector<FinalChannel> &channels() const noexcept {
    return channels_;
  }

private:
  static constexpr std::size_t kBucketCount = 32768;
  static constexpr std::size_t kBucketMask = kBucketCount - 1;
  static_assert(std::has_single_bit(kBucketCount));
  static_assert(kMaxChannels < kBucketCount);

  FinalChannelBucket *find_slot(const ChannelRef &channel) noexcept {
    std::size_t index = channel.hash & kBucketMask;
    for (;;) {
      FinalChannelBucket &bucket = buckets_[index];
      if (bucket.id != 0 && bucket.hash == channel.hash &&
          bucket.length == channel.length &&
          equal_channel(channels_[bucket.id - 1].text, channel.text,
                        channel.length)) {
        return &bucket;
      }
      if (bucket.id == 0) {
        return &bucket;
      }
      index = (index + 1) & kBucketMask;
    }
  }

  std::vector<FinalChannelBucket> buckets_;
  std::vector<FinalChannel> channels_;
  std::uint32_t channel_count_ = 0;
};

static inline void merge_compact(
    Stats &target, const CompactStats &source,
    const WideLengthRange *wide_lengths) noexcept {
  const bool has_wide_length =
      wide_lengths != nullptr && wide_lengths->max_length != 0;
  const bool has_small_length =
      has_wide_length
          ? source.message_count() != wide_lengths->message_count
          : source.inverted_max_length !=
                std::numeric_limits<std::uint16_t>::max();
  // small値はwide値より必ず小さいので、min/maxの候補は片側だけ。
  const std::uint32_t source_min =
      has_small_length ? source.min_length : wide_lengths->min_length;
  const std::uint32_t source_max =
      has_wide_length
          ? wide_lengths->max_length
          : static_cast<std::uint16_t>(~source.inverted_max_length);
  target.min_length = std::min(target.min_length, source_min);
  target.max_length = std::max(target.max_length, source_max);
  target.length_and_count +=
      static_cast<std::uint64_t>(source.total_length()) |
      (static_cast<std::uint64_t>(source.message_count()) << 32);
  target.total_stamp_count += source.total_stamp_count();
}

// 各threadのチャンネルを一度だけglobal ID化し、12か月を密にmergeする。
static FinalChannelTable merge_partial_results(const PartialResults &partial) {
  FinalChannelTable totals;
  for (const auto &part : partial) {
    const auto &channels = part->channel_table.channels();
    for (std::size_t channel_id = 0;
         channel_id < part->channel_table.channel_count();
         ++channel_id) {
      const ChannelRef &channel = channels[channel_id];
      FinalChannel &target = totals.get(channel);
      const MonthSlot *slots =
          part->channel_table.month_slots() + channel_id * kMonthsPerYear;
      const WideLengthRange *wide_lengths =
          part->wide_lengths
              ? part->wide_lengths.get() + channel_id * kMonthsPerYear
              : nullptr;
      for (unsigned slot_index = 0; slot_index < kMonthsPerYear; ++slot_index) {
        const CompactStats &compact = slots[slot_index].stats;
        if (compact.length_and_stamps == 0) {
          continue;
        }
        merge_compact(target.months[slot_index], compact,
                      wide_lengths ? wide_lengths + slot_index : nullptr);
      }
    }
  }
  return totals;
}

// -----------------------------------------------------------------------------
// 出力整形
// -----------------------------------------------------------------------------

static void append_integer(std::string &output, std::int64_t value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  output.append(buffer, result.ptr);
}

static void append_year_month(std::string &output, std::int32_t month_key) {
  const std::uint32_t month =
      static_cast<std::uint32_t>(month_key - kFirst2027MonthKey + 1);
  output.append("2027-");
  output.push_back(static_cast<char>('0' + month / 10));
  output.push_back(static_cast<char>('0' + month % 10));
}

static void append_average(std::string &output, const Stats &stats) {
    const std::uint32_t total_length = stats.total_length();
    const std::uint32_t message_count = stats.message_count();
    char buffer[64];
    const double average = static_cast<double>(total_length) /
                           static_cast<double>(message_count);
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), average,
                                      std::chars_format::fixed, 2);
    output.append(buffer, result.ptr);
    return;
}

static void append_output_line(std::string &output, const FinalChannel &channel,
                               std::uint32_t month_slot, const Stats &stats) {
  output.append(channel.text, channel.length);
  output.push_back(',');
  append_year_month(output, kFirst2027MonthKey +
                                static_cast<std::int32_t>(month_slot));
  output.push_back('=');
  append_integer(output, stats.min_length);
  output.push_back('/');
  append_average(output, stats);
  output.push_back('/');
  append_integer(output, stats.max_length);
  output.push_back('/');
  append_integer(output, stats.message_count());
  output.push_back('/');
  append_integer(output, stats.total_stamp_count);
  output.push_back('\n');
}

static std::string build_output(const FinalChannelTable &totals) {
  std::string output;
  output.reserve(totals.channels().size() * kMonthsPerYear * 56);
  for (const FinalChannel &channel : totals.channels()) {
    for (std::uint32_t month_slot = 0; month_slot < kMonthsPerYear;
         ++month_slot) {
      const Stats &stats = channel.months[month_slot];
      if (stats.length_and_count != 0) {
        append_output_line(output, channel, month_slot, stats);
      }
    }
  }
  return output;
}

static bool write_output(const char *path, std::string_view output) {
  const int output_fd =
      ::open(path, O_WRONLY | O_CREAT | O_TRUNC, static_cast<mode_t>(0666));
  if (output_fd < 0) {
    return false;
  }

  const char *cursor = output.data();
  std::size_t remaining = output.size();
  while (remaining != 0) {
    ssize_t written;
    do {
      written = ::write(output_fd, cursor, remaining);
    } while (written < 0 && errno == EINTR);
    if (written <= 0) {
      ::close(output_fd);
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return ::close(output_fd) == 0;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    return 1;
  }

  MappedInput input(argv[1]);
  if (!input.valid()) {
    return 1;
  }
  if (!input.has_rows()) {
    return write_output(argv[2], {}) ? 0 : 1;
  }

  const RangeBoundaries boundaries =
      split_input_ranges(input.data(), input.rows_begin(), input.file_size());
  const PartialResults partial =
      aggregate_ranges_in_parallel(input.data(), boundaries);

  const FinalChannelTable totals = merge_partial_results(partial);
  const std::string output = build_output(totals);
  const int status = write_output(argv[2], output) ? 0 : 1;
  // 出力はwrite/close済みなので、終了時の大きな個別解放を省く。
  std::_Exit(status);
}
