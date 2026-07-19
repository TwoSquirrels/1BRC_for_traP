#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <immintrin.h>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kSlotCount = 65536;
constexpr std::size_t kMaxChannels = 10000;
constexpr std::size_t kDirectSlotCount = std::size_t{1} << 20;
constexpr uint32_t kSharedBatchSize = 1280;

[[noreturn]] void fail(const char* message) {
#if defined(_WIN32)
    std::fprintf(stderr, "%s\n", message);
#else
    std::perror(message);
#endif
    std::exit(1);
}

struct MappedFile {
    const char* data = nullptr;
    std::size_t size = 0;
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int fd = -1;
#endif

    explicit MappedFile(const char* path) {
#if defined(_WIN32)
        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) fail("CreateFileA input");
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz)) fail("GetFileSizeEx input");
        size = static_cast<std::size_t>(sz.QuadPart);
        if (size == 0) return;
        mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) fail("CreateFileMappingA input");
        data = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!data) fail("MapViewOfFile input");
#else
        fd = open(path, O_RDONLY);
        if (fd < 0) fail("open input");
        struct stat st {};
        if (fstat(fd, &st) != 0) fail("fstat input");
        size = static_cast<std::size_t>(st.st_size);
        if (size == 0) return;
        void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) fail("mmap input");
        data = static_cast<const char*>(mapped);
#endif
    }

    ~MappedFile() {
#if defined(_WIN32)
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#else
        if (data) munmap(const_cast<char*>(data), size);
        if (fd >= 0) close(fd);
#endif
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

void write_all(const char* path, const char* data, std::size_t size) {
#if defined(_WIN32)
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) fail("CreateFileA output");
    std::size_t written_total = 0;
    while (written_total < size) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - written_total, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, data + written_total, chunk, &written, nullptr)) {
            CloseHandle(file);
            fail("WriteFile output");
        }
        written_total += written;
    }
    if (!CloseHandle(file)) fail("CloseHandle output");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) fail("open output");
    std::size_t written_total = 0;
    while (written_total < size) {
        ssize_t written = write(fd, data + written_total, size - written_total);
        if (written < 0) {
            if (errno == EINTR) continue;
            close(fd);
            fail("write output");
        }
        written_total += static_cast<std::size_t>(written);
    }
    if (close(fd) != 0) fail("close output");
#endif
}

[[gnu::always_inline]] inline uint32_t parse_month_2027(const char* p) {
    static constexpr std::array<uint32_t, 13> starts = {
        1798761600u, 1801440000u, 1803859200u, 1806537600u, 1809129600u,
        1811808000u, 1814400000u, 1817078400u, 1819756800u, 1822348800u,
        1825027200u, 1827619200u, 1830297600u,
    };
    static constexpr auto bcd4 = [](uint32_t value) {
        return ((value / 1000u) % 10u) |
               (((value / 100u) % 10u) << 4) |
               (((value / 10u) % 10u) << 8) |
               ((value % 10u) << 12);
    };
    static constexpr auto prefix_month = [] {
        std::array<uint8_t, 65536> result{};
        for (uint32_t prefix = 1798; prefix <= 1830; ++prefix) {
            uint32_t month = 0;
            while (month < 11 &&
                   starts[month + 1] < prefix * 1000000u) {
                ++month;
            }
            result[bcd4(prefix)] = static_cast<uint8_t>(month);
        }
        return result;
    }();
    static constexpr auto ascii8 = [](uint32_t value) {
        uint64_t result = 0;
        uint32_t divisor = 10000000u;
        for (uint32_t i = 0; i < 8; ++i) {
            result = (result << 8) | ('0' + (value / divisor) % 10u);
            divisor /= 10u;
        }
        return result;
    };
    static constexpr auto start_prefix = [] {
        std::array<uint64_t, 13> result{};
        for (uint32_t i = 0; i < result.size(); ++i) {
            result[i] = ascii8(starts[i] / 100u);
        }
        return result;
    }();

    uint64_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    const uint32_t month = prefix_month[
        _pext_u64(raw, 0x0f0f0f0full)];
    const uint64_t ordered = __builtin_bswap64(raw);
    return month + (ordered >= start_prefix[month + 1]);
}

