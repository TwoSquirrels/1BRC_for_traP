#pragma GCC optimize("Ofast")
#pragma GCC optimize("unroll-loops")
#include <bits/stdc++.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <bit>

using namespace std;
typedef long long ll;
typedef uint64_t ull;
inline uint parse_int(const char*& p) {
    uint res = 0;
    while (*p >= '0' && *p <= '9') {
        res = res * 10 + uint(*p - '0');
        p++;
    }
    return res;
}

// カンマ(0x2C)を8バイト分並べたマスク
constexpr uint64_t COMMA_MASK = 0x2C2C2C2C2C2C2C2Cull;
constexpr uint64_t SWAR_ADD = 0x0101010101010101ull;
constexpr uint64_t SWAR_XOR = 0x8080808080808080ull;

inline int parse_and_hash_channel(
    const char*& p,
    uint64_t& out_hash,
    std::string_view& out_str
) {
    const char* start = p;
    uint64_t h = 14695981039346656037ull; 

    while (true) {
        uint64_t block;
        std::memcpy(&block, p, 8); 

        uint64_t xor_block = block ^ COMMA_MASK;
        uint64_t has_comma = (xor_block - SWAR_ADD) & ~xor_block & SWAR_XOR;

        if (has_comma) {
            int comma_pos = __builtin_ctzll(has_comma) / 8;
            uint64_t mask = (1ULL << (comma_pos * 8)) - 1;
            uint64_t tail = block & mask;
            h = rotl(h, 5) ^ tail;
            p += comma_pos;
            break;
        } else {
            h = rotl(h, 5) ^ block;
            p += 8;
        }
    }
    
    out_hash = h;
    out_str = std::string_view(start, p - start);
    p++; 
    return 0; 
}

constexpr uint64_t str2u64(const char* s) {
    return (uint64_t(s[0]) << 56) | (uint64_t(s[1]) << 48) | (uint64_t(s[2]) << 40) | (uint64_t(s[3]) << 32) |
           (uint64_t(s[4]) << 24) | (uint64_t(s[5]) << 16) | (uint64_t(s[6]) << 8)  | uint64_t(s[7]);
}

struct Stat {
    uint cnt[12]{};
    uint min_len[12];
    uint max_len[12]{};
    uint total_len[12]{};
    uint stamp_sum[12]{};

    Stat() {
        fill(min_len, min_len + 12, UINT_MAX);
    }
};

struct Entry {
    ull hash;
    const char* ptr;
    uint16_t len;
    uint16_t id;
};

constexpr int HASH_SIZE = 1 << 16;
constexpr int HASH_MASK = HASH_SIZE - 1;

// スレッドごとの状態をまとめる構造体
struct ThreadState {
    alignas(64) Entry table[HASH_SIZE];
    vector<Stat> dat;
    vector<string_view> names;

    void init() {
        for (int i = 0; i < HASH_SIZE; i++)
            table[i].id = 0xFFFF;
    }

    inline uint16_t get_id(string_view s, ull h) {
        int pos = (h ^ (h >> 32)) & HASH_MASK;
        while (true) {
            Entry &e = table[pos];
            if (__builtin_expect(e.hash == h, 1)) {
                return e.id;
            }
            if (__builtin_expect(e.id == 0xFFFF, 0)) {
                uint16_t id = dat.size();
                e.hash = h;
                e.ptr = s.data();
                e.len = static_cast<uint16_t>(s.size());
                e.id = id;
                names.emplace_back(s);
                dat.emplace_back();
                return id;
            }
            pos = (pos + 1) & HASH_MASK;
        }
    }
};
#include <immintrin.h>

// 修正版
// 8個分を最初のレジスタにパッキング
__m512i month_thresholds_1 = _mm512_set_epi64(
    str2u64("18197568"), str2u64("18170784"), str2u64("18144000"), str2u64("18118080"),
    str2u64("18091296"), str2u64("18065376"), str2u64("18038592"), str2u64("18014400")
);

// 残り3個分を2つ目のレジスタにパッキング（使わない上位5レーンは ~0ULL で埋める）
__m512i month_thresholds_2 = _mm512_set_epi64(
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    str2u64("18276192"), str2u64("18250272"), str2u64("18223488")
);

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " input.csv output.txt\n";
        return 1;
    }
    const char* input_path = argv[1];
    const char* output_path = argv[2];
	
    int fd = open(input_path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        close(fd); return 1;
    }

    size_t length = sb.st_size;
    const char* mapped = (const char*)mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd); return 1;
    }

    // 8スレッドで並列処理
    const int num_threads = 8;
    vector<ThreadState> states(num_threads);
    vector<thread> threads;
