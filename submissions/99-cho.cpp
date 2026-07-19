#include <fcntl.h>
#include <immintrin.h>
#include <wmmintrin.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <bits/stdc++.h>

using namespace std;

// 2桁の数字テーブル ("00"～"99")
static constexpr char digit_pairs[200] = {
    '0','0','0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8','0','9',
    '1','0','1','1','1','2','1','3','1','4','1','5','1','6','1','7','1','8','1','9',
    '2','0','2','1','2','2','2','3','2','4','2','5','2','6','2','7','2','8','2','9',
    '3','0','3','1','3','2','3','3','3','4','3','5','3','6','3','7','3','8','3','9',
    '4','0','4','1','4','2','4','3','4','4','4','5','4','6','4','7','4','8','4','9',
    '5','0','5','1','5','2','5','3','5','4','5','5','5','6','5','7','5','8','5','9',
    '6','0','6','1','6','2','6','3','6','4','6','5','6','6','6','7','6','8','6','9',
    '7','0','7','1','7','2','7','3','7','4','7','5','7','6','7','7','7','8','7','9',
    '8','0','8','1','8','2','8','3','8','4','8','5','8','6','8','7','8','8','8','9',
    '9','0','9','1','9','2','9','3','9','4','9','5','9','6','9','7','9','8','9','9',
};

struct FastIO {
    int fd;
    static constexpr size_t BUF_SIZE = 8 * 1024 * 1024;  // 8MB - 出力全体が入る
    char buf[BUF_SIZE];
    size_t pos = 0;
    
    FastIO(int fd) : fd(fd) {}
    ~FastIO() { flush(); }
    
    inline void flush() {
        if (pos > 0) {
            size_t written = 0;
            while (written < pos) {
                ssize_t w = ::write(fd, buf + written, pos - written);
                if (w <= 0) break;
                written += w;
            }
            pos = 0;
        }
    }

    inline void write_str(span<const char> s) {
        memcpy(buf + pos, s.data(), s.size());
        pos += s.size();
    }
    
    inline void write_str2(const char* s) {
        size_t len = strlen(s);
        memcpy(buf + pos, s, len);
        pos += len;
    }

    inline void write_char(char c) {
        buf[pos++] = c;
    }

    inline void write_u32(uint32_t x) {
        if (x < 10) {
            buf[pos++] = '0' + x;
            return;
        } else if (x < 100) {
            memcpy(buf + pos, digit_pairs + x * 2, 2);
            pos += 2;
            return;
        }
        char tmp[12];
        char* p = tmp + 12;
        while (x >= 100) {
            uint32_t r = x % 100;
            x /= 100;
            p -= 2;
            memcpy(p, digit_pairs + r * 2, 2);
        }
        if (x >= 10) {
            p -= 2;
            memcpy(p, digit_pairs + x * 2, 2);
        } else {
            *--p = '0' + x;
        }
        size_t len = (tmp + 12) - p;
        memcpy(buf + pos, p, len);
        pos += len;
    }
    
    inline void write_u32_2(uint32_t x) {
        memcpy(buf + pos, digit_pairs + x * 2, 2);
        pos += 2;
    }
    
    inline void write_double_fixed2(double x) {
        // Average is [1.00, 32.00], so it easily fits in integer logic.
        // We can just use IEEE double nearest rounding by adding 0.005?
        // Wait, standard double rounding is half-to-even.
        // The numbers are sum / count, which won't hit exactly .XX5 often, but if they do we must round to even.
        // std::to_chars handles all IEEE rounding perfectly. We'll use to_chars for correctness but avoid it if it's too slow.
        // Let's stick to to_chars for double since 3.6ms is negligible, but optimize the string output.
        auto [ptr, ec] = std::to_chars(buf + pos, buf + pos + 32, x, std::chars_format::fixed, 2);
        pos = ptr - buf;
    }
};