struct MonthStat {
    uint32_t sum = 0;
    uint32_t count = 0;
    uint32_t stamp_sum = 0;
    uint32_t min_len = ~uint32_t{0};
    uint32_t max_len = 0;
};

static_assert(sizeof(MonthStat) == 20);

[[gnu::always_inline]] inline void add_sum_and_count(
    MonthStat& stat, uint32_t message_len) {
    stat.sum += message_len;
    ++stat.count;
}

struct ChannelKey {
    const char* key = nullptr;
    std::uint64_t hash = 0;
    uint32_t key_len = 0;
};

struct ChannelStats {
    MonthStat months[12];

    [[gnu::always_inline]] MonthStat& month_at(uint32_t month) {
        return months[month];
    }

    [[gnu::always_inline]] const MonthStat& month_at(uint32_t month) const {
        return months[month];
    }
};

static_assert(sizeof(ChannelStats) == 240);

struct ChannelSlot {
    std::uint64_t tag0 = 0;
    std::uint64_t tag1 = 0;
    uint32_t metadata = 0;

    [[gnu::always_inline]] uint32_t key_len() const {
        return metadata & 0xffu;
    }

    [[gnu::always_inline]] uint32_t channel_id_plus_one() const {
        return metadata >> 8;
    }

    [[gnu::always_inline]] void set_metadata(
        uint32_t length, uint32_t id_plus_one) {
        metadata = length | (id_plus_one << 8);
    }
} __attribute__((packed));

static_assert(sizeof(ChannelSlot) == 20);

struct ShortKeyWords {
    uint64_t first;
    uint64_t second;
};

[[gnu::always_inline]] inline ShortKeyWords load_short_key_words(
    const char* key, uint32_t key_len) {
    uint64_t first = 0;
    uint64_t second = 0;
    if (key_len <= 8) {
        std::memcpy(&first, key, key_len);
    } else {
        std::memcpy(&first, key, 8);
        std::memcpy(&second, key + 8, std::min<uint32_t>(key_len - 8, 8));
    }
    return {first, second};
}

[[gnu::always_inline]] inline bool keys_equal(
    const char* left, const char* right, uint32_t length) {
    while (length >= 8) {
        uint64_t a;
        uint64_t b;
        std::memcpy(&a, left, sizeof(a));
        std::memcpy(&b, right, sizeof(b));
        if (a != b) return false;
        left += 8;
        right += 8;
        length -= 8;
    }
    if (length >= 3) {
        uint64_t a;
        uint64_t b;
        std::memcpy(&a, left, sizeof(a));
        std::memcpy(&b, right, sizeof(b));
        const uint64_t mask = ~uint64_t{0} >> ((8 - length) * 8);
        return ((a ^ b) & mask) == 0;
    }
    if (length == 2) {
        uint16_t a;
        uint16_t b;
        std::memcpy(&a, left, sizeof(a));
        std::memcpy(&b, right, sizeof(b));
        return a == b;
    }
    return length == 0 || *left == *right;
}

class ChannelMap {
public:
    std::vector<ChannelSlot> slots;
    std::vector<ChannelStats> channels;
    std::vector<ChannelKey> channel_keys;
    std::vector<uint16_t> direct_ids;
    std::vector<uint64_t> direct_collision_ids;
    bool direct_hashes_unique = true;

    ChannelMap() {
        slots.assign(kSlotCount, ChannelSlot{});
        channels.reserve(kMaxChannels);
        channel_keys.reserve(kMaxChannels);
    }

    [[gnu::always_inline]] inline void add(
        const char* key, uint32_t key_len, uint32_t month,
        std::uint64_t hash, uint32_t message_len, uint32_t stamps) {
        const ShortKeyWords words = load_short_key_words(key, key_len);
        ChannelSlot* slot = find_or_empty(key, key_len, hash, words.first, words.second);
        const uint32_t channel_id =
            __builtin_expect(slot->metadata == 0, 0)
                ? insert_channel(slot, key, key_len, hash, words.first, words.second)
                : slot->channel_id_plus_one() - 1;

        ChannelStats& channel = channels[channel_id];
        MonthStat& stat = channel.month_at(month);
        add_sum_and_count(stat, message_len);
        stat.stamp_sum += stamps;
        stat.min_len = std::min(stat.min_len, message_len);
        stat.max_len = std::max(stat.max_len, message_len);
    }

