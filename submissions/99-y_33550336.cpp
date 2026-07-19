#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <thread>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>

// 2027年 Unix秒 -> 月(1-12)
constexpr uint32_t BASE_TIME = 1798761600;
constexpr uint32_t SECONDS_PER_DAY = 86400;

inline uint32_t get_month_2027(uint32_t timestamp) noexcept {
    uint32_t days = (timestamp - BASE_TIME) / SECONDS_PER_DAY;
    if (days < 181) {
        if (days < 90) {
            if (days < 31) return 1;
            return (days < 59) ? 2 : 3;
        }
        if (days < 120) return 4;
        return (days < 151) ? 5 : 6;
    } else {
        if (days < 273) {
            if (days < 212) return 7;
            return (days < 243) ? 8 : 9;
        }
        if (days < 304) return 10;
        return (days < 334) ? 11 : 12;
    }
}

struct GroupStats {
    uint32_t min_length = 0xFFFFFFFF;
    uint32_t max_length = 0;
    uint32_t message_count = 0;
    uint32_t total_stamp_count = 0;
    uint64_t total_length = 0;

    inline void merge(uint32_t length, uint32_t stamps) noexcept {
        if (length < min_length) min_length = length;
        if (length > max_length) max_length = length;
        total_length += length;
        message_count++;
        total_stamp_count += stamps;
    }

    inline void merge_group(const GroupStats& other) noexcept {
        if (other.message_count == 0) return;
        if (other.min_length < min_length) min_length = other.min_length;
        if (other.max_length > max_length) max_length = other.max_length;
        total_length += other.total_length;
        message_count += other.message_count;
        total_stamp_count += other.total_stamp_count;
    }
};

constexpr size_t MAX_CHANNELS = 10050;
std::string g_channel_paths[MAX_CHANNELS];
uint32_t g_channel_count = 0;
std::mutex g_dict_mutex;

// ハッシュテーブルを大幅に拡大して衝突の探索ループをほぼ消滅させる
constexpr size_t HASH_SIZE = 131072; 
constexpr size_t HASH_MASK = HASH_SIZE - 1;
uint16_t g_hash_table[HASH_SIZE] = {0};

struct ThreadContext {
    GroupStats stats[MAX_CHANNELS][13];
};