struct FastMonthTable {
    uint8_t table[366] = {};
    constexpr FastMonthTable() {
        for (uint32_t doy = 0; doy < 366; doy++) {
            uint8_t m = 0;
            if (doy < 31) m = 1;
            else if (doy < 59) m = 2;
            else if (doy < 90) m = 3;
            else if (doy < 120) m = 4;
            else if (doy < 151) m = 5;
            else if (doy < 181) m = 6;
            else if (doy < 212) m = 7;
            else if (doy < 243) m = 8;
            else if (doy < 273) m = 9;
            else if (doy < 304) m = 10;
            else if (doy < 334) m = 11;
            else m = 12;
            table[doy] = m;
        }
    }
};

constexpr FastMonthTable FAST_MONTH;

struct Stats {
    uint32_t count = 0;
    uint32_t min_len = 0xFFFFFFFF;
    uint32_t max_len = 0;
    uint32_t sum_len = 0;
    uint32_t stamp_sum = 0;
    
    inline void add(uint32_t m_len, uint32_t s_count) {
        min_len = m_len < min_len ? m_len : min_len;
        max_len = m_len > max_len ? m_len : max_len;
        sum_len += m_len;
        stamp_sum += s_count;
        count++;
    }
};

struct ChannelMap {
    static constexpr size_t CAP = 16384;  // 128KB: L2キャッシュに余裕で収まる
    alignas(64) uint64_t hashes[CAP] = {};
    alignas(64) uint32_t ids[CAP] = {};
    uint32_t size = 0;
    span<const char> id_to_key[CAP];

    alignas(32) static constexpr uint8_t hash_mask_array[64] = {
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };

    inline uint64_t fast_hash(span<const char> s) {
        size_t len = s.size();
        __m256i v = _mm256_loadu_si256((const __m256i*)s.data());
        __m256i mask = _mm256_loadu_si256((const __m256i*)(hash_mask_array + 32 - len));
        v = _mm256_and_si256(v, mask);

        // 256bit → 128bit XOR fold
        __m128i lo = _mm256_castsi256_si128(v);
        __m128i hi = _mm256_extracti128_si256(v, 1);
        __m128i x  = _mm_xor_si128(lo, hi);

        // Parallel execution: two independent 64x64->128 multiplications (mulq)
        uint64_t a = (uint64_t)_mm_cvtsi128_si64(x) ^ (uint64_t)len;
        uint64_t b = (uint64_t)_mm_extract_epi64(x, 1);
        __uint128_t r = (__uint128_t)(a ^ 0x9e3779b97f4a7c15ULL)
                      * ((__uint128_t)(b ^ 0xe7037ed1a0b428dbULL) | 1ULL);
        uint64_t h = (uint64_t)r ^ (uint64_t)(r >> 64);
        return h ? h : 1;
    }

    uint32_t get_or_add_with_hash(span<const char> s, uint64_t h) {
        if (h == 0) h = 1;
        uint32_t idx = h & (CAP - 1);
        while (true) {
            uint64_t slot = hashes[idx];
            if (slot == 0) { 
                hashes[idx] = h;
                ids[idx] = ++size; 
                id_to_key[size] = s;
                return size;
            }
            if (slot == h) { 
                return ids[idx];
            }
            idx = (idx + 1) & (CAP - 1);
        }
    }

    uint32_t get_or_add(span<const char> s) {
        return get_or_add_with_hash(s, fast_hash(s));
    }
};

struct WorkerResult {
    ChannelMap cmap;
    Stats stats[120020];
};

constexpr size_t thread_count = 8;
WorkerResult* global_results;