    [[gnu::always_inline]] inline void prefetch_slot(std::uint64_t hash) const {
        const std::size_t pos = static_cast<std::size_t>(hash) & (kSlotCount - 1);
        _mm_prefetch(reinterpret_cast<const char*>(&slots[pos]), _MM_HINT_T0);
    }

    void build_direct_lookup() {
        direct_ids.assign(kDirectSlotCount, 0);
        for (uint32_t channel_id = 0;
             channel_id < channel_keys.size(); ++channel_id) {
            uint16_t& entry = direct_ids[
                static_cast<std::size_t>(channel_keys[channel_id].hash) &
                (kDirectSlotCount - 1)];
            if (entry == 0) {
                entry = static_cast<uint16_t>(channel_id + 1);
            } else {
                entry = 0xffffu;
            }
        }

        constexpr std::size_t collision_slots = 32768;
        direct_collision_ids.assign(collision_slots, 0);
        direct_hashes_unique = true;
        for (uint32_t channel_id = 0;
             channel_id < channel_keys.size(); ++channel_id) {
            const uint32_t hash = static_cast<uint32_t>(
                channel_keys[channel_id].hash);
            if (direct_ids[hash & (kDirectSlotCount - 1)] != 0xffffu) {
                continue;
            }
            std::size_t pos = (hash * 0x9e3779b1u) & (collision_slots - 1);
            while (direct_collision_ids[pos] != 0) {
                if (static_cast<uint32_t>(direct_collision_ids[pos]) == hash) {
                    direct_hashes_unique = false;
                    break;
                }
                pos = (pos + 1) & (collision_slots - 1);
            }
            if (direct_collision_ids[pos] == 0) {
                direct_collision_ids[pos] =
                    (uint64_t{channel_id + 1} << 32) | hash;
            }
        }
    }

    [[gnu::always_inline]] inline void prefetch_direct(
        std::uint64_t hash) const {
        const std::size_t pos = static_cast<std::size_t>(hash) &
                                (kDirectSlotCount - 1);
        _mm_prefetch(
            reinterpret_cast<const char*>(&direct_ids[pos]), _MM_HINT_T0);
    }

    [[gnu::always_inline]] inline uint32_t resolve_channel(
        const char* const* key_location, uint32_t key_len, std::uint64_t hash,
        uint64_t key0, uint64_t key1) {
        const char* key = key_len > 16 ? *key_location : nullptr;
        ChannelSlot* slot = find_or_empty(key, key_len, hash, key0, key1);
        return __builtin_expect(slot->metadata == 0, 0)
                   ? insert_channel(
                         slot, key ? key : *key_location,
                         key_len, hash, key0, key1)
                   : slot->channel_id_plus_one() - 1;
    }

    [[gnu::always_inline]] inline uint32_t resolve_existing(
        const char* const* key_location, uint32_t key_len, std::uint64_t hash,
        uint64_t key0, uint64_t key1) const {
        const char* key = key_len > 16 ? *key_location : nullptr;
        std::size_t pos = static_cast<std::size_t>(hash) & (kSlotCount - 1);
        while (true) {
            const ChannelSlot& slot = slots[pos];
            if (slot.key_len() == key_len) {
                if (key_len <= 16) {
                    if (slot.tag0 == key0 && slot.tag1 == key1) {
                        return slot.channel_id_plus_one() - 1;
                    }
                } else if (slot.tag0 == hash &&
                           keys_equal(reinterpret_cast<const char*>(slot.tag1),
                                      key, key_len)) {
                    return slot.channel_id_plus_one() - 1;
                }
            }
            pos = (pos + 1) & (kSlotCount - 1);
        }
    }

    [[gnu::always_inline]] inline uint32_t resolve_direct_hash(
        uint32_t hash) const {
        const uint16_t entry = direct_ids[
            static_cast<std::size_t>(hash) & (kDirectSlotCount - 1)];
        if (__builtin_expect(entry != 0xffffu, 1)) {
            return static_cast<uint32_t>(entry - 1);
        }
        const std::size_t mask = direct_collision_ids.size() - 1;
        std::size_t pos = (hash * 0x9e3779b1u) & mask;
        while (static_cast<uint32_t>(direct_collision_ids[pos]) != hash) {
            pos = (pos + 1) & mask;
        }
        return static_cast<uint32_t>(direct_collision_ids[pos] >> 32) - 1;
    }