for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&](int tid) {
            states[tid].init();

            // 各スレッドの「行頭と一致する確実な開始位置」を計算する関数
            auto get_start_pos = [&](int t) -> const char* {
                if (t == 0) {
                    const char* s = mapped;
                    // ヘッダー行をスキップ
                    while (s < mapped + length && *s != '\n') s++;
                    if (s < mapped + length) s++;
                    return s;
                }
                if (t == num_threads) return mapped + length;
                
                const char* s = mapped + t * (length / num_threads);
                // 境界の1つ前の文字が改行(\n)でない場合、行の途中で切断されているため次の行頭まで進める
                if (s[-1] != '\n') {
                    while (s < mapped + length && *s != '\n') s++;
                    if (s < mapped + length) s++;
                }
                return s;
            };

            const char* t_start = get_start_pos(tid);
            const char* t_end = get_start_pos(tid + 1); // 次のスレッドの開始位置を自分の終了位置とする
            
            const char* p = t_start;

            // p が ピッタリ t_end に到達するまでパース（途中で止まらない）
            while (p < t_end) {
                uint64_t ts_raw;
                std::memcpy(&ts_raw, p, 8);
                uint64_t ts = __builtin_bswap64(ts_raw);
                
                __m512i v_ts = _mm512_set1_epi64(ts);
                __mmask8 mask1 = _mm512_cmpge_epu64_mask(v_ts, month_thresholds_1);
                __mmask8 mask2 = _mm512_cmpge_epu64_mask(v_ts, month_thresholds_2);

                // スカラ加算をなくし、マスクのpopcntのみで完結
                int month = _mm_popcnt_u32(mask1) + _mm_popcnt_u32(mask2);
                
                p += 11;

                ull channel_hash;
                string_view channel_path;
                parse_and_hash_channel(p, channel_hash, channel_path);
                
                uint message_length = *p++ - '0';
                while (*p >= '0') {
                    message_length = message_length * 10 + (*p++ - '0');
                }
                p++;

                uint stamp_count = *p++ - '0';
                while (*p >= '0') {
                    stamp_count = stamp_count * 10 + (*p++ - '0');
                }
                p++;
                
                uint16_t id = states[tid].get_id(channel_path, channel_hash);

                Stat& st = states[tid].dat[id];
                st.cnt[month]++;
                st.total_len[month] += message_length;
                if (message_length > st.max_len[month]) st.max_len[month] = message_length;
                if (message_length < st.min_len[month]) st.min_len[month] = message_length;
                st.stamp_sum[month] += stamp_count;           }
        }, i);
    }

    // 全スレッドの終了を待機
    for (auto& t : threads) {
        t.join();
    }

    // --- ここからマージ処理 ---
    unordered_map<string_view, int> global_name_to_id;
    vector<Stat> global_dat;
    vector<string_view> global_names;

    for (int tid = 0; tid < num_threads; tid++) {
        for (size_t i = 0; i < states[tid].names.size(); i++) {
            string_view name = states[tid].names[i];
            const Stat& local_s = states[tid].dat[i];
            
            auto it = global_name_to_id.find(name);
            int gid;
            if (it == global_name_to_id.end()) {
                gid = global_dat.size();
                global_name_to_id[name] = gid;
                global_names.push_back(name);
                global_dat.emplace_back();
            } else {
                gid = it->second;
            }

            Stat& gs = global_dat[gid];
            for (int m = 0; m < 12; m++) {
                if (local_s.cnt[m] > 0) {
                    gs.cnt[m] += local_s.cnt[m];
                    gs.total_len[m] += local_s.total_len[m];
                    gs.stamp_sum[m] += local_s.stamp_sum[m];
                    if (local_s.min_len[m] < gs.min_len[m]) gs.min_len[m] = local_s.min_len[m];
                    if (local_s.max_len[m] > gs.max_len[m]) gs.max_len[m] = local_s.max_len[m];
                }
            }
        }
    }
    // --- 出力処理 ---
    ofstream fout(output_path);
    if (!fout) return 1;

    vector<int> ord(global_names.size());
    iota(ord.begin(), ord.end(), 0);
    sort(ord.begin(), ord.end(), [&](int a, int b) {
        return global_names[a] < global_names[b];
    });
    
    fout << fixed << setprecision(2);

    string buffer;
    buffer.reserve(1024 * 1024 * 64); // 64MB程度確保

    // double の挙動を完璧に模倣するため、出力時のみ double キャストを使用
    // 整数演算の丸め誤差から完全に解放されます
    for (int idx : ord) {
        string_view &name = global_names[idx];
        const Stat &s = global_dat[idx];
        for (int month = 0; month < 12; month++) {
            if (s.cnt[month] == 0) continue;
        
            buffer += name;
            buffer += ',';
            buffer += "2027-";
            if (month + 1 < 10) buffer += '0';
            buffer += to_string(month + 1);
            buffer += '=';
            
            buffer += to_string(s.min_len[month]) + '/';

            // double キャスト出力により diff を完全に解消しつつ、出力速度を最大化
            char out_buf[32];
            int len = snprintf(out_buf, sizeof(out_buf), "%.2f/", (double)s.total_len[month] / s.cnt[month]);
            buffer.append(out_buf, len);

            buffer += to_string(s.max_len[month]) + '/';
            buffer += to_string(s.cnt[month]) + '/';
            buffer += to_string(s.stamp_sum[month]) + '\n';
        }
    }

    // 最後に一括で書き出し
    FILE* fp = fopen(output_path, "wb");
    fwrite(buffer.data(), 1, buffer.size(), fp);
    fclose(fp);

    munmap((void*)mapped, length);
    close(fd);
    return 0;
}