void process_chunk(const char* p, const char* end, WorkerResult* res) {
    bool has_prev = false;
    uint32_t prev_s_idx1 = 0, prev_s_idx2 = 0;
    uint32_t prev_msg_len1 = 0, prev_stamp_count1 = 0;
    uint32_t prev_msg_len2 = 0, prev_stamp_count2 = 0;

    auto flush_pipeline = [&]() {
        if (has_prev) {
            res->stats[prev_s_idx1].add(prev_msg_len1, prev_stamp_count1);
            res->stats[prev_s_idx2].add(prev_msg_len2, prev_stamp_count2);
            has_prev = false;
        }
    };

    while (p + 128 < end) {
        // --- Line 1 ---
        __m128i v1 = _mm_loadl_epi64((const __m128i*)p);
        v1 = _mm_and_si128(v1, _mm_set1_epi8(0x0F));
        v1 = _mm_maddubs_epi16(v1, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 0, 0, 0, 0, 0, 0, 0, 0));
        v1 = _mm_madd_epi16(v1, _mm_setr_epi16(100, 1, 100, 1, 0, 0, 0, 0));
        v1 = _mm_packus_epi32(v1, v1);
        uint32_t val1 = _mm_extract_epi16(v1, 0) * 10000 + _mm_extract_epi16(v1, 1);
        uint32_t month1 = FAST_MONTH.table[(val1 / 864) - 20819];
        
        const char* c_start1 = p + 11;
        __m256i v_data1 = _mm256_loadu_si256((const __m256i*)c_start1);
        uint32_t c_mask1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_data1, _mm256_set1_epi8(',')));
        uint32_t n_mask1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_data1, _mm256_set1_epi8('\n')));
        
        if (__builtin_expect(n_mask1 != 0 && c_mask1 != 0, 1)) {
            uint32_t c_idx1 = __builtin_ctz(c_mask1);
            uint32_t n_idx1 = __builtin_ctz(n_mask1);
            if (__builtin_expect(n_idx1 > c_idx1, 1)) {
                // --- Line 2 ---
                const char* p2 = c_start1 + n_idx1 + 1;
                __m128i v2 = _mm_loadl_epi64((const __m128i*)p2);
                v2 = _mm_and_si128(v2, _mm_set1_epi8(0x0F));
                v2 = _mm_maddubs_epi16(v2, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 0, 0, 0, 0, 0, 0, 0, 0));
                v2 = _mm_madd_epi16(v2, _mm_setr_epi16(100, 1, 100, 1, 0, 0, 0, 0));
                v2 = _mm_packus_epi32(v2, v2);
                uint32_t val2 = _mm_extract_epi16(v2, 0) * 10000 + _mm_extract_epi16(v2, 1);
                uint32_t month2 = FAST_MONTH.table[(val2 / 864) - 20819];
                
                const char* c_start2 = p2 + 11;
                __m256i v_data2 = _mm256_loadu_si256((const __m256i*)c_start2);
                uint32_t c_mask2 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_data2, _mm256_set1_epi8(',')));
                uint32_t n_mask2 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_data2, _mm256_set1_epi8('\n')));
                
                if (__builtin_expect(n_mask2 != 0 && c_mask2 != 0, 1)) {
                    uint32_t c_idx2 = __builtin_ctz(c_mask2);
                    uint32_t n_idx2 = __builtin_ctz(n_mask2);
                    if (__builtin_expect(n_idx2 > c_idx2, 1)) {
                        // Parallel Hashing
                        span<const char> ch1(c_start1, c_idx1);
                        span<const char> ch2(c_start2, c_idx2);
                        
                        uint64_t h1 = res->cmap.fast_hash(ch1);
                        uint64_t h2 = res->cmap.fast_hash(ch2);
                        
                        uint32_t id1 = res->cmap.get_or_add_with_hash(ch1, h1);
                        uint32_t id2 = res->cmap.get_or_add_with_hash(ch2, h2);
                        
                        uint32_t s_idx1 = id1 * 12 + month1 - 1;
                        uint32_t s_idx2 = id2 * 12 + month2 - 1;
                        
                        __builtin_prefetch(&res->stats[s_idx1], 1, 3);
                        __builtin_prefetch(&res->stats[s_idx2], 1, 3);
                        
                        // Parse Ints 1
                        const char* num_p1 = c_start1 + c_idx1 + 1;
                        uint32_t msg_len1 = 0;
                        if (num_p1[1] == ',') { msg_len1 = num_p1[0] & 0x0F; num_p1 += 2; }
                        else if (num_p1[2] == ',') { msg_len1 = (num_p1[0] & 0x0F) * 10 + (num_p1[1] & 0x0F); num_p1 += 3; }
                        else if (num_p1[3] == ',') { msg_len1 = (num_p1[0] & 0x0F) * 100 + (num_p1[1] & 0x0F) * 10 + (num_p1[2] & 0x0F); num_p1 += 4; }
                        else { while (*num_p1 != ',') { msg_len1 = msg_len1 * 10 + (*num_p1 & 0x0F); num_p1++; } num_p1++; }
                        uint32_t stamp_count1 = 0;
                        if (num_p1[1] == '\n') { stamp_count1 = num_p1[0] & 0x0F; }
                        else if (num_p1[2] == '\n') { stamp_count1 = (num_p1[0] & 0x0F) * 10 + (num_p1[1] & 0x0F); }
                        else if (num_p1[3] == '\n') { stamp_count1 = (num_p1[0] & 0x0F) * 100 + (num_p1[1] & 0x0F) * 10 + (num_p1[2] & 0x0F); }
                        else { while (*num_p1 != '\n') { stamp_count1 = stamp_count1 * 10 + (*num_p1 & 0x0F); num_p1++; } }
                        
                        // Parse Ints 2
                        const char* num_p2 = c_start2 + c_idx2 + 1;
                        uint32_t msg_len2 = 0;
                        if (num_p2[1] == ',') { msg_len2 = num_p2[0] & 0x0F; num_p2 += 2; }
                        else if (num_p2[2] == ',') { msg_len2 = (num_p2[0] & 0x0F) * 10 + (num_p2[1] & 0x0F); num_p2 += 3; }
                        else if (num_p2[3] == ',') { msg_len2 = (num_p2[0] & 0x0F) * 100 + (num_p2[1] & 0x0F) * 10 + (num_p2[2] & 0x0F); num_p2 += 4; }
                        else { while (*num_p2 != ',') { msg_len2 = msg_len2 * 10 + (*num_p2 & 0x0F); num_p2++; } num_p2++; }
                        uint32_t stamp_count2 = 0;
                        if (num_p2[1] == '\n') { stamp_count2 = num_p2[0] & 0x0F; }
                        else if (num_p2[2] == '\n') { stamp_count2 = (num_p2[0] & 0x0F) * 10 + (num_p2[1] & 0x0F); }
                        else if (num_p2[3] == '\n') { stamp_count2 = (num_p2[0] & 0x0F) * 100 + (num_p2[1] & 0x0F) * 10 + (num_p2[2] & 0x0F); }
                        else { while (*num_p2 != '\n') { stamp_count2 = stamp_count2 * 10 + (*num_p2 & 0x0F); num_p2++; } }
                        
                        if (has_prev) {
                            res->stats[prev_s_idx1].add(prev_msg_len1, prev_stamp_count1);
                            res->stats[prev_s_idx2].add(prev_msg_len2, prev_stamp_count2);
                        }
                        
                        prev_s_idx1 = s_idx1;
                        prev_msg_len1 = msg_len1;
                        prev_stamp_count1 = stamp_count1;
                        
                        prev_s_idx2 = s_idx2;
                        prev_msg_len2 = msg_len2;
                        prev_stamp_count2 = stamp_count2;
                        has_prev = true;
                        
                        p = c_start2 + n_idx2 + 1;
                        continue;
                    }
                }
            }
        }
        
        flush_pipeline();
        
        // Single row fallback
        __m128i v = _mm_loadl_epi64((const __m128i*)p);
        v = _mm_and_si128(v, _mm_set1_epi8(0x0F));
        v = _mm_maddubs_epi16(v, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 0, 0, 0, 0, 0, 0, 0, 0));
        v = _mm_madd_epi16(v, _mm_setr_epi16(100, 1, 100, 1, 0, 0, 0, 0));
        v = _mm_packus_epi32(v, v);
        uint32_t val = _mm_extract_epi16(v, 0) * 10000 + _mm_extract_epi16(v, 1);
        
        uint32_t month = FAST_MONTH.table[(val / 864) - 20819];
        p += 11;

        const char* channel_begin = p;