    [[gnu::always_inline]] inline uint32_t intern_channel(
        const char* key, uint32_t key_len, std::uint64_t hash,
        uint64_t key0, uint64_t key1) {
        ChannelSlot* slot = find_or_empty(key, key_len, hash, key0, key1);
        if (slot->metadata == 0) {
            return insert_channel(slot, key, key_len, hash, key0, key1);
        }
        return slot->channel_id_plus_one() - 1;
    }

    [[gnu::always_inline]] inline bool is_full() const {
        return channels.size() == kMaxChannels;
    }

    [[gnu::always_inline]] inline void prefetch_stat(
        uint32_t channel_id, uint32_t month) const {
        _mm_prefetch(
            reinterpret_cast<const char*>(&channels[channel_id].month_at(month)),
            _MM_HINT_T0);
    }

    [[gnu::always_inline]] inline void update_stat(
        uint32_t channel_id, uint32_t month,
        uint32_t message_len, uint32_t stamps) {
        ChannelStats& channel = channels[channel_id];
        MonthStat& stat = channel.month_at(month);
        add_sum_and_count(stat, message_len);
        stat.stamp_sum += stamps;
        stat.min_len = std::min(stat.min_len, message_len);
        stat.max_len = std::max(stat.max_len, message_len);
    }

    void merge_channel(const ChannelKey& source_key, const ChannelStats& source) {
        const ShortKeyWords words = load_short_key_words(
            source_key.key, source_key.key_len);
        ChannelSlot* slot = find_or_empty(
            source_key.key, source_key.key_len, source_key.hash,
            words.first, words.second);
        if (slot->metadata == 0) {
            slot->tag0 = source_key.key_len <= 16 ? words.first : source_key.hash;
            slot->tag1 = source_key.key_len <= 16
                             ? words.second
                             : reinterpret_cast<uintptr_t>(source_key.key);
            slot->set_metadata(
                source_key.key_len, static_cast<uint32_t>(channels.size()) + 1);

            channels.emplace_back();
            channel_keys.push_back(source_key);
        }

        ChannelStats& target = channels[slot->channel_id_plus_one() - 1];
        for (uint32_t month = 0; month < 12; ++month) {
            const MonthStat& src = source.month_at(month);
            MonthStat& dst = target.month_at(month);
            dst.sum += src.sum;
            dst.count += src.count;
            dst.stamp_sum += src.stamp_sum;
            dst.min_len = std::min(dst.min_len, src.min_len);
            dst.max_len = std::max(dst.max_len, src.max_len);
        }
    }

private:
    [[gnu::cold, gnu::noinline]] uint32_t insert_channel(
        ChannelSlot* slot, const char* key, uint32_t key_len, std::uint64_t hash,
        uint64_t key0, uint64_t key1) {
        slot->tag0 = key_len <= 16 ? key0 : hash;
        slot->tag1 = key_len <= 16 ? key1 : reinterpret_cast<uintptr_t>(key);
        slot->set_metadata(key_len, static_cast<uint32_t>(channels.size()) + 1);

        channels.emplace_back();
        channel_keys.push_back({key, hash, key_len});
        return slot->channel_id_plus_one() - 1;
    }

    [[gnu::always_inline]] inline ChannelSlot* find_or_empty(
        const char* key, uint32_t key_len, std::uint64_t hash,
        uint64_t key0, uint64_t key1) {
        std::size_t pos = static_cast<std::size_t>(hash) & (kSlotCount - 1);
        while (true) {
            ChannelSlot& slot = slots[pos];
            if (slot.metadata == 0) return &slot;
            if (slot.key_len() == key_len) {
                if (key_len <= 16) {
                    if (slot.tag0 == key0 && slot.tag1 == key1) return &slot;
                } else if (slot.tag0 == hash &&
                           keys_equal(reinterpret_cast<const char*>(slot.tag1),
                                      key, key_len)) {
                    return &slot;
                }
            }
            pos = (pos + 1) & (kSlotCount - 1);
        }
    }
};

