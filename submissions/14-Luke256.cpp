// proram input.csv output.txt
// input は次の形式のCSV
// unix_timestamp,channel_path,message_length,stamp_count
// outputはこれらからチャンネル・月ごとの統計情報を出力:
// channel_path,YYYY-MM=min_length/average_length/max_length/message_count/total_stamp_count

#pragma GCC optimize("Ofast")
#include <bits/stdc++.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <x86intrin.h>
#include <charconv>
#pragma GCC target("avx2,sse4.2,popcnt")
using namespace std;
#ifdef LOCAL
#define perf_cpt(x, start) x += chrono::duration_cast<chrono::nanoseconds>(chrono::high_resolution_clock::now() - start).count(); start = chrono::high_resolution_clock::now();
#else
#define perf_cpt(x, start)
#endif

using int64 = long long;
using uint64 = unsigned long long;
using uint = unsigned int;

struct Stats {
    char channel_path[128] = {};
    uint channel_path_len = 0;
    uint hash = 0;
    uint ym = 0;
    uint min_len = 1000000;
    uint max_len = 0;
    uint sum_len = 0;
    uint message_count = 0;
    uint64 total_stamp_count = 0;

    void merge(const Stats& other) {
        min_len = min(min_len, other.min_len);
        max_len = max(max_len, other.max_len);
        sum_len += other.sum_len;
        message_count += other.message_count;
        total_stamp_count += other.total_stamp_count;
    }
};

static constexpr auto month_table = []() {
    array<uint, 366> t{};
    for (int i = 0; i < 366; ++i) {
        if (i < 31) t[i] = 1;
        else if (i < 59) t[i] = 2;
        else if (i < 90) t[i] = 3;
        else if (i < 120) t[i] = 4;
        else if (i < 151) t[i] = 5;
        else if (i < 181) t[i] = 6;
        else if (i < 212) t[i] = 7;
        else if (i < 243) t[i] = 8;
        else if (i < 273) t[i] = 9;
        else if (i < 304) t[i] = 10;
        else if (i < 334) t[i] = 11;
        else t[i] = 12;
    }
    return t;
}();

static uint month_ym_from_unix_timestamp(int64 ts) {
    // 2027-01-01 - 2027-12-31 の範囲に限定
    constexpr int64 OFFSET = 1798761600; // 2027-01-01 00:00:00 UTC
    int64 days = (ts - OFFSET) / 86400;

    return (2027 << 4) | month_table[days];
}

static inline uint64_t init_hash(uint64_t x) {
    // XOR shift の1回目のシフトを行う
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static uint64_t crc_hash(const char* data, size_t len, uint ym) {
    uint64_t h = init_hash(ym);
    uint64_t s = init_hash(len);
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t v;
        __builtin_memcpy(&v, data + i, 8);
        h = _mm_crc32_u64(h, v);
        s = _mm_crc32_u64(s, v);
    }

    if (i + 4 <= len) {
        uint v;
        __builtin_memcpy(&v, data + i, 4);
        h = _mm_crc32_u32(h, v);
        s = _mm_crc32_u32(s, v);
        i += 4;
    }
    for (; i < len; ++i) {
        h = _mm_crc32_u8(h, static_cast<uint8_t>(data[i]));
        s = _mm_crc32_u8(s, static_cast<uint8_t>(data[i]));
    }
    return (h << 32) | s;
}

inline uint64_t parse_ts(const char* p) {
    uint64_t top;
    __builtin_memcpy(&top, p, 8);
    top = __builtin_bswap64(top);
    top -= 0x3030303030303030;
    top += (top * 10) >> 8;
    top &= 0x00FF00FF00FF00FF;
    top += (top * 100) >> 16;
    top &= 0x0000FFFF0000FFFF;
    top += (top * 10000) >> 32;
    top &= 0x00000000FFFFFFFF;
    
    top = top * 10 + p[8];
    top = top * 10 + p[9];
    return top - 48 * 11;
}