#if defined(__AVX512BW__)
        if (__builtin_expect(end - p >= 64, 1)) {
            __m512i v_data = _mm512_loadu_si512((const void*)p);
            uint64_t mask = _mm512_cmpeq_epi8_mask(v_data, _mm512_set1_epi8(','));
            if (__builtin_expect(mask != 0, 1)) {
                p += __builtin_ctzll(mask);
            } else {
                p = (const char*)memchr(p, ',', end - p);
            }
        } else {
            p = (const char*)memchr(p, ',', end - p);
        }
#else
        if (__builtin_expect(end - p >= 32, 1)) {
            __m256i v_data = _mm256_loadu_si256((const __m256i*)p);
            uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_data, _mm256_set1_epi8(',')));
            if (__builtin_expect(mask != 0, 1)) {
                p += __builtin_ctz(mask);
            } else {
                p = (const char*)memchr(p, ',', end - p);
            }
        } else {
            p = (const char*)memchr(p, ',', end - p);
        }
#endif
        span<const char> channel(channel_begin, p - channel_begin);
        ++p;

        uint32_t channel_id = res->cmap.get_or_add(channel);
        uint32_t stats_idx = channel_id * 12 + month - 1;
        __builtin_prefetch(&res->stats[stats_idx], 1, 3); // Statsを整数パース中にL1へ先読み

        uint32_t msg_len = 0;
        if (p[1] == ',') {
            msg_len = p[0] & 0x0F;
            p += 2;
        } else if (p[2] == ',') {
            msg_len = (p[0] & 0x0F) * 10 + (p[1] & 0x0F);
            p += 3;
        } else if (p[3] == ',') {
            msg_len = (p[0] & 0x0F) * 100 + (p[1] & 0x0F) * 10 + (p[2] & 0x0F);
            p += 4;
        } else if (p[4] == ',') {
            msg_len = (p[0] & 0x0F) * 1000 + (p[1] & 0x0F) * 100 + (p[2] & 0x0F) * 10 + (p[3] & 0x0F);
            p += 5;
        } else if (p[5] == ',') {
            msg_len = (p[0] & 0x0F) * 10000 + (p[1] & 0x0F) * 1000 + (p[2] & 0x0F) * 100 + (p[3] & 0x0F) * 10 + (p[4] & 0x0F);
            p += 6;
        } else {
            while (*p != ',') {
                msg_len = msg_len * 10 + (*p & 0x0F);
                ++p;
            }
            ++p;
        }

        uint32_t stamp_count = 0;
        if (p[1] == '\n') {
            stamp_count = p[0] & 0x0F;
            p += 2;
        } else if (p[2] == '\n') {
            stamp_count = (p[0] & 0x0F) * 10 + (p[1] & 0x0F);
            p += 3;
        } else if (p[3] == '\n') {
            stamp_count = (p[0] & 0x0F) * 100 + (p[1] & 0x0F) * 10 + (p[2] & 0x0F);
            p += 4;
        } else if (p[4] == '\n') {
            stamp_count = (p[0] & 0x0F) * 1000 + (p[1] & 0x0F) * 100 + (p[2] & 0x0F) * 10 + (p[3] & 0x0F);
            p += 5;
        } else if (p[5] == '\n') {
            stamp_count = (p[0] & 0x0F) * 10000 + (p[1] & 0x0F) * 1000 + (p[2] & 0x0F) * 100 + (p[3] & 0x0F) * 10 + (p[4] & 0x0F);
            p += 6;
        } else {
            while (*p != '\n') {
                stamp_count = stamp_count * 10 + (*p & 0x0F);
                ++p;
            }
            ++p;
        }

        res->stats[stats_idx].add(msg_len, stamp_count);
    }

    flush_pipeline();

    while (p < end) {
        __m128i v = _mm_loadl_epi64((const __m128i*)p);
        v = _mm_and_si128(v, _mm_set1_epi8(0x0F));
        v = _mm_maddubs_epi16(v, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 0, 0, 0, 0, 0, 0, 0, 0));
        v = _mm_madd_epi16(v, _mm_setr_epi16(100, 1, 100, 1, 0, 0, 0, 0));
        v = _mm_packus_epi32(v, v);
        uint32_t val = _mm_extract_epi16(v, 0) * 10000 + _mm_extract_epi16(v, 1);
        
        uint32_t month = FAST_MONTH.table[(val / 864) - 20819];
        p += 11;

        const char* channel_begin = p;