struct WorkerResult {
    ChannelMap map;
    std::uint64_t rows = 0;
};

[[gnu::always_inline]] inline uint32_t hash_channel_crc(
    const char* p, uint32_t length) {
    uint32_t hash = 0x9e3779b9u;
    uint32_t remaining = length;
    while (remaining >= 8) {
        uint64_t word;
        std::memcpy(&word, p, sizeof(word));
        hash = static_cast<uint32_t>(_mm_crc32_u64(hash, word));
        p += 8;
        remaining -= 8;
    }
    if (remaining >= 4) {
        uint32_t word;
        std::memcpy(&word, p, sizeof(word));
        hash = _mm_crc32_u32(static_cast<uint32_t>(hash), word);
        p += 4;
        remaining -= 4;
    }
    if (remaining >= 2) {
        uint16_t word;
        std::memcpy(&word, p, sizeof(word));
        hash = _mm_crc32_u16(static_cast<uint32_t>(hash), word);
        p += 2;
        remaining -= 2;
    }
    if (remaining != 0) {
        hash = _mm_crc32_u8(static_cast<uint32_t>(hash),
                           static_cast<unsigned char>(*p));
    }
    hash ^= length * 0x9e3779b1u;
    hash ^= hash >> 16;
    return hash ? hash : 1;
}

struct ParsedChannel {
    const char* comma;
    uint64_t hash;
    uint64_t key0;
    uint64_t key1;
    uint32_t length;
};

[[gnu::always_inline]] inline uint32_t hash_channel_words(
    uint64_t w0, uint64_t w1, uint64_t w2, uint64_t w3, uint32_t length) {
    if (length <= 8) {
        w0 = _bzhi_u64(w0, length * 8);
        return static_cast<uint32_t>(_mm_crc32_u64(
            0x9e3779b9u ^ length, w0));
    }

    if (length <= 16) {
        w1 = _bzhi_u64(w1, (length - 8) * 8);
        const uint64_t folded = w0 ^ std::rotl(w1, 29);
        return static_cast<uint32_t>(_mm_crc32_u64(
            0x85ebca6bu ^ length, folded));
    }

    if (length <= 24) {
        w2 = _bzhi_u64(w2, (length - 16) * 8);
        const uint64_t folded = w0 ^ std::rotl(w1, 29) ^ std::rotl(w2, 47);
        return static_cast<uint32_t>(_mm_crc32_u64(
            0x85ebca6bu ^ length, folded));
    }

    w3 = _bzhi_u64(w3, (length - 24) * 8);
    const uint64_t folded = w0 ^ std::rotl(w1, 29) ^ std::rotl(w2, 47) ^
                            std::rotl(w3, 13);
    return static_cast<uint32_t>(_mm_crc32_u64(
        0x85ebca6bu ^ length, folded));
}

[[gnu::always_inline]] inline ParsedChannel parse_channel(
    const char* p, const char* end) {
    if (__builtin_expect(end - p >= 16, 1)) {
        const __m128i comma = _mm_set1_epi8(',');
        const __m128i first = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        uint32_t mask = static_cast<uint32_t>(
            _mm_movemask_epi8(_mm_cmpeq_epi8(first, comma)));
        const uint64_t w0 = static_cast<uint64_t>(_mm_cvtsi128_si64(first));
        const uint64_t w1 = static_cast<uint64_t>(_mm_extract_epi64(first, 1));
        if (__builtin_expect(mask != 0, 1)) {
            const uint32_t length = static_cast<uint32_t>(__builtin_ctz(mask));
            const uint64_t key0 = _bzhi_u64(w0, std::min(length, 8u) * 8);
            const uint64_t key1 = length > 8
                                      ? _bzhi_u64(w1, (length - 8) * 8)
                                      : 0;
            return {p + length,
                    hash_channel_words(w0, w1, 0, 0, length),
                    key0, key1, length};
        }

        if (__builtin_expect(end - p >= 32, 1)) {
            const __m128i second =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
            mask = static_cast<uint32_t>(
                _mm_movemask_epi8(_mm_cmpeq_epi8(second, comma)));
            if (__builtin_expect(mask != 0, 1)) {
                const uint32_t length =
                    16u + static_cast<uint32_t>(__builtin_ctz(mask));
                const uint64_t w2 = static_cast<uint64_t>(_mm_cvtsi128_si64(second));
                const uint64_t w3 = static_cast<uint64_t>(_mm_extract_epi64(second, 1));
                return {p + length,
                        hash_channel_words(w0, w1, w2, w3, length),
                        w0, w1, length};
            }
        }
    }

    const char* comma = p;
    while (*comma != ',') ++comma;
    const uint32_t length = static_cast<uint32_t>(comma - p);
    if (length <= 32) {
        uint64_t words[4]{};
        std::memcpy(words, p, length);
        return {comma,
                hash_channel_words(
                    words[0], words[1], words[2], words[3], length),
                words[0], words[1], length};
    }
    return {comma, hash_channel_crc(p, length), 0, 0, length};
}

