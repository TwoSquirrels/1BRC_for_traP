#line 1 "src/main.cpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#line 1 "src/chunk.hpp"
#line 2 "src/chunk.hpp"

// mmapした入力データを、スレッド数に応じて「完全な行の集合」に分割する。
// 各チャンクの境界は次の改行の直後に揃えるため、行の途中で分断されることはない。

#include <cstddef>
#include <cstring>
#include <vector>

struct Chunk {
    size_t begin;
    size_t end;
};

// data[0, size) を thread_count 個以下のチャンクに分割する。
// skip_header が true の場合、先頭行(ヘッダ)を全チャンクの対象から除外する。
inline std::vector<Chunk> split_chunks(const char* data, size_t size, int thread_count, bool skip_header) {
    std::vector<Chunk> chunks;
    if (thread_count < 1) {
        thread_count = 1;
    }
    chunks.reserve(static_cast<size_t>(thread_count));

    size_t start = 0;
    if (skip_header && size > 0) {
        const char* nl = static_cast<const char*>(memchr(data, '\n', size));
        start = nl != nullptr ? static_cast<size_t>(nl - data) + 1 : size;
    }

    if (start >= size) {
        return chunks;
    }

    const size_t remaining = size - start;

    std::vector<size_t> bounds(static_cast<size_t>(thread_count) + 1);
    bounds[0] = start;
    bounds[static_cast<size_t>(thread_count)] = size;
    for (int i = 1; i < thread_count; ++i) {
        size_t pos = start + remaining * static_cast<size_t>(i) / static_cast<size_t>(thread_count);
        const char* nl = static_cast<const char*>(memchr(data + pos, '\n', size - pos));
        bounds[static_cast<size_t>(i)] = nl != nullptr ? static_cast<size_t>(nl - data) + 1 : size;
    }

    for (int i = 0; i < thread_count; ++i) {
        size_t b = bounds[static_cast<size_t>(i)];
        size_t e = bounds[static_cast<size_t>(i) + 1];
        if (b < e) {
            chunks.push_back({b, e});
        }
    }
    return chunks;
}
#line 14 "src/main.cpp"
#line 1 "src/hash_table.hpp"
#line 2 "src/hash_table.hpp"

// control配列とdense entry配列を分離したオープンアドレス法ハッシュテーブル。
// プローブ中は8バイトのcontrolだけを連続走査し、fingerprintが一致したときだけ
// 大きなEntryと文字列へアクセスする。

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#line 1 "src/stats.hpp"
#line 2 "src/stats.hpp"

// チャンネル×月ごとに集計する統計値。

#include <algorithm>
#include <cstdint>
#include <limits>

struct Stats {
    int64_t sum_len = 0;
    int64_t sum_stamp = 0;
    uint64_t count = 0;
    uint32_t min_len = std::numeric_limits<uint32_t>::max();
    uint32_t max_len = 0;

    inline void add(uint32_t len, uint32_t stamp) {
        sum_len += len;
        sum_stamp += stamp;
        count += 1;
        min_len = std::min(min_len, len);
        max_len = std::max(max_len, len);
    }

    inline void merge(const Stats& other) {
        sum_len += other.sum_len;
        sum_stamp += other.sum_stamp;
        count += other.count;
        min_len = std::min(min_len, other.min_len);
        max_len = std::max(max_len, other.max_len);
    }
};
#line 15 "src/hash_table.hpp"

struct Entry {
    const char* key_ptr = nullptr;
    Stats stats;
    uint32_t hash = 0;
    uint32_t key_len = 0;
    uint32_t ym = 0;
};

inline uint32_t normalize_hash(uint64_t h) {
    uint32_t folded = static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32);
    return folded == 0 ? 1 : folded;
}

inline uint32_t finish_channel_hash(uint64_t channel_hash, uint32_t ym) {
    channel_hash ^= ym;
    channel_hash *= 1099511628211ULL;
    return normalize_hash(channel_hash);
}
inline uint32_t hash_key(const char* data, uint32_t len, uint32_t ym) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    h ^= ym;
    h *= 1099511628211ULL;
    return normalize_hash(h);
}

class HashTable {
public:
    explicit HashTable(size_t expected_entries = 128) { reserve(expected_entries); }

    void reserve(size_t expected_entries) {
        const size_t wanted = expected_entries < 128 ? 128 : expected_entries;
        size_t capacity = std::bit_ceil((wanted * 10 + 6) / 7);
        if (capacity < 256) capacity = 256;
        if (!entries_.empty() || controls_.size() >= capacity) return;
        controls_.assign(capacity, 0);
        entries_.reserve(wanted);
        mask_ = capacity - 1;
    }