#if defined(__AVX512BW__)
        if (__builtin_expect(end - p >= 64, 1)) {
            __m512i v_data = _mm512_loadu_si512((const void*)p);
            uint64_t mask = _mm512_cmpeq_epi8_mask(v_data, _mm512_set1_epi8(','));
            if (__builtin_expect(mask != 0, 1)) {
                p += __builtin_ctzll(mask);
            } else {
                p = (const char*)memchr(p, ',', end - p);
            }
        } else {
            p = (const char*)memchr(p, ',', end - p);
        }
#else
        if (__builtin_expect(end - p >= 32, 1)) {
            __m256i v_data = _mm256_loadu_si256((const __m256i*)p);
            uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_data, _mm256_set1_epi8(',')));
            if (__builtin_expect(mask != 0, 1)) {
                p += __builtin_ctz(mask);
            } else {
                p = (const char*)memchr(p, ',', end - p);
            }
        } else {
            p = (const char*)memchr(p, ',', end - p);
        }
#endif
        span<const char> channel(channel_begin, p - channel_begin);
        ++p;

        uint32_t channel_id = res->cmap.get_or_add(channel);
        uint32_t stats_idx = channel_id * 12 + month - 1;
        __builtin_prefetch(&res->stats[stats_idx], 1, 3); // Statsを整数パース中にL1へ先読み

        uint32_t msg_len = 0;
        if (p[1] == ',') {
            msg_len = p[0] & 0x0F;
            p += 2;
        } else if (p[2] == ',') {
            msg_len = (p[0] & 0x0F) * 10 + (p[1] & 0x0F);
            p += 3;
        } else if (p[3] == ',') {
            msg_len = (p[0] & 0x0F) * 100 + (p[1] & 0x0F) * 10 + (p[2] & 0x0F);
            p += 4;
        } else if (p[4] == ',') {
            msg_len = (p[0] & 0x0F) * 1000 + (p[1] & 0x0F) * 100 + (p[2] & 0x0F) * 10 + (p[3] & 0x0F);
            p += 5;
        } else if (p[5] == ',') {
            msg_len = (p[0] & 0x0F) * 10000 + (p[1] & 0x0F) * 1000 + (p[2] & 0x0F) * 100 + (p[3] & 0x0F) * 10 + (p[4] & 0x0F);
            p += 6;
        } else {
            while (*p != ',') {
                msg_len = msg_len * 10 + (*p & 0x0F);
                ++p;
            }
            ++p;
        }

        uint32_t stamp_count = 0;
        if (p[1] == '\n') {
            stamp_count = p[0] & 0x0F;
            p += 2;
        } else if (p[2] == '\n') {
            stamp_count = (p[0] & 0x0F) * 10 + (p[1] & 0x0F);
            p += 3;
        } else if (p[3] == '\n') {
            stamp_count = (p[0] & 0x0F) * 100 + (p[1] & 0x0F) * 10 + (p[2] & 0x0F);
            p += 4;
        } else if (p[4] == '\n') {
            stamp_count = (p[0] & 0x0F) * 1000 + (p[1] & 0x0F) * 100 + (p[2] & 0x0F) * 10 + (p[3] & 0x0F);
            p += 5;
        } else if (p[5] == '\n') {
            stamp_count = (p[0] & 0x0F) * 10000 + (p[1] & 0x0F) * 1000 + (p[2] & 0x0F) * 100 + (p[3] & 0x0F) * 10 + (p[4] & 0x0F);
            p += 6;
        } else {
            while (*p != '\n') {
                stamp_count = stamp_count * 10 + (*p & 0x0F);
                ++p;
            }
            ++p;
        }

        res->stats[stats_idx].add(msg_len, stamp_count);
    }
}