// ハッシュ計算
inline uint64_t calculate_hash(const char* start, const char* end) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (const char* p = start; p < end; p++) {
        hash ^= static_cast<uint64_t>(*p);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint16_t get_or_insert_id(uint64_t hash, std::string_view path) {
    size_t idx = hash & HASH_MASK;
    while (true) [[likely]] {
        uint16_t slot = g_hash_table[idx];
        if (slot == 0) break;
        if (g_channel_paths[slot - 1] == path) return slot - 1;
        idx = (idx + 1) & HASH_MASK;
    }

    std::lock_guard<std::mutex> lock(g_dict_mutex);
    idx = hash & HASH_MASK;
    while (true) {
        uint16_t slot = g_hash_table[idx];
        if (slot == 0) {
            uint16_t new_id = g_channel_count++;
            g_channel_paths[new_id] = std::string(path);
            g_hash_table[idx] = new_id + 1;
            return new_id;
        }
        if (g_channel_paths[slot - 1] == path) return slot - 1;
        idx = (idx + 1) & HASH_MASK;
    }
}

inline const char* parse_uint32(const char* p, uint32_t& val) noexcept {
    uint32_t res = 0;
    while (*p >= '0' && *p <= '9') [[likely]] {
        res = res * 10 + (*p - '0');
        p++;
    }
    val = res;
    return p;
}

// madv_dontneed を呼ぶ単位 (32MB)
constexpr size_t ADVISE_CHUNK_SIZE = 32 * 1024 * 1024;

void worker_thread(const char* start_ptr, const char* end_ptr, ThreadContext& ctx) {
    const char* p = start_ptr;
    const char* last_advise_ptr = start_ptr;

    while (p < end_ptr) {
        // 一定量を処理するごとに、通過したメモリのページキャッシュを解放
        if (p - last_advise_ptr >= ADVISE_CHUNK_SIZE) {
            size_t len = p - last_advise_ptr;
            // ページ境界にアラインされるように調整して物理メモリから除外
            uintptr_t addr = reinterpret_cast<uintptr_t>(last_advise_ptr);
            uintptr_t aligned_addr = (addr + 4095) & ~4095;
            uintptr_t end_addr = reinterpret_cast<uintptr_t>(p) & ~4095;
            if (end_addr > aligned_addr) {
                madvise(reinterpret_cast<void*>(aligned_addr), end_addr - aligned_addr, MADV_DONTNEED);
            }
            last_advise_ptr = p;
        }

        uint32_t timestamp;
        p = parse_uint32(p, timestamp);
        p++; // ','

        const char* path_start = p;
        while (*p != ',') p++;
        const char* path_end = p;
        p++; // ','

        uint32_t length;
        p = parse_uint32(p, length);
        p++; // ','

        uint32_t stamps;
        p = parse_uint32(p, stamps);
        p++; // '\n'

        uint32_t month = get_month_2027(timestamp);
        uint64_t path_hash = calculate_hash(path_start, path_end);
        std::string_view path_view(path_start, path_end - path_start);
        uint16_t cid = get_or_insert_id(path_hash, path_view);
        
        ctx.stats[cid][month].merge(length, stamps);
    }

    // スレッド終了時に残りの部分を完全に解放
    if (p > last_advise_ptr) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(last_advise_ptr);
        uintptr_t aligned_addr = (addr + 4095) & ~4095;
        uintptr_t end_addr = reinterpret_cast<uintptr_t>(p) & ~4095;
        if (end_addr > aligned_addr) {
            madvise(reinterpret_cast<void*>(aligned_addr), end_addr - aligned_addr, MADV_DONTNEED);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) return 1;
    const char* input_path = argv[1];
    const char* output_path = argv[2];

    int fd = open(input_path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat sb;
    if (fstat(fd, &sb) == -1) return 1;
    size_t file_size = sb.st_size;
    if (file_size == 0) return 0;

    // ファイル全体をメモリマップ
    char* mmapped_data = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mmapped_data == MAP_FAILED) return 1;

    // OSに先読みのヒントを出す
    madvise(mmapped_data, file_size, MADV_SEQUENTIAL);

    // ヘッダーをスキップ
    const char* data_start = mmapped_data;
    while (data_start < mmapped_data + file_size && *data_start != '\n') {
        data_start++;
    }
    if (data_start < mmapped_data + file_size) data_start++;

    size_t data_len = (mmapped_data + file_size) - data_start;
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 8;

    std::vector<std::unique_ptr<ThreadContext>> contexts;
    for (unsigned int i = 0; i < num_threads; i++) {
        contexts.push_back(std::make_unique<ThreadContext>());
    }

    std::vector<std::thread> threads;
    size_t chunk_size = data_len / num_threads;
    const char* current_pos = data_start;
    const char* file_end = mmapped_data + file_size;

    for (unsigned int i = 0; i < num_threads; i++) {
        const char* start = current_pos;
        const char* end = (i == num_threads - 1) ? file_end : current_pos + chunk_size;

        if (i < num_threads - 1) {
            while (end < file_end && *end != '\n') {
                end++;
            }
            if (end < file_end) end++;
        }
        current_pos = end;

        if (start < end) {
            threads.emplace_back(worker_thread, start, end, std::ref(*contexts[i]));
        }
    }

    for (auto& th : threads) {
        th.join();
    }

    struct FinalResult {
        std::string path;
        uint32_t month;
        GroupStats stats;
    };
    std::vector<FinalResult> final_results;
    final_results.reserve(g_channel_count * 12);

    for (uint32_t cid = 0; cid < g_channel_count; cid++) {
        for (uint32_t m = 1; m <= 12; m++) {
            GroupStats master_stats;
            for (unsigned int i = 0; i < num_threads; i++) {
                master_stats.merge_group(contexts[i]->stats[cid][m]);
            }
            if (master_stats.message_count > 0) {
                final_results.push_back({g_channel_paths[cid], m, master_stats});
            }
        }
    }

    std::string out_buf;
    out_buf.reserve(final_results.size() * 128);
    char tmp[256];
    for (const auto& res : final_results) {
        double avg = (double)res.stats.total_length / res.stats.message_count;
        int len = snprintf(tmp, sizeof(tmp), "%s,2027-%02u=%u/%.2f/%u/%u/%u\n",
            res.path.c_str(),
            res.month,
            res.stats.min_length,
            avg,
            res.stats.max_length,
            res.stats.message_count,
            res.stats.total_stamp_count
        );
        out_buf.append(tmp, len);
    }

    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        write(out_fd, out_buf.data(), out_buf.size());
        close(out_fd);
    }

    munmap(mmapped_data, file_size);
    close(fd);
    return 0;
}