    inline void upsert(const char* key, uint32_t key_len, uint32_t ym, uint32_t len, uint32_t stamp) {
        upsert_prehashed(hash_key(key, key_len, ym), key, key_len, ym, len, stamp);
    }

    inline void upsert_prehashed(uint32_t hash, const char* key, uint32_t key_len, uint32_t ym,
                                 uint32_t len, uint32_t stamp) {
        size_t empty_bucket = 0;
        uint32_t entry_index = find(hash, key, key_len, ym, empty_bucket);
        if (entry_index == kNotFound) {
            if ((entries_.size() + 1) * 10 > controls_.size() * 7) [[unlikely]] {
                grow();
                entry_index = find(hash, key, key_len, ym, empty_bucket);
            }
            entry_index = static_cast<uint32_t>(entries_.size());
            entries_.push_back(Entry{key, Stats{}, hash, key_len, ym});
            key_bytes_ += key_len;
            controls_[empty_bucket] = make_control(entry_index);
        }
        entries_[entry_index].stats.add(len, stamp);
    }

    inline void merge_entry(const Entry& other) {
        size_t empty_bucket = 0;
        uint32_t entry_index = find(other.hash, other.key_ptr, other.key_len, other.ym, empty_bucket);
        if (entry_index == kNotFound) {
            if ((entries_.size() + 1) * 10 > controls_.size() * 7) [[unlikely]] {
                grow();
                entry_index = find(other.hash, other.key_ptr, other.key_len, other.ym, empty_bucket);
            }
            entry_index = static_cast<uint32_t>(entries_.size());
            entries_.push_back(other);
            key_bytes_ += other.key_len;
            controls_[empty_bucket] = make_control(entry_index);
            return;
        }
        entries_[entry_index].stats.merge(other.stats);
    }

    const std::vector<Entry>& entries() const { return entries_; }
    size_t occupied() const { return entries_.size(); }
    size_t key_bytes() const { return key_bytes_; }

private:
    static constexpr uint32_t kNotFound = std::numeric_limits<uint32_t>::max();

    static inline uint32_t make_control(uint32_t entry_index) {
        return entry_index + 1;
    }

    inline uint32_t find(uint32_t hash, const char* key, uint32_t key_len, uint32_t ym,
                         size_t& empty_bucket) const {
        size_t bucket = hash & mask_;
        for (;;) {
            const uint32_t control = controls_[bucket];
            if (control == 0) {
                empty_bucket = bucket;
                return kNotFound;
            }
            const uint32_t index = control - 1;
            const Entry& entry = entries_[index];
            if (entry.hash == hash && entry.ym == ym && entry.key_len == key_len &&
                memcmp(entry.key_ptr, key, key_len) == 0) {
                return index;
            }
            bucket = (bucket + 1) & mask_;
        }
    }

    void grow() {
        const size_t new_capacity = controls_.size() * 2;
        controls_.assign(new_capacity, 0);
        mask_ = new_capacity - 1;
        for (uint32_t i = 0; i < entries_.size(); ++i) {
            const uint32_t hash = entries_[i].hash;
            size_t bucket = hash & mask_;
            while (controls_[bucket] != 0) bucket = (bucket + 1) & mask_;
            controls_[bucket] = make_control(i);
        }
    }

    std::vector<uint32_t> controls_;
    std::vector<Entry> entries_;
    size_t mask_ = 0;
    size_t key_bytes_ = 0;
};
#line 15 "src/main.cpp"
#line 1 "src/mmap_file.hpp"
#line 2 "src/mmap_file.hpp"

// 入力CSVをmmapするRAIIラッパ。
// PROT_READ/MAP_PRIVATEでゼロコピー読み込みし、madviseで順次読み込みを予告する。