int main(int argc, char** argv) {
    global_results = new WorkerResult[thread_count];
    auto t0 = chrono::high_resolution_clock::now();

    if (argc < 3) return 1;
    const char* input_path = argv[1];
    const char* output_path = argv[2];

    const int fd = open(input_path, O_RDONLY);
    assert(fd >= 0);

    struct stat st{};
    assert(fstat(fd, &st) == 0);
    const size_t file_size = st.st_size;

    assert(file_size > 55);

    const char* file_data = (const char*)mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    assert(file_data != MAP_FAILED);
    madvise((void*)file_data, file_size, MADV_SEQUENTIAL);
    madvise((void*)file_data, file_size, MADV_HUGEPAGE);

    const char* start_ptr = file_data + 55;
    assert(*(start_ptr-1) == '\n');
    
    vector<const char*> boundaries;
    boundaries.push_back(start_ptr);
    size_t data_size = (file_data + file_size) - start_ptr;
    size_t chunk_size = data_size / thread_count;
    for (size_t i = 1; i < thread_count; ++i) {
        const char* p = start_ptr + i * chunk_size;
        while (*p != '\n') {
            ++p;
        }
        ++p;
        boundaries.push_back(p);
    }
    boundaries.push_back(file_data + file_size);

    auto t1 = chrono::high_resolution_clock::now();

    vector<thread> workers;
    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back(process_chunk, boundaries[i], boundaries[i+1], &global_results[i]);
    }

    for (auto& w : workers) {
        w.join();
    }

    auto t2 = chrono::high_resolution_clock::now();

    static ChannelMap global_cmap;
    static Stats global_stats[120020];
    for (size_t i = 0; i < thread_count; ++i) {
        for (uint32_t cid = 1; cid <= global_results[i].cmap.size; ++cid) {
            span<const char> cname = global_results[i].cmap.id_to_key[cid];
            uint32_t global_cid = global_cmap.get_or_add(cname);
            for (uint32_t m = 1; m <= 12; ++m) {
                uint32_t local_idx = cid * 12 + m - 1;
                const Stats& local_st = global_results[i].stats[local_idx];
                if (local_st.count == 0) continue;
                
                uint32_t global_idx = global_cid * 12 + m - 1;
                Stats& global_st = global_stats[global_idx];
                if (local_st.min_len < global_st.min_len) global_st.min_len = local_st.min_len;
                if (local_st.max_len > global_st.max_len) global_st.max_len = local_st.max_len;
                global_st.sum_len += local_st.sum_len;
                global_st.count += local_st.count;
                global_st.stamp_sum += local_st.stamp_sum;
            }
        }
    }
    auto t3 = chrono::high_resolution_clock::now();

    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    {
        auto io_ptr = std::make_unique<FastIO>(out_fd);
        FastIO& io = *io_ptr;
        for (uint32_t cid = 1; cid <= global_cmap.size; ++cid) {
            span<const char> cname = global_cmap.id_to_key[cid];
            for (uint32_t m = 1; m <= 12; ++m) {
                uint32_t global_idx = cid * 12 + m - 1;
                const Stats& st = global_stats[global_idx];
                if (st.count == 0) continue;
                
                double average = (double)st.sum_len / st.count;
                io.write_str(cname);
                io.write_str2(",2027-");
                io.write_u32_2(m);
                io.write_char('=');
                io.write_u32(st.min_len);
                io.write_char('/');
                io.write_double_fixed2(average);
                io.write_char('/');
                io.write_u32(st.max_len);
                io.write_char('/');
                io.write_u32(st.count);
                io.write_char('/');
                io.write_u32(st.stamp_sum);
                io.write_char('\n');
            }
        }
    }
    close(out_fd);
    munmap((void*)file_data, file_size);
    close(fd);

    auto t4 = chrono::high_resolution_clock::now();

    fprintf(stderr, "[Phase 1] Mmap & Init : %.3f ms\n", chrono::duration<double, milli>(t1 - t0).count());
    fprintf(stderr, "[Phase 2] Multi-thread: %.3f ms\n", chrono::duration<double, milli>(t2 - t1).count());
    fprintf(stderr, "[Phase 3] Merge global: %.3f ms\n", chrono::duration<double, milli>(t3 - t2).count());
    fprintf(stderr, "[Phase 4] Write output: %.3f ms\n", chrono::duration<double, milli>(t4 - t3).count());
    return 0;
}