constexpr size_t TABLE_SIZE = 1 << 18;
constexpr size_t TABLE_MASK = TABLE_SIZE - 1;
constexpr size_t MAX_TABLE_USED = 10000;
constexpr size_t SWS_CTRL_SIZE = TABLE_SIZE >> 3;
constexpr size_t SWS_CTRL_MASK = SWS_CTRL_SIZE - 1;

struct SwissTable {
    vector<Stats> table;
    vector<uint> indices;
    vector<uint64_t> control_bytes;
    SwissTable() : table(TABLE_SIZE), control_bytes(SWS_CTRL_SIZE) {
        indices.reserve(MAX_TABLE_USED);
    }
    
    inline Stats* get(const char* __restrict path, size_t path_len, uint ym, bool* __restrict found) {
        uint64_t h = crc_hash(path, path_len, ym);
        uint group = (h >> 7) & SWS_CTRL_MASK; 
        uint8_t h2 = static_cast<uint8_t>((h & 0x7F) | 0x80); 

        // ループ外で定数を計算
        const uint64_t h2_broadcast = h2 * 0x0101010101010101ULL;
        const uint64_t msb_mask = 0x8080808080808080ULL;

        // operator[]の境界チェックや毎回のメモリアドレス計算を避けるため、生ポインタを取得
        uint64_t* __restrict ctrl_ptr = control_bytes.data();
        Stats* __restrict table_ptr = table.data();

        for(;;)
        {
            uint64_t ctrl = ctrl_ptr[group];

            // 1. SWAR (SIMD Within A Register) によるマッチング
            // _mm_cvtsi64_si128等によるXMMレジスタへの転送ペナルティを排除
            uint64_t diff = ctrl ^ h2_broadcast;
            uint64_t match_mask = (diff - 0x0101010101010101ULL) & ~diff & msb_mask;

            while (match_mask)
            {
                // マッチしたバイト位置を算出 (ビット位置を8で割る)
                int slot = __builtin_ctzll(match_mask) >> 3;
                uint idx = (group << 3) | slot;

                auto& entry = table_ptr[idx];
                if (entry.ym == ym && 
                    entry.channel_path_len == path_len &&
                    __builtin_memcmp(entry.channel_path, path, path_len) == 0)
                {
                    *found = true;
                    return &entry;
                }

                // 2. 処理済みの最下位ビットをクリア (BLSR命令相当)
                match_mask &= (match_mask - 1);
            }

            // 3. 空きスロットの探索もSWARで実行
            uint64_t empty_mask = (ctrl - 0x0101010101010101ULL) & ~ctrl & msb_mask;
            if (empty_mask) {
                int slot = __builtin_ctzll(empty_mask) >> 3;
                uint idx = (group << 3) | slot;
                indices.push_back(idx);

                ctrl_ptr[group] |= (static_cast<uint64_t>(h2) << (slot << 3));

                *found = false;
                return &table_ptr[idx];
            }

            group = (group + 1) & SWS_CTRL_MASK;
        }
    }
};

struct WorkerResult {
    SwissTable table;

#ifdef LOCAL
    uint records = 0;
    uint ts_t = 0;
    uint path_t = 0;
    uint msg_len_t = 0;
    uint stamp_count_t = 0;
    uint hash_t = 0;
    uint insert_t = 0;
#endif
};