#include <cstddef>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class MmapFile {
public:
    explicit MmapFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error("failed to open input file: " + path);
        }

        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("fstat failed: " + path);
        }
        size_ = static_cast<size_t>(st.st_size);

        if (size_ == 0) {
            // 空ファイルはmmapせず、data()==nullptrとして扱う。
            return;
        }

        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) {
            ::close(fd_);
            throw std::runtime_error("mmap failed: " + path);
        }
        data_ = static_cast<const char*>(p);

        ::madvise(p, size_, MADV_SEQUENTIAL);
        // 巨大入力全体へのWILLNEEDは、64GiB環境でページキャッシュを押し出し得る。
        // 小規模入力だけを先読みし、1GiB超はkernelの逐次read-aheadへ任せる。
        if (size_ <= (1ULL << 30)) {
            ::madvise(p, size_, MADV_WILLNEED);
        }
    }

    ~MmapFile() {
        if (data_ != nullptr) {
            ::munmap(const_cast<char*>(data_), size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    const char* data() const { return data_; }
    size_t size() const { return size_; }

private:
    int fd_ = -1;
    const char* data_ = nullptr;
    size_t size_ = 0;
};
#line 16 "src/main.cpp"
#line 1 "src/output.hpp"
#line 2 "src/output.hpp"

// dense entry列を出力バッファへ整形し、複数shardをwritevで一括出力する。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#line 17 "src/output.hpp"
#line 1 "src/ym.hpp"
#line 2 "src/ym.hpp"

// unix秒 -> 「年月インデックス」変換。
// ym index = (year - 1970) * 12 + (month - 1) という uint32 1個で年月を表す。
// 文字列化(YYYY-MM)は出力時にのみ行う。

#include <cstdint>
#include <utility>
#include <vector>

namespace ym {

// Howard Hinnant の civil_from_days アルゴリズム。
// z: 1970-01-01からの経過日数(負も許容)。戻り値: {year, month(1-12)}。
inline std::pair<int64_t, unsigned> civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                // [0, 11]
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;                           // [1, 12]
    return {y + static_cast<int64_t>(m <= 2), m};
}

inline uint32_t index_from_civil(int64_t year, unsigned month) {
    return static_cast<uint32_t>((year - 1970) * 12 + static_cast<int64_t>(month - 1));
}

// 1970-01-01 (day=0) 〜 2130-01-01 (day=58439, 排他的上限)をカバーするLUT。
constexpr int64_t kLutDays = 58440;

class Lut {
public:
    Lut() {
        table_.resize(static_cast<size_t>(kLutDays));
        for (int64_t d = 0; d < kLutDays; ++d) {
            const auto [y, m] = civil_from_days(d);
            table_[static_cast<size_t>(d)] = index_from_civil(y, m);
        }
    }

    inline uint32_t operator()(int64_t day) const {
        if (day >= 0 && day < kLutDays) [[likely]] {
            return table_[static_cast<size_t>(day)];
        }
        const auto [y, m] = civil_from_days(day);
        return index_from_civil(y, m);
    }

private:
    std::vector<uint32_t> table_;
};

}  // namespace ym
#line 18 "src/output.hpp"

namespace output {

constexpr size_t kMaxLineOverhead = 160;

inline constexpr auto kDigitPairs = [] {
    std::array<char, 200> table{};
    for (unsigned i = 0; i < 100; ++i) {
        table[i * 2] = static_cast<char>('0' + i / 10);
        table[i * 2 + 1] = static_cast<char>('0' + i % 10);
    }
    return table;
}();

inline char* write_uint(char* buf, uint64_t v) {
    char tmp[20];
    char* p = tmp + sizeof(tmp);
    while (v >= 100) {
        const uint64_t q = v / 100;
        const unsigned r = static_cast<unsigned>(v - q * 100);
        p -= 2;
        memcpy(p, kDigitPairs.data() + r * 2, 2);
        v = q;
    }
    if (v < 10) {
        *--p = static_cast<char>('0' + v);
    } else {
        p -= 2;
        memcpy(p, kDigitPairs.data() + static_cast<unsigned>(v) * 2, 2);
    }
    const size_t n = static_cast<size_t>(tmp + sizeof(tmp) - p);
    memcpy(buf, p, n);
    return buf + n;
}

inline char* write_ym(char* buf, uint32_t ym_index) {
    const int64_t year = 1970 + static_cast<int64_t>(ym_index) / 12;
    const unsigned month = static_cast<unsigned>(ym_index % 12) + 1;
    char ytmp[24];
    int yn = 0;
    int64_t yv = year;
    if (yv == 0) ytmp[yn++] = '0';
    while (yv > 0) {
        ytmp[yn++] = static_cast<char>('0' + yv % 10);
        yv /= 10;
    }
    for (int i = yn; i < 4; ++i) *buf++ = '0';
    while (yn > 0) *buf++ = ytmp[--yn];
    *buf++ = '-';
    memcpy(buf, kDigitPairs.data() + month * 2, 2);
    return buf + 2;
}

inline char* write_avg(char* buf, int64_t sum, uint64_t count) {
    const unsigned __int128 scaled = static_cast<unsigned __int128>(sum) * 100;
    uint64_t hundredths = static_cast<uint64_t>(scaled / count);
    const uint64_t remainder = static_cast<uint64_t>(scaled % count);
    const unsigned __int128 twice_remainder = static_cast<unsigned __int128>(remainder) * 2;
    if (twice_remainder == count) [[unlikely]] {
        char tmp[64];
        const int n = snprintf(tmp, sizeof(tmp), "%.2f", static_cast<double>(sum) / static_cast<double>(count));
        memcpy(buf, tmp, static_cast<size_t>(n));
        return buf + n;
    }
    hundredths += twice_remainder > count;
    buf = write_uint(buf, hundredths / 100);
    *buf++ = '.';
    const unsigned fraction = static_cast<unsigned>(hundredths % 100);
    memcpy(buf, kDigitPairs.data() + fraction * 2, 2);
    return buf + 2;
}

inline std::string format_shard(const HashTable& table) {
    const size_t capacity = table.occupied() * kMaxLineOverhead + table.key_bytes();
    std::string buf;
    buf.resize_and_overwrite(capacity, [&](char* begin, size_t) {
        char* p = begin;
        for (const Entry& entry : table.entries()) {
            memcpy(p, entry.key_ptr, entry.key_len);
            p += entry.key_len;
            *p++ = ',';
            p = write_ym(p, entry.ym);
            *p++ = '=';
            p = write_uint(p, entry.stats.min_len);
            *p++ = '/';
            p = write_avg(p, entry.stats.sum_len, entry.stats.count);
            *p++ = '/';
            p = write_uint(p, entry.stats.max_len);
            *p++ = '/';
            p = write_uint(p, entry.stats.count);
            *p++ = '/';
            p = write_uint(p, static_cast<uint64_t>(entry.stats.sum_stamp));
            *p++ = '\n';
        }
        return static_cast<size_t>(p - begin);
    });
    return buf;
}
template <size_t N>
inline void write_buffers(const std::array<std::string, N>& buffers, const std::string& out_path) {
    const int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::runtime_error("failed to open output file: " + out_path);
    std::array<iovec, N> vectors{};
    int count = 0;
    for (const std::string& buffer : buffers) {
        if (!buffer.empty()) vectors[count++] = {const_cast<char*>(buffer.data()), buffer.size()};
    }
    int first = 0;
    while (first < count) {
        const ssize_t written = ::writev(fd, vectors.data() + first, count - first);
        if (written <= 0) {
            ::close(fd);
            throw std::runtime_error("writev failed: " + out_path);
        }
        size_t consumed = static_cast<size_t>(written);
        while (first < count && consumed >= vectors[first].iov_len) {
            consumed -= vectors[first].iov_len;
            ++first;
        }
        if (first < count && consumed != 0) {
            vectors[first].iov_base = static_cast<char*>(vectors[first].iov_base) + consumed;
            vectors[first].iov_len -= consumed;
        }
    }
    ::close(fd);
}

}  // namespace output
#line 17 "src/main.cpp"
#line 1 "src/parse.hpp"
#line 2 "src/parse.hpp"

// CSV行を左から一度だけ走査するscalar parser。
// delimiter探索、整数変換、channelのFNV-1a計算を同じループで行う。

#include <cstdint>

struct Record {
    int64_t timestamp;
    const char* channel;
    uint64_t channel_hash;
    uint32_t channel_len;
    uint32_t message_length;
    uint32_t stamp_count;
};

inline const char* parse_line(const char* p, const char* end, Record& rec, bool& ok) {
    ok = false;
    if (p >= end) return end;
    if (*p == '\n') return p + 1;

    int64_t timestamp = 0;
    while (p < end && *p != ',') {
        timestamp = timestamp * 10 + (*p - '0');
        ++p;
    }
    if (p == end) return end;
    ++p;

    const char* channel = p;
    uint64_t channel_hash = 1469598103934665603ULL;
    while (p < end && *p != ',') {
        channel_hash ^= static_cast<unsigned char>(*p);
        channel_hash *= 1099511628211ULL;
        ++p;
    }
    if (p == end) return end;
    const uint32_t channel_len = static_cast<uint32_t>(p - channel);
    ++p;

    uint32_t message_length = 0;
    while (p < end && *p != ',') {
        message_length = message_length * 10 + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (p == end) return end;
    ++p;

    uint32_t stamp_count = 0;
    while (p < end && *p != '\n' && *p != '\r') {
        stamp_count = stamp_count * 10 + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (p < end && *p == '\r') {
        ++p;
        if (p < end && *p == '\n') ++p;
    } else if (p < end && *p == '\n') {
        ++p;
    }

    rec.timestamp = timestamp;
    rec.channel = channel;
    rec.channel_hash = channel_hash;
    rec.channel_len = channel_len;
    rec.message_length = message_length;
    rec.stamp_count = stamp_count;
    ok = true;
    return p;
}
#line 18 "src/main.cpp"
#line 19 "src/main.cpp"

namespace {

constexpr size_t kShardCount = 8;
constexpr size_t kBytesPerWorker = 3ULL << 20;
constexpr size_t kMaxInitialEntriesPerShard = 1ULL << 14;

struct alignas(64) WorkerState {
    std::array<HashTable, kShardCount> tables;

    explicit WorkerState(size_t chunk_bytes) {
        const size_t estimate = std::max<size_t>(128, chunk_bytes / 64 / kShardCount);
        const size_t expected_per_shard = std::min(estimate, kMaxInitialEntriesPerShard);
        for (HashTable& table : tables) table.reserve(expected_per_shard);
    }
};

void write_empty_output(const std::string& out_path) {
    const int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) ::close(fd);
}

void worker(const char* begin, const char* end, const ym::Lut* lut, WorkerState* state) {
    const char* p = begin;
    while (p < end) {
        Record rec;
        bool ok = false;
        p = parse_line(p, end, rec, ok);
        if (!ok) continue;
        const int64_t day = rec.timestamp / 86400;
        const uint32_t ym_index = (*lut)(day);
        const uint32_t hash = finish_channel_hash(rec.channel_hash, ym_index);
        state->tables[hash >> 30].upsert_prehashed(hash, rec.channel, rec.channel_len, ym_index,
                                                   rec.message_length, rec.stamp_count);
    }
}

int decide_thread_count(size_t input_size) {
    if (const char* env = std::getenv("ONE_BRC_THREADS")) {
        const int v = std::atoi(env);
        if (v > 0) return v;
    }
    const unsigned hw = std::max(1U, std::thread::hardware_concurrency());
    const size_t by_size = std::max<size_t>(1, (input_size + kBytesPerWorker - 1) / kBytesPerWorker);
    return static_cast<int>(std::min<size_t>({by_size, hw, 8}));
}

}  // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.csv> <output.txt>\n", argc > 0 ? argv[0] : "program");
        return 1;
    }
    const std::string in_path = argv[1];
    const std::string out_path = argv[2];

    try {
        MmapFile file(in_path);
        if (file.size() == 0) {
            write_empty_output(out_path);
            return 0;
        }
        const bool skip_header = !(file.data()[0] >= '0' && file.data()[0] <= '9');
        const int thread_count = decide_thread_count(file.size());
        const std::vector<Chunk> chunks = split_chunks(file.data(), file.size(), thread_count, skip_header);
        if (chunks.empty()) {
            write_empty_output(out_path);
            return 0;
        }
        const ym::Lut lut;
        std::vector<WorkerState> states;
        states.reserve(chunks.size());
        for (const Chunk& chunk : chunks) states.emplace_back(chunk.end - chunk.begin);
        const auto initialized = Clock::now();

        std::vector<std::thread> threads;
        threads.reserve(chunks.size());
        for (size_t i = 0; i < chunks.size(); ++i) {
            threads.emplace_back(worker, file.data() + chunks[i].begin, file.data() + chunks[i].end,
                                 &lut, &states[i]);
        }
        for (std::thread& thread : threads) thread.join();
        const auto parsed = Clock::now();

        std::array<HashTable, kShardCount> merged;
        std::array<std::string, kShardCount> buffers;
        threads.clear();
        threads.reserve(kShardCount);
        for (size_t shard = 0; shard < kShardCount; ++shard) {
            threads.emplace_back([&, shard] {
                size_t local_entries = 0;
                for (const WorkerState& state : states) local_entries += state.tables[shard].occupied();
                merged[shard].reserve(std::max<size_t>(128, local_entries / states.size()));
                for (const WorkerState& state : states) {
                    for (const Entry& entry : state.tables[shard].entries()) merged[shard].merge_entry(entry);
                }
                buffers[shard] = output::format_shard(merged[shard]);
            });
        }
        for (std::thread& thread : threads) thread.join();
        const auto merged_and_formatted = Clock::now();
        output::write_buffers(buffers, out_path);
        const auto finished = Clock::now();

        if (std::getenv("ONE_BRC_PROFILE") != nullptr) {
            const auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr, "profile init=%.3fms parse=%.3fms merge_format=%.3fms write=%.3fms threads=%zu\n",
                         ms(started, initialized), ms(initialized, parsed),
                         ms(parsed, merged_and_formatted), ms(merged_and_formatted, finished), chunks.size());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