bool try_build_shared_dictionary(
    const char* begin, const char* end, ChannelMap& dictionary) {
    constexpr std::size_t scan_limit = 64u * 1024u * 1024u;
    const char* const limit = begin + std::min<std::size_t>(
        static_cast<std::size_t>(end - begin), scan_limit);
    const char* p = begin;
    while (p < limit && !dictionary.is_full()) {
        p += 11;
        const char* key = p;
        const ParsedChannel channel = parse_channel(p, end);
        dictionary.intern_channel(
            key, channel.length, channel.hash, channel.key0, channel.key1);
        p = channel.comma + 1;
        while (*p != '\n') ++p;
        ++p;
    }
    return dictionary.is_full();
}

struct LookupRow {
    uint64_t key0;
    uint64_t key1;
    uint32_t hash;
    uint32_t key_len_and_month;
};

struct ValueRow {
    uint32_t message_len;
    uint32_t stamps;
};

struct SharedLookupRow {
    uint32_t hash;
    uint32_t key_len_and_month;
};

static_assert(sizeof(LookupRow) == 24);
static_assert(sizeof(ValueRow) == 8);
static_assert(sizeof(SharedLookupRow) == 8);

void parse_range(const char* begin, const char* end, WorkerResult& result) {
    const char* p = begin;
    constexpr uint32_t batch_size = 1024;
    LookupRow lookup_rows[batch_size];
    const char* key_locations[batch_size];
    ValueRow value_rows[batch_size];
    uint32_t channel_ids[batch_size];

    while (p < end) {
        const bool map_is_full = result.map.is_full();
        uint32_t count = 0;
        do {
            const uint32_t month = parse_month_2027(p);
            p += 11;

            const char* key = p;
            const ParsedChannel channel = parse_channel(p, end);
            p = channel.comma + 1;

            uint32_t message_len = static_cast<uint32_t>(*p++ - '0');
            while (*p != ',') {
                message_len = message_len * 10u + static_cast<uint32_t>(*p++ - '0');
            }
            ++p;

            uint32_t stamps = static_cast<uint32_t>(*p++ - '0');
            while (*p != '\n') {
                stamps = stamps * 10u + static_cast<uint32_t>(*p++ - '0');
            }
            ++p;

            lookup_rows[count] = {
                channel.key0, channel.key1,
                static_cast<uint32_t>(channel.hash), channel.length | (month << 8),
            };
            if (!map_is_full || channel.length > 16) {
                key_locations[count] = key;
            }
            value_rows[count] = {message_len, stamps};
            result.map.prefetch_slot(channel.hash);
            ++count;
        } while (count < batch_size && p < end);

        if (__builtin_expect(map_is_full, 1)) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t metadata = lookup_rows[i].key_len_and_month;
                const uint32_t month = metadata >> 8;
                const uint32_t channel_id = result.map.resolve_existing(
                    &key_locations[i], metadata & 0xffu, lookup_rows[i].hash,
                    lookup_rows[i].key0, lookup_rows[i].key1);
                channel_ids[i] = channel_id | (month << 16);
                result.map.prefetch_stat(channel_id, month);
            }
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t metadata = lookup_rows[i].key_len_and_month;
                const uint32_t month = metadata >> 8;
                const uint32_t channel_id = result.map.resolve_channel(
                    &key_locations[i], metadata & 0xffu, lookup_rows[i].hash,
                    lookup_rows[i].key0, lookup_rows[i].key1);
                channel_ids[i] = channel_id | (month << 16);
                result.map.prefetch_stat(channel_id, month);
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t id_and_month = channel_ids[i];
            result.map.update_stat(
                id_and_month & 0xffffu, id_and_month >> 16,
                value_rows[i].message_len, value_rows[i].stamps);
        }
        result.rows += count;
    }
}