void process_line(const char* start, const char* end, WorkerResult& result) {
    const char *p = start;

    auto &statsTable = result.table;

    while (p < end) {
#ifdef LOCAL
        auto start = chrono::high_resolution_clock::now();
#endif

        // 1. unix_timestamp
        uint64 ts = parse_ts(p);
        p += 11;

perf_cpt(result.ts_t, start);

        // 2. channel_path (hash based on FNV-1a)
        const char* path_start = p;
        const char* path_end = static_cast<const char*>(__builtin_memchr(p, ',', end - p));
        size_t channel_path_len = path_end - path_start;
        p = path_end + 1;

perf_cpt(result.path_t, start);

        // 3. message_length
        uint msg_len = 0;
        do {
            msg_len = (msg_len << 3) + (msg_len << 1) + (*p++ - '0');
        } while (p < end && *p != ',');
        p++;

perf_cpt(result.msg_len_t, start);

        // 4. stamp_count
        uint64 stamp_count = 0;
        do {
            stamp_count = (stamp_count << 3) + (stamp_count << 1) + (*p++ - '0');
        } while (p < end && *p != '\n' && *p != '\r');
        p++;

perf_cpt(result.stamp_count_t, start);

        // 日付計算
        uint ym = month_ym_from_unix_timestamp(ts);

        // ハッシュテーブルへの反映
        uint h = crc_hash(path_start, channel_path_len, ym);
        uint idx = h & TABLE_MASK;
    
perf_cpt(result.hash_t, start);
        bool found = false;
        Stats& entry = *statsTable.get(path_start, channel_path_len, ym, &found);

        if (found) 
        {
            // 既存
            entry.min_len = min(entry.min_len, msg_len);
            entry.max_len = max(entry.max_len, msg_len);
            entry.sum_len += msg_len;
            entry.message_count += 1;
            entry.total_stamp_count += stamp_count;
        }
        else
        {
            // 新規挿入
            entry.ym = ym;
            __builtin_memcpy(entry.channel_path, path_start, channel_path_len);
            entry.channel_path[channel_path_len] = '\0';
            entry.channel_path_len = channel_path_len;
            entry.hash = h;
            entry.min_len = msg_len;
            entry.max_len = msg_len;
            entry.sum_len = msg_len;
            entry.message_count = 1;
            entry.total_stamp_count = stamp_count;
        }
perf_cpt(result.insert_t, start);
#ifdef LOCAL
        result.records++;
#endif
    }
}

void process_chunk(int fd, size_t start_offset, size_t end_offset, WorkerResult& result) {
    constexpr size_t BUFFER_SIZE = 2 * 1024 * 1024; // 2MB buffer
    constexpr size_t MAX_LINE_LEN = 128;

    char *buf = nullptr;
    if(posix_memalign(reinterpret_cast<void**>(&buf), 4096, BUFFER_SIZE + MAX_LINE_LEN) != 0) {
        cerr << "Failed to allocate aligned buffer\n";
        return;
    }

    size_t current_offset = start_offset;
    size_t leftover = 0;

    while (current_offset < end_offset) {
        size_t read_size = min(BUFFER_SIZE, end_offset - current_offset);
        
        // cerr << "[Thread" << start_offset << "] " << "Reading " << read_size << " bytes from offset " << current_offset << " / " << end_offset << endl;
        ssize_t bytes_read = pread(fd, buf + leftover, read_size, current_offset);
        if (bytes_read <= 0) break;
        
        char *valid_start = buf;
        char *valid_end = buf + leftover + bytes_read;
        
        // read whole chunk
        if (current_offset + bytes_read == end_offset) {
            process_line(valid_start, valid_end, result);
            break;
        }
        
        // find last newline
        char *last_nl = static_cast<char*>(memrchr(valid_start, '\n', valid_end - valid_start));
        
        char* parse_end = last_nl + 1;
        
        process_line(valid_start, parse_end, result);

        leftover = valid_end - parse_end;
        __builtin_memmove(buf, parse_end, leftover);
        current_offset += bytes_read;
    }

    free(buf);

#ifdef LOCAL
    cerr << "{ \"ts_t\": " << result.ts_t
         << ", \"path_t\": " << result.path_t
         << ", \"msg_len_t\": " << result.msg_len_t
         << ", \"stamp_count_t\": " << result.stamp_count_t
         << ", \"hash_t\": " << result.hash_t
         << ", \"insert_t\": " << result.insert_t
         << ", \"records\": " << result.records
         << " },\n";
#endif
}