struct SharedWorkerResult {
    std::vector<ChannelStats> channels;
    std::uint64_t rows = 0;

    explicit SharedWorkerResult(std::size_t channel_count)
        : channels(channel_count) {}
};

[[gnu::always_inline]] inline void prefetch_shared_stat(
    const SharedWorkerResult& result, uint32_t channel_id, uint32_t month) {
    _mm_prefetch(
        reinterpret_cast<const char*>(&result.channels[channel_id].month_at(month)),
        _MM_HINT_T0);
}

[[gnu::always_inline]] inline void update_shared_stat(
    SharedWorkerResult& result, uint32_t channel_id, uint32_t month,
    uint32_t message_len, uint32_t stamps) {
    MonthStat& stat = result.channels[channel_id].month_at(month);
    add_sum_and_count(stat, message_len);
    stat.stamp_sum += stamps;
    stat.min_len = std::min(stat.min_len, message_len);
    stat.max_len = std::max(stat.max_len, message_len);
}

[[gnu::always_inline]] inline void parse_shared_row(
    const char*& p, const char* end, const ChannelMap& dictionary,
    SharedLookupRow& lookup, ValueRow& value) {
    const uint32_t month = parse_month_2027(p);
    p += 11;

    const char* key = p;
    const ParsedChannel channel = parse_channel(p, end);
    p = channel.comma + 1;

    uint32_t message_len = static_cast<uint32_t>(*p++ - '0');
    while (*p != ',') {
        message_len = message_len * 10u +
                      static_cast<uint32_t>(*p++ - '0');
    }
    ++p;

    uint32_t stamps = static_cast<uint32_t>(*p++ - '0');
    while (*p != '\n') {
        stamps = stamps * 10u + static_cast<uint32_t>(*p++ - '0');
    }
    ++p;

    lookup = {
        static_cast<uint32_t>(channel.hash),
        channel.length | (month << 8),
    };
    value = {message_len, stamps};
    dictionary.prefetch_direct(channel.hash);
}

void parse_range_shared(
    const char* begin, const char* end, const ChannelMap& dictionary,
    SharedWorkerResult& result) {
    const char* p = begin;
    constexpr uint32_t batch_size = kSharedBatchSize;
    SharedLookupRow lookup_rows[batch_size];
    ValueRow value_rows[batch_size];
    uint32_t channel_ids[batch_size];

    while (p < end) {
        uint32_t count = 0;
        do {
            parse_shared_row(
                p, end, dictionary, lookup_rows[count], value_rows[count]);
            ++count;
        } while (count < batch_size && p < end);

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t metadata = lookup_rows[i].key_len_and_month;
            const uint32_t month = metadata >> 8;
            const uint32_t channel_id = dictionary.resolve_direct_hash(
                lookup_rows[i].hash);
            channel_ids[i] = channel_id | (month << 16);
            prefetch_shared_stat(result, channel_id, month);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t id_and_month = channel_ids[i];
            update_shared_stat(
                result, id_and_month & 0xffffu, id_and_month >> 16,
                value_rows[i].message_len, value_rows[i].stamps);
        }
        result.rows += count;
    }
}

void append_uint(std::string& out, std::uint32_t value) {
    char buffer[16];
    char* p = buffer + sizeof(buffer);
    do {
        *--p = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    out.append(p, static_cast<std::size_t>(buffer + sizeof(buffer) - p));
}

void append_average(std::string& out, uint32_t sum, uint32_t count) {
    char buffer[32];
    const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer),
        static_cast<double>(sum) / static_cast<double>(count),
        std::chars_format::fixed, 2);
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

std::size_t choose_thread_count(std::size_t bytes) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 8;
    std::size_t count = std::min<std::size_t>(hw, 8);
    while (count > 1 && bytes / count < 8u * 1024u * 1024u) --count;
    return std::max<std::size_t>(count, 1);
}

void append_line(
    std::string& out, const ChannelKey& key,
    const ChannelStats& channel, uint32_t month) {
    const MonthStat& stat = channel.month_at(month);
    out.append(key.key, key.key_len);
    out.append(",2027-", 6);
    out.push_back('0' + static_cast<char>((month + 1) / 10));
    out.push_back('0' + static_cast<char>((month + 1) % 10));
    out.push_back('=');
    append_uint(out, stat.min_len);
    out.push_back('/');
    append_average(out, stat.sum, stat.count);
    out.push_back('/');
    append_uint(out, stat.max_len);
    out.push_back('/');
    append_uint(out, stat.count);
    out.push_back('/');
    append_uint(out, stat.stamp_sum);
    out.push_back('\n');
}

std::string build_output(const ChannelMap& map) {
    std::string out;
    out.reserve(map.channels.size() * 12 * 80);
    for (std::size_t channel_id = 0;
         channel_id < map.channels.size(); ++channel_id) {
        const ChannelStats& channel = map.channels[channel_id];
        for (uint32_t month = 0; month < 12; ++month) {
            if (channel.month_at(month).count != 0) {
                append_line(
                    out, map.channel_keys[channel_id], channel, month);
            }
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
        return 2;
    }

    MappedFile input(argv[1]);
    const char* data_end = input.data + input.size;

    const char* rows_begin = input.data;
    while (*rows_begin != '\n') ++rows_begin;
    ++rows_begin;

    const std::size_t data_bytes = static_cast<std::size_t>(data_end - rows_begin);
    const std::size_t thread_count = choose_thread_count(data_bytes);

    std::vector<const char*> cuts(thread_count + 1);
    cuts[0] = rows_begin;
    cuts[thread_count] = data_end;
    for (std::size_t i = 1; i < thread_count; ++i) {
        const char* cut = rows_begin + data_bytes * i / thread_count;
        while (*cut != '\n') ++cut;
        cuts[i] = cut + 1;
    }

    ChannelMap shared_dictionary;
    const bool shared_dictionary_ready = try_build_shared_dictionary(
        rows_begin, data_end, shared_dictionary);
    if (shared_dictionary_ready) shared_dictionary.build_direct_lookup();
    if (shared_dictionary_ready && shared_dictionary.direct_hashes_unique) {
        std::vector<SharedWorkerResult> shared_results;
        shared_results.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            shared_results.emplace_back(shared_dictionary.channels.size());
        }

        std::vector<std::thread> shared_threads;
        shared_threads.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            shared_threads.emplace_back(
                parse_range_shared, cuts[i], cuts[i + 1],
                std::cref(shared_dictionary), std::ref(shared_results[i]));
        }
        for (auto& thread : shared_threads) thread.join();

        for (const SharedWorkerResult& result : shared_results) {
            for (std::size_t channel_id = 0;
                 channel_id < result.channels.size(); ++channel_id) {
                ChannelStats& target = shared_dictionary.channels[channel_id];
                const ChannelStats& source = result.channels[channel_id];
                for (uint32_t month = 0; month < 12; ++month) {
                    MonthStat& dst = target.month_at(month);
                    const MonthStat& src = source.month_at(month);
                    dst.sum += src.sum;
                    dst.count += src.count;
                    dst.stamp_sum += src.stamp_sum;
                    dst.min_len = std::min(dst.min_len, src.min_len);
                    dst.max_len = std::max(dst.max_len, src.max_len);
                }
            }
        }

        const std::string output = build_output(shared_dictionary);
        write_all(argv[2], output.data(), output.size());
        return 0;
    }

    std::vector<WorkerResult> results(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back(parse_range, cuts[i], cuts[i + 1], std::ref(results[i]));
    }
    for (auto& thread : threads) thread.join();

    ChannelMap merged;
    for (const WorkerResult& result : results) {
        for (std::size_t channel_id = 0;
             channel_id < result.map.channels.size(); ++channel_id) {
            merged.merge_channel(
                result.map.channel_keys[channel_id],
                result.map.channels[channel_id]);
        }
    }

    const std::string output = build_output(merged);
    write_all(argv[2], output.data(), output.size());
    return 0;
}