void output_stats(
    const std::string& output_path, 
    const std::vector<uint32_t>& indices, 
    const std::vector<Stats>& stats) 
{
    const size_t num_indices = indices.size();
    if (num_indices == 0) return;

    // 1. 全データが一括で収まる巨大バッファを1回だけ確保
    constexpr size_t ESTIMATED_MAX_ROW_SIZE = 128;
    std::vector<char> out_buf(num_indices * ESTIMATED_MAX_ROW_SIZE);
    
    char* p = out_buf.data();
    char* const buf_end = p + out_buf.size();

    // 2. 2桁固定値（00-15）用の超高速コンパイル時ルックアップテーブル (LUT)
    // (s.ym & 0xF) は最大でも15なので、100未満の2桁文字テーブルを constexpr で用意
    static constexpr auto lut_2digits = []() {
        std::array<std::array<char, 2>, 100> t{};
        for (int i = 0; i < 100; ++i) {
            t[i][0] = '0' + (i / 10);
            t[i][1] = '0' + (i % 10);
        }
        return t;
    }();

    // ループ内でのインダイレクションを最適化するため、生ポインタを取得
    const uint32_t* const idx_ptr = indices.data();
    const Stats* const stats_ptr = stats.data();

    for (size_t i = 0; i < num_indices; ++i) {

        const Stats& s = stats_ptr[idx_ptr[i]];
        
        // --- 超高速 Zero-Copy データ書き込み ---
        
        // パス文字列のコピー（インライン展開される memcpy）
        __builtin_memcpy(p, s.channel_path, s.channel_path_len);
        p += s.channel_path_len;
        
        *p++ = ',';
        
        // 年 (s.ym >> 4) の文字列化
        auto res = std::to_chars(p, buf_end, s.ym >> 4);
        p = res.ptr;
        
        *p++ = '-';
        
        // 月 (s.ym & 0xF) 2桁固定・0埋め。LUTから2バイト一括コピー（分岐なし）
        const uint32_t month = s.ym & 0xF;
        __builtin_memcpy(p, lut_2digits[month].data(), 2);
        p += 2;
        
        *p++ = '=';
        
        // min_len
        res = std::to_chars(p, buf_end, s.min_len);
        p = res.ptr;
        
        *p++ = '/';
        
        // avg_len (C++23対応の超高速 std::to_chars 浮動小数点版)
        // 引数に fixed と精度 2 を指定。内部で Ryu / Dragonbox アルゴリズムが走り最速
        const double avg_len = static_cast<double>(s.sum_len) / s.message_count;
        res = std::to_chars(p, buf_end, avg_len, std::chars_format::fixed, 2);
        p = res.ptr;
        
        *p++ = '/';
        
        // max_len
        res = std::to_chars(p, buf_end, s.max_len);
        p = res.ptr;
        
        *p++ = '/';
        
        // message_count
        res = std::to_chars(p, buf_end, s.message_count);
        p = res.ptr;
        
        *p++ = '/';
        
        // total_stamp_count
        res = std::to_chars(p, buf_end, s.total_stamp_count);
        p = res.ptr;
        
        *p++ = '\n';
    }

    // 3. 最低レイヤのI/Oでファイルへ一括書き込み
    if (std::FILE* f = std::fopen(output_path.c_str(), "wb")) {
        std::fwrite(out_buf.data(), 1, p - out_buf.data(), f);
        std::fclose(f);
    }
}

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);

#ifdef LOCAL
    auto start = std::chrono::high_resolution_clock::now();
    auto now = start;
#endif

    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " input.csv output.txt\n";
        return 1;
    }

    const char* input_path = argv[1];
    const char* output_path = argv[2];

    // ファイル読み込み
    int fd = open(input_path, O_RDONLY);
    if (fd < 0) {
        cerr << "Failed to open input file: " << input_path << "\n";
        return 1;
    }

    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        cerr << "Failed to get file status: " << input_path << "\n";
        return 1;
    }
    size_t file_size = sb.st_size;

    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    // 分割箇所の計算
    int num_threads = thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    vector<size_t> chunk_starts(num_threads + 1, 0);
    chunk_starts[0] = 0;
    
    // ヘッダーをスキップ
    char tiny_buf[4096];
    ssize_t br = pread(fd, tiny_buf, sizeof(tiny_buf), 0);
    if (br > 0) {
        char* nl = static_cast<char*>(__builtin_memchr(tiny_buf, '\n', br));
        if (nl) {
            chunk_starts[0] = nl - tiny_buf + 1;
        }
    }

    // チャンク境界の計算
    for (int i = 1; i < num_threads; ++i) {
        size_t target = file_size * i / num_threads;
        while (target < file_size) {
            size_t read_size = min((size_t)4096, file_size - target);
            br = pread(fd, tiny_buf, read_size, target);
            if (br <= 0) break;

            char* nl = static_cast<char*>(__builtin_memchr(tiny_buf, '\n', br));
            if (nl) {
                chunk_starts[i] = target + (nl - tiny_buf) + 1;
                break;
            }
            target += br;
        }
    }
    chunk_starts[num_threads] = file_size;

#ifdef LOCAL
    now = std::chrono::high_resolution_clock::now();
    cerr << "Chunk starts calculated in " << std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() << " ms\n";
    start = now;
#endif

    // スレッドの起動
    vector<thread> threads;
    vector<WorkerResult> worker_results(num_threads);

    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED | POSIX_FADV_NOREUSE);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([fd, start = chunk_starts[i], end = chunk_starts[i + 1], &result = worker_results[i]]() {
            process_chunk(fd, start, end, result);
        });
    }

    for (auto& th : threads) {
        th.join();
    }

#ifdef LOCAL
    now = std::chrono::high_resolution_clock::now();
    cerr << "Chunks processed in " << std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() << " ms\n";
    start = now;
#endif

    // 結果の統合
    constexpr size_t GLOBAL_TABLE_SIZE = 1 << 18;
    constexpr size_t GLOBAL_TABLE_MASK = GLOBAL_TABLE_SIZE - 1;
    // vector<Stats> stats(GLOBAL_TABLE_SIZE);
    // vector<uint> indices;
    // indices.reserve(10000);
    SwissTable stats;

    for (const auto& result : worker_results) {
        for (uint idx : result.table.indices) {
            const Stats& s = result.table.table[idx];
            uint32_t h = s.hash;
            uint insert_idx = h & GLOBAL_TABLE_MASK;

            bool found = false;
            Stats& entry = *stats.get(s.channel_path, s.channel_path_len, s.ym, &found);

            if (found) {
                entry.merge(s);
            }
            else {
                entry.ym = s.ym;
                __builtin_memcpy(entry.channel_path, s.channel_path, s.channel_path_len);
                entry.channel_path[s.channel_path_len] = '\0';
                entry.channel_path_len = s.channel_path_len;
                entry.hash = h;
                entry.min_len = s.min_len;
                entry.max_len = s.max_len;
                entry.sum_len = s.sum_len;
                entry.message_count = s.message_count;
                entry.total_stamp_count = s.total_stamp_count;
            }

            // while (true) {
            //     if (stats[insert_idx].hash == h &&
            //         stats[insert_idx].ym == s.ym && 
            //         stats[insert_idx].channel_path_len == s.channel_path_len && 
            //         __builtin_memcmp(stats[insert_idx].channel_path, s.channel_path, s.channel_path_len) == 0) 
            //     {
            //         stats[insert_idx].merge(s);
            //         break;
            //     }

            //     if (stats[insert_idx].ym == 0) {
            //         __builtin_memcpy(stats[insert_idx].channel_path, s.channel_path, s.channel_path_len);
            //         stats[insert_idx].channel_path[s.channel_path_len] = '\0';
            //         stats[insert_idx].channel_path_len = s.channel_path_len;
            //         stats[insert_idx].hash = h;
            //         stats[insert_idx].ym = s.ym;
            //         stats[insert_idx].min_len = s.min_len;
            //         stats[insert_idx].max_len = s.max_len;
            //         stats[insert_idx].sum_len = s.sum_len;
            //         stats[insert_idx].message_count = s.message_count;
            //         stats[insert_idx].total_stamp_count = s.total_stamp_count;

            //         indices.push_back(insert_idx);
            //         break;
            //     }

            //     insert_idx = (insert_idx + 1) & GLOBAL_TABLE_MASK;
            // }
        }
    }

    close(fd);

#ifdef LOCAL
    now = std::chrono::high_resolution_clock::now();
    cerr << "Results merged in " << std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() << " ms\n";
    start = now;
#endif

    // 出力
    output_stats(output_path, stats.indices, stats.table);

#ifdef LOCAL
    now = std::chrono::high_resolution_clock::now();
    cerr << "Output written in " << std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() << " ms\n";
#endif

    return 0;
}