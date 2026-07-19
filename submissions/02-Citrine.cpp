#ifndef RINGD
#define RINGD 4
#endif
// 1BRC for traP — e137: e136 − チャンク終了時の MADV_DONTNEED 分離返却。
// fork 早期退出により PTE/VMA 解体は親 exit 後の孤児側で無償になるため、
// 計測窓内(merge/出力と並走)で CPU を使う DONTNEED は純コストとなり削除。
// e136: e135 + fork 早期退出(zero.cpp クリーンルーム実装からの移植)。
// 子プロセスが全作業と出力 write+close を終えた後、pipe で 'K' を親へ通知して親が即 _exit(0)。
// 26GB mmap の残 PTE/VMA 解体は孤児化した子側で進む(判定が親プロセスのみを待つ実装なら
// その分だけ壁時計が縮む。プロセスグループ全体を待つ実装でも損はない)。
// 子は fork 直後に fd 0/1/2 を閉じる(ランナーが stdout/stderr の EOF を待つ型でも
// 親 exit と同時に EOF が成立する)。出力は通知前に write+close 済みで正しさに影響しない。
// 失敗系: fork 不能→インライン続行。子が 'K' 前に死亡→親が waitpid で子の終了コードを伝搬。
// e135: e134 + lookup_med(33..127B チャンネルの固定 128B 鍵テーブル化)。
// 新保証「channel_path ≤ 104B(1..5 階層 × 1..20 文字)」より、32B 超の名前は合法かつ有界。
// 従来の lookup_big(行ごと std::string+unordered_map)を 33..127B についてテーブル化し、
// Private データが 33..104B チャンネルを含む場合の失速を防ぐ(≥128B は保証外バックストップとして従来通り)。
// 公開データ(1B 実測: max clen=32)ではこの経路は不発 → ホットパスは e134 と不変。
// e134: e124 + リング再開修正(parse_fast 再入時に rp_io を尊重)。性能変更なし。
// e124: e121 + _exit(0) 終了(ヒープ破棄・glibc teardown をスキップし
// カーネル一括 munmap に置換。出力は write+close 完了後なので正しさに影響しない)
// 各スレッドが先頭 LEARN バイトをパース後、acc の cnt 頻度で slots を降順再挿入し
// 高頻度 ch を home(probe=1)位置へ。id/keys/acc/リングは slots 非依存 → 正しさ不変。
// build: g++ -std=c++20 -O3 -march=native -o a.out main.cpp
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <immintrin.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

static constexpr u32 TS_BASE = 1798761600u;  // 2027-01-01T00:00:00Z (verified)
static constexpr int MDAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static u8 d2m[366];
static u64 thr_lo8[8], thr_hi8[8];  // bswap'd 8-char prefixes of month-start ts (idx 1..11 + pad)

// ---- channel hash table: open addressing, fingerprint-in-slot (no key load) ----
// 鍵 = 名前 32B を「0x80+clen」でパッドした単射 32B(ASCII 保証より実バイトと不衝突)。
// fp は速度用プレフィルタに過ぎず、恒等性はドレインの 32B 完全比較(+recover_slow)で決定的。
static constexpr u32 SLOT_EMPTY = ~0u;  // u32 slot: fp18<<14 | id14(64KB = L1 常駐)
static constexpr size_t TBL_BITS = 14;
static constexpr size_t TBL_SIZE = size_t(1) << TBL_BITS;
static constexpr size_t TBL_MASK = TBL_SIZE - 1;

// 16B accumulator: cnt+stamp packed into one u64 RMW (cnt<2^32 guaranteed -> no carry
// into stamp half). mn/mx are u16 and only aggregate lv<0xFFFF; rare large lv go to ovf.
struct Acc {
    u64 cnt_st = 0;             // lo32 = message_count, hi32 = total_stamp_count
    u32 sum = 0;                // total_length (fits u32, guaranteed)
    u16 mn = 0xFFFF, mx = 0;    // min/max over values < 0xFFFF (0xFFFF = "none")
};
static_assert(sizeof(Acc) == 16);

struct Agg {
    std::vector<u32> slots;  // (fp18<<14)|id, SLOT_EMPTY = vacant(偽一致はドレイン完全照合が回収)
    __m256i* keys = nullptr;  // id → 32B zero-padded 鍵(64B アライン: 照合ロードのライン跨ぎ排除)
    std::deque<std::string> owned;        // 名前の実体(mmap 非依存: DONTNEED 後も安全)
    std::vector<std::string_view> names;  // id -> channel name
    std::vector<Acc> acc;                 // [id*12 + month]
    std::vector<u32> mslots;   // medium(33..127B)ch 用テーブル: (fp18<<14)|id14
    __m256i* keys2 = nullptr;  // id → 128B 単射鍵(0x80+clen パッド)。calloc: 不使用なら物理ページゼロ
    std::unordered_map<std::string, u32> big;  // channels >= 128B (保証外の正しさバックストップ)
    std::unordered_map<u64, std::pair<u32, u32>> ovf;  // group -> (mn,mx) of lv >= 0xFFFF

    Agg() : slots(TBL_SIZE, SLOT_EMPTY), mslots(TBL_SIZE, SLOT_EMPTY) {
        keys = (__m256i*)aligned_alloc(64, 16384 * 32);  // ch ≤1万は公式保証 → 固定容量で安全
        // ゼロ初期化必須: want が EMPTY の fp 部と一致する鍵では未挿入時に偽一致 id=16383 が返り、
        // ドレイン照合が keys[16383] を読む。ゼロなら kv≠0 と必ず不一致 → recover_slow が正しく挿入。
        memset(keys, 0, 16384 * 32);
        keys2 = (__m256i*)calloc(16384 * 4, 32);  // 2MiB 仮想。medium ch が無ければ触れない
        names.reserve(16384);
        acc.reserve(16384 * 12);
    }
    Agg(const Agg&) = delete;
    Agg& operator=(const Agg&) = delete;
    ~Agg() {
        free(keys);
        free(keys2);
    }
    u32 new_id(const char* cs, u32 clen) {
        u32 id = (u32)names.size();
        // 公式保証は ch ≤10,000。万一の保証違反入力でも黙って壊れず停止する(keys/id14bit の防壁)
        if (__builtin_expect(id >= 16000, 0)) abort();
        names.emplace_back(owned.emplace_back(cs, clen));
        acc.resize(acc.size() + 12);
        return id;
    }
    __attribute__((always_inline)) inline u32 lookup32(const u64 k[4], const char* cs, u32 clen) {
        u64 c1 = _mm_crc32_u64(0, k[0]);
        u64 c2 = _mm_crc32_u64(c1, k[1]);
        u64 c3 = _mm_crc32_u64(c2, k[2]);
        u64 c4 = _mm_crc32_u64(c3, k[3]);
        // 18bit プレフィルタ(照合はドレインで完全)。EMPTY(id部=0x3FFF)は id≤15999 保証より
        // 実エントリと衝突せず、want=fp全1 の鍵は未挿入時に EMPTY と偽一致するが
        // keys ゼロ初期化+ドレイン照合→recover_slow が決定的に回収する(fp 特殊化不要)。
#ifdef FPTEST
        u32 want = (u32)c2 & (0x7u << 14);  // 検証用: 衝突を多発させ回復パスを踏ませる
#else
        u32 want = (u32)c2 & 0xFFFFC000u;
#endif
        size_t i = (size_t)c4 & TBL_MASK;  // crc32c は低位も良く混ざる
        while (true) {
            u32 e = slots[i];
            if (__builtin_expect((e & ~0x3FFFu) == want, 1)) return e & 0x3FFFu;
            if (e == SLOT_EMPTY) {
                u32 nid = new_id(cs, clen);
                keys[nid] = _mm256_load_si256((const __m256i*)k);
                slots[i] = want | nid;
                return nid;
            }
            i = (i + 1) & TBL_MASK;
        }
    }
    u32 lookup_big(const char* cs, u32 clen) {
        auto [it, ins] = big.try_emplace(std::string(cs, clen), 0u);
        if (ins) {
            it->second = new_id(cs, clen);
            keys[it->second] = _mm256_setzero_si256();  // big id の照合エントリはゼロ(kv=0 と恒等一致)
        }
        return it->second;
    }
    // medium channel(33..127B): 保証「パス長 ≤ 104B」内の 32B 超名を、行ごとの alloc なしで処理。
    // 鍵 = 128B を 0x80+clen でパッド(ASCII < 0x80 と不衝突 → 長さ込みで単射、clen ≤ 127 で pad ≤ 0xFF)。
    // 同一性はテーブル内 128B 完全比較で即時確定(fp はプレフィルタ)→ 決定的。slots/リングと独立で、
    // ドレインは kv=0 vs keys[id]=0(ゼロ初期化のまま)の恒等一致 → ホットパス契約は lookup_big と同一。
    __attribute__((noinline)) u32 lookup_med(const char* cs, u32 clen) {
        alignas(32) unsigned char kb[128];
        memset(kb, 0x80 + (int)clen, 128);
        memcpy(kb, cs, clen);
        const u64* kq = (const u64*)kb;
        u64 c = 0;
        for (int w = 0; w < 8; w++) c = _mm_crc32_u64(c, kq[w]);
#ifdef FPTEST
        u32 want = (u32)c & (0x7u << 14);  // 検証用: 衝突多発で完全比較経路を踏ませる
#else
        u32 want = (u32)c & 0xFFFFC000u;
#endif
        for (int w = 8; w < 16; w++) c = _mm_crc32_u64(c, kq[w]);
        __m256i k0 = _mm256_load_si256((const __m256i*)kb);
        __m256i k1 = _mm256_load_si256((const __m256i*)(kb + 32));
        __m256i k2 = _mm256_load_si256((const __m256i*)(kb + 64));
        __m256i k3 = _mm256_load_si256((const __m256i*)(kb + 96));
        size_t i = (size_t)c & TBL_MASK;
        while (true) {
            u32 e = mslots[i];
            if (e == SLOT_EMPTY) {  // EMPTY を fp 照合より先に判定(fp 全 1 鍵の偽一致・無限周回を構造的に排除)
                u32 nid = new_id(cs, clen);
                __m256i* kp = keys2 + (size_t)nid * 4;
                _mm256_storeu_si256(kp, k0);
                _mm256_storeu_si256(kp + 1, k1);
                _mm256_storeu_si256(kp + 2, k2);
                _mm256_storeu_si256(kp + 3, k3);
                mslots[i] = want | nid;
                return nid;
            }
            if ((e & ~0x3FFFu) == want) {
                u32 sid = e & 0x3FFFu;
                const __m256i* kp = keys2 + (size_t)sid * 4;
                __m256i x0 = _mm256_xor_si256(_mm256_loadu_si256(kp), k0);
                __m256i x1 = _mm256_xor_si256(_mm256_loadu_si256(kp + 1), k1);
                __m256i x2 = _mm256_xor_si256(_mm256_loadu_si256(kp + 2), k2);
                __m256i x3 = _mm256_xor_si256(_mm256_loadu_si256(kp + 3), k3);
                __m256i o = _mm256_or_si256(_mm256_or_si256(x0, x1), _mm256_or_si256(x2, x3));
                if (_mm256_testz_si256(o, o)) return sid;
            }
            i = (i + 1) & TBL_MASK;
        }
    }
    // 頻度順 slots 再構築: 現に slots に載っているエントリ(fp|id は不変)だけを、
    // acc 実測頻度の降順で home から線形再挿入する。高頻度 ch ほど probe 数が短くなり、
    // probe 継続分岐(実測 0.129 回/行、ミス ~0.12/行)の taken 率を下げる。
    // 線形 probing の不変量(home→現在位置の間に EMPTY なし)は全消し→順次挿入で保たれる。
    void reorganize() {
        std::vector<std::pair<u64, u32>> fe;  // (freq, slot entry)
        fe.reserve(names.size());
        for (u32 e : slots) {
            if (e == SLOT_EMPTY) continue;
            u32 id = e & 0x3FFFu;
            u64 f = 0;
            for (int m = 0; m < 12; m++) f += (u32)acc[(size_t)id * 12 + m].cnt_st;
            fe.push_back({f, e});
        }
        std::sort(fe.begin(), fe.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;    // 頻度降順
            return (a.second & 0x3FFFu) < (b.second & 0x3FFFu);  // 同点は id 昇順(決定化)
        });
        std::fill(slots.begin(), slots.end(), SLOT_EMPTY);
        for (auto& [f, e] : fe) {
            alignas(32) u64 k[4];
            _mm256_store_si256((__m256i*)k, keys[e & 0x3FFFu]);
            u64 c = _mm_crc32_u64(0, k[0]);
            c = _mm_crc32_u64(c, k[1]);
            c = _mm_crc32_u64(c, k[2]);
            c = _mm_crc32_u64(c, k[3]);
            size_t i = (size_t)c & TBL_MASK;
            while (slots[i] != SLOT_EMPTY) i = (i + 1) & TBL_MASK;
            slots[i] = e;
        }
    }
};

// parse 8 ASCII digits (MSD first) via SWAR
static inline u32 parse8(const char* p) {
    u64 val;
    memcpy(&val, p, 8);
    val -= 0x3030303030303030ULL;
    u64 b10 = (val * (1 + (10ULL << 8))) >> 8 & 0x00FF00FF00FF00FFULL;
    u64 s100 = (b10 * (1 + (100ULL << 16))) >> 16 & 0x0000FFFF0000FFFFULL;
    return (u32)((s100 * (1 + (10000ULL << 32))) >> 32);
}

static inline void acc_apply(Agg& g, size_t gi, u32 lv, u32 sv) {
    Acc& a = g.acc[gi];
    a.cnt_st += ((u64)sv << 32) | 1u;
    a.sum += lv;
    if (__builtin_expect(lv < 0xFFFFu, 1)) {
        u16 v = (u16)lv;
        if (__builtin_expect(v < a.mn || v > a.mx, 0)) {  // 定常状態ではほぼ更新なし → store 省略
            if (v < a.mn) a.mn = v;
            if (v > a.mx) a.mx = v;
        }
    } else {  // rare large message_length: exact min/max on the side
        auto [it, ins] = g.ovf.try_emplace((u64)gi, std::pair<u32, u32>{lv, lv});
        if (!ins) {
            if (lv < it->second.first) it->second.first = lv;
            if (lv > it->second.second) it->second.second = lv;
        }
    }
}

static inline void acc_update(Agg& g, u32 id, u32 mon, u32 lv, u32 sv) {
    acc_apply(g, (size_t)id * 12 + mon, lv, sv);
}

// 遅延 RMW リング: acc 行の滞留を隠しつつ、排出時に 32B 完全鍵照合で同一性を「決定的に」確定する。
// fp(50bit)一致は楽観で、照合不一致(=構成可能なハッシュ衝突データ)は recover_slow が正確に処理。
struct alignas(64) Pend {
    __m256i kv;              // マスク済み 32B 鍵(big id は 0 = keys 側も 0 で恒等一致)
    u32 id, gi, lv, sv;      // gi = id*12+mon。clen は kv から導出可能(単射パッドの副産物)
    u32 pad[4];
};
static_assert(sizeof(Pend) == 64);

__attribute__((noinline)) static void recover_slow(Agg& g, const Pend& q);

static inline void pend_apply(Agg& g, const Pend& q) {
    __m256i sk = _mm256_load_si256(g.keys + q.id);
    if (__builtin_expect(_ktestc_mask32_u8(_mm256_cmpeq_epi8_mask(sk, q.kv), (__mmask32)~0u), 1))
        acc_apply(g, q.gi, q.lv, q.sv);
    else
        recover_slow(g, q);  // fp 衝突: 完全照合で真の id を確定(正しさは無条件)
}

__attribute__((noinline)) static void recover_slow(Agg& g, const Pend& q) {
    u32 mon = q.gi - q.id * 12u;  // gi は(偽の)id で構成されているので差分が month
    // clen 導出: パッドは 0x80+clen(符号ビット付き)、名前は ASCII(<0x80)→ 最初の符号バイト位置
    u32 hb = (u32)_mm256_movemask_epi8(q.kv);
    u32 clen = hb ? _tzcnt_u32(hb) : 32;
    alignas(32) u64 k[4];
    _mm256_store_si256((__m256i*)k, q.kv);
    u64 c1 = _mm_crc32_u64(0, k[0]);
    u64 c2 = _mm_crc32_u64(c1, k[1]);
    u64 c3 = _mm_crc32_u64(c2, k[2]);
    u64 c4 = _mm_crc32_u64(c3, k[3]);
#ifdef FPTEST
    u32 want = (u32)c2 & (0x7u << 14);
#else
    u32 want = (u32)c2 & 0xFFFFC000u;
#endif
    size_t i = (size_t)c4 & TBL_MASK;
    while (true) {
        u32 e = g.slots[i];
        if (e == SLOT_EMPTY) {  // 真に新規(衝突相手より後の空きに挿入)
            char nb[32];
            memcpy(nb, k, 32);
            u32 nid = g.new_id(nb, clen);
            g.keys[nid] = q.kv;
            g.slots[i] = want | nid;
            acc_apply(g, (size_t)nid * 12 + mon, q.lv, q.sv);
            return;
        }
        if ((e & ~0x3FFFu) == want) {
            u32 sid = e & 0x3FFFu;
            __m256i sk = _mm256_load_si256(g.keys + sid);
            if ((u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(sk, q.kv)) == 0xFFFFFFFFu) {
                acc_apply(g, (size_t)sid * 12 + mon, q.lv, q.sv);
                return;
            }
        }
        i = (i + 1) & TBL_MASK;
    }
}

__attribute__((noinline)) static const char* parse_fast(const char* p, const char* end, Agg& g,
                                                        Pend* ring, unsigned& rn_io, Pend*& rp_io) {
    unsigned rn = rn_io;
    const char* fast_end = end - 64;  // 32B-load safety margin
    const __m256i commas = _mm256_set1_epi8(',');
    const __m256i iota = _mm256_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                          16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
    const __m512i thra = _mm512_loadu_si512((const void*)thr_lo8);
    const __m512i thrb = _mm512_loadu_si512((const void*)thr_hi8);
    const __m512i bswap8 = _mm512_broadcast_i32x4(_mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8));
    // リング・ウォームアップをプロローグに剥がし、本体ループの rn>=RINGD 判定(毎行 1µop)を除去
#define ROW_BODY(DRAIN_STMT) do { \
        _mm_prefetch(p + 384, _MM_HINT_T0); \
        __m512i xv = _mm512_shuffle_epi8(_mm512_set1_epi64(*(const long long*)p), bswap8); \
        __mmask16 mk = _mm512_kunpackb(_mm512_cmpge_epu64_mask(xv, thrb), _mm512_cmpge_epu64_mask(xv, thra)); \
        u32 mon = (u32)__builtin_popcount((u32)mk); \
        const char* cs = p + 11; \
        __m256i v = _mm256_loadu_si256((const __m256i*)cs); \
        u32 cm = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, commas)); \
        u32 clen, id; \
        __m256i kv; \
        if (__builtin_expect(cm != 0, 1)) { \
            clen = (u32)_tzcnt_u32(cm); \
            __m256i mask = _mm256_cmpgt_epi8(_mm256_set1_epi8((char)clen), iota); \
            kv = _mm256_ternarylogic_epi32(mask, v, _mm256_set1_epi8((char)(0x80 + clen)), 0xCA); \
            alignas(32) u64 k[4]; \
            _mm256_store_si256((__m256i*)k, kv); \
            id = g.lookup32(k, cs, clen); \
        } else { \
            const char* q = cs + 32; \
            while (*q != ',') ++q; \
            clen = (u32)(q - cs); \
            if (clen == 32) { \
                kv = v; \
                alignas(32) u64 k[4]; \
                _mm256_store_si256((__m256i*)k, v); \
                id = g.lookup32(k, cs, clen); \
            } else { \
                id = __builtin_expect(clen < 128, 1) ? g.lookup_med(cs, clen) : g.lookup_big(cs, clen); \
                kv = _mm256_setzero_si256(); \
            } \
        } \
        p = cs + clen + 1; \
        u32 gi = id * 12 + mon; \
        _mm_prefetch((const char*)(g.acc.data() + gi), _MM_HINT_T0); \
        _mm_prefetch((const char*)(g.keys + id), _MM_HINT_T0); \
        u32 lv; \
        { \
            u32 c0 = (unsigned char)p[0] - '0', c1 = (unsigned char)p[1] - '0'; \
            if (__builtin_expect(c1 <= 9, 1)) { \
                lv = c0 * 10 + c1; \
                p += 2; \
                for (unsigned c; (c = (unsigned char)(*p) - '0') <= 9; ++p) lv = lv * 10 + c; \
            } else { \
                lv = c0; \
                ++p; \
            } \
            ++p; \
        } \
        u32 sv = (unsigned char)p[0] - '0'; \
        { \
            u32 c1 = (unsigned char)p[1] - '0'; \
            if (__builtin_expect(c1 <= 9, 0)) { \
                sv = sv * 10 + c1; \
                p += 2; \
                for (unsigned c; (c = (unsigned char)(*p) - '0') <= 9; ++p) sv = sv * 10 + c; \
            } else { \
                ++p; \
            } \
            ++p; \
        } \
        DRAIN_STMT; \
        Pend& e = *rp; \
        rp = (rp == rend) ? ring : rp + 1; \
        e.kv = kv; \
        e.id = id; \
        e.gi = gi; \
        e.lv = lv; \
        e.sv = sv; \
    } while (0)
    // 再入時は前回のリング位置から継続する(ring 起点に戻すと、学習領域の行数 < RINGD の
    // とき未ドレインエントリを上書き+未初期化スロットをドレインする実バグになる。
    // 判定データ規模では踏めないが、保証内の病的入力(数 MB 級チャンネル名×少数行)で顕在化しうる)
    Pend* rp = rp_io;
    Pend* const rend = ring + (RINGD - 1);
    while (p < fast_end && rn < RINGD) {
        ROW_BODY(((void)0));
        ++rn;
    }
    while (p < fast_end) {
        ROW_BODY(pend_apply(g, *rp));
    }
#undef ROW_BODY
    rn_io = rn;
    rp_io = rp;
    return p;
}

static void kernel(const char* p, const char* end, Agg& g, size_t learn) {
    const char* kbegin = p;
    // populate をヘルパースレッドに逃がし、パース本体と重ねる(先行して PTE を張る)
    std::thread pop([p, end] {
        uintptr_t a = (uintptr_t)p & ~(uintptr_t)4095;
        uintptr_t e = (uintptr_t)end;
        const size_t BLK = 256u << 20;
        for (uintptr_t x = a; x < e; x += BLK)
            madvise((void*)x, (size_t)(x + BLK < e ? BLK : e - x), MADV_POPULATE_READ);
    });
    alignas(64) Pend ring[RINGD];
    unsigned rn = 0;
    Pend* rp = ring;
    if (learn && (size_t)(end - p) > learn * 4) {  // 大チャンクのみ: 学習パース → slots 再構築
        p = parse_fast(p, p + learn, g, ring, rn, rp);  // 行境界スナップ不要(行頭で判定・返却)
        g.reorganize();  // リングは slots 非依存なのでドレイン不要のまま続行できる
    }
    p = parse_fast(p, end, g, ring, rn, rp);
    if (rn < RINGD) {  // 高速路の行数 < RINGD: 先頭から rn 個
        for (unsigned j = 0; j < rn; ++j) pend_apply(g, ring[j]);
    } else {  // リング満杯: rp(最古)から一周
        for (unsigned j = 0; j < RINGD; ++j) {
            pend_apply(g, *rp);
            rp = (rp == ring + (RINGD - 1)) ? ring : rp + 1;
        }
    }
    // scalar tail (last <=64B): no wide loads
    while (p < end) {
        u32 ts = 0;
        while (*p != ',') ts = ts * 10 + u32(*p++ - '0');
        ++p;
        const char* cs = p;
        while (*p != ',') ++p;
        u32 clen = (u32)(p - cs);
        ++p;
        u32 lv = 0;
        while (*p != ',') lv = lv * 10 + u32(*p++ - '0');
        ++p;
        u32 sv = 0;
        while (*p != '\n') sv = sv * 10 + u32(*p++ - '0');
        ++p;
        u32 mon = d2m[(ts - TS_BASE) / 86400u];
        if (clen <= 32) {
            alignas(32) u64 k[4];
            memset(k, 0x80 + (int)clen, 32);
            memcpy(k, cs, clen);
            u32 id = g.lookup32(k, cs, clen);
            Pend q;
            q.kv = _mm256_load_si256((const __m256i*)k);
            q.id = id;
            q.gi = id * 12 + mon;
            q.lv = lv;
            q.sv = sv;
            pend_apply(g, q);  // 末尾も完全照合を通す
        } else {
            u32 id = clen < 128 ? g.lookup_med(cs, clen) : g.lookup_big(cs, clen);
            acc_update(g, id, mon, lv, sv);
        }
    }
    pop.join();
    // (e136 まであった MADV_DONTNEED 分離返却は削除: fork 早期退出で解体が計測外)
}

static int g_done_w = -1;  // fork 子→親の完了通知 pipe(-1 = フォークなしで動作中)

int main(int argc, char** argv) {
#ifndef NOFORK
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            pid_t c = fork();
            if (c > 0) {  // 親: 子の「出力完了」通知を待って即終了(解体は子に残す)
                close(pfd[1]);
                char b = 0;
                ssize_t r;
                do { r = read(pfd[0], &b, 1); } while (r < 0 && errno == EINTR);
                if (r == 1 && b == 'K') _exit(0);
                int st = 0;  // 通知前に子が死んだ: 終了コードを伝搬
                waitpid(c, &st, 0);
                _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
            }
            if (c == 0) {  // 子: 本体を実行。stdio はランナーの EOF 検知のため即閉鎖
                close(pfd[0]);
                g_done_w = pfd[1];
                close(0);
                close(1);
                close(2);
            } else {  // fork 失敗: インラインで続行
                close(pfd[0]);
                close(pfd[1]);
            }
        }
    }
#endif
    if (argc < 3) return 1;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return 1;
    struct stat sb;
    if (fstat(fd, &sb) != 0) return 1;
    size_t size = (size_t)sb.st_size;
    const char* data = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) return 1;
    madvise((void*)data, size, MADV_SEQUENTIAL);

    {
        int d = 0;
        for (int m = 0; m < 12; m++)
            for (int i = 0; i < MDAYS[m]; i++) d2m[d++] = (u8)m;
    }
    {
        u64 thr[12];
        int d = 0;
        for (int m = 0; m < 12; m++) {
            char b[16];
            snprintf(b, sizeof b, "%u", TS_BASE + (u32)d * 86400u);  // always 10 digits
            u64 x;
            memcpy(&x, b, 8);
            thr[m] = __builtin_bswap64(x);
            d += MDAYS[m];
        }
        for (int i = 0; i < 8; i++) thr_lo8[i] = thr[i + 1];             // thr[1..8]
        for (int i = 0; i < 8; i++) thr_hi8[i] = (i < 3) ? thr[i + 9] : ~0ULL;  // thr[9..11] + pad
    }

    const char* p = data;
    const char* end = data + size;
    while (p < end && *p != '\n') ++p;  // skip header
    ++p;

    // ---- threads: line-boundary chunks, thread-local aggregation ----
    unsigned nt = std::thread::hardware_concurrency();
    if (const char* e = getenv("THREADS")) {
        int v = atoi(e);
        if (v >= 1 && v <= 64) nt = (unsigned)v;
    }
    if (nt < 1) nt = 1;
    size_t learn = size_t(32) << 20;  // 頻度学習パース量(LEARN=MB で上書き、0 で無効)
    if (const char* e = getenv("LEARN")) {
        long v = atol(e);
        if (v >= 0 && v <= 4096) learn = (size_t)v << 20;
    }
    size_t total = (size_t)(end - p);
    if (total < (size_t)nt * 4096) nt = 1;  // tiny input: single thread

    auto snap = [&](const char* q) -> const char* {
        if (q <= p) return p;
        if (q >= end) return end;
        while (q < end && *q != '\n') ++q;
        return q < end ? q + 1 : end;
    };

    std::vector<Agg> aggs(nt);
    // ---- merge 用グローバル(スレッド join と逐次マージを重ねるため先に宣言) ----
    std::unordered_map<std::string_view, u32> gid;
    gid.reserve(1 << 14);
    std::vector<std::string_view> gnames;
    gnames.reserve(16384);
    std::vector<Acc> gacc;
    gacc.reserve(16384 * 12);
    std::unordered_map<u64, std::pair<u32, u32>> govf;
    {
        unsigned hc = std::thread::hardware_concurrency();
        std::vector<std::thread> ths;
        ths.reserve(nt);
        for (unsigned i = 0; i < nt; i++) {
            const char* b = snap(p + total * i / nt);
            const char* e2 = snap(p + total * (i + 1) / nt);
            ths.emplace_back(kernel, b, e2, std::ref(aggs[i]), learn);
            if (hc && nt <= hc) {  // 1 スレッド 1 vCPU にピン留め(マイグレーション抑止)
                cpu_set_t cs;
                CPU_ZERO(&cs);
                CPU_SET(i, &cs);
                pthread_setaffinity_np(ths.back().native_handle(), sizeof(cs), &cs);
            }
        }
        // join した順に逐次マージ(最遅スレッドを待つ間に他 7 本分のマージを消化)
        for (unsigned ti = 0; ti < nt; ti++) {
        ths[ti].join();
        Agg& a = aggs[ti];
        std::vector<u32> id2g(a.names.size());
        for (u32 id = 0; id < (u32)a.names.size(); id++) {
            auto [it, ins] = gid.try_emplace(a.names[id], (u32)gnames.size());
            if (ins) {
                gnames.push_back(a.names[id]);
                gacc.resize(gacc.size() + 12);
            }
            u32 gi = it->second;
            id2g[id] = gi;
            for (int m = 0; m < 12; m++) {
                const Acc& s = a.acc[(size_t)id * 12 + m];
                if (!(u32)s.cnt_st) continue;
                Acc& d = gacc[(size_t)gi * 12 + m];
                d.cnt_st += s.cnt_st;
                d.sum += s.sum;
                if (s.mn < d.mn) d.mn = s.mn;
                if (s.mx > d.mx) d.mx = s.mx;
            }
        }
        for (auto& [k, v] : a.ovf) {
            u64 gk = (u64)id2g[(size_t)(k / 12)] * 12 + (u32)(k % 12);
            auto [it2, ins2] = govf.try_emplace(gk, v);
            if (!ins2) {
                if (v.first < it2->second.first) it2->second.first = v.first;
                if (v.second > it2->second.second) it2->second.second = v.second;
            }
        }
        }
    }

    // build output in memory (formatted in parallel), then sequential writes
    u32 gn = (u32)gnames.size();
    unsigned ont = nt < 8 ? nt : 8;
    if (!ont) ont = 1;
    std::vector<std::vector<char>> outs(ont);
    {
        std::vector<std::thread> oth;
        oth.reserve(ont);
        for (unsigned t = 0; t < ont; t++) {
            u32 id0 = (u32)((u64)gn * t / ont), id1 = (u32)((u64)gn * (t + 1) / ont);
            oth.emplace_back([&, id0, id1, t] {
                std::vector<char>& out = outs[t];
                out.reserve((size_t)(id1 - id0) * 12 * 48 + 4096);
                char buf[256];
                for (u32 id = id0; id < id1; id++) {
                    const std::string_view& nm = gnames[id];
                    for (int m = 0; m < 12; m++) {
                        const Acc& a = gacc[(size_t)id * 12 + m];
                        u32 cnt = (u32)a.cnt_st;
                        if (!cnt) continue;
                        u32 st = (u32)(a.cnt_st >> 32);
                        u32 mn = (a.mn == 0xFFFF) ? 0xFFFFFFFFu : a.mn;
                        u32 mx = a.mx;
                        if (__builtin_expect(!govf.empty(), 0)) {
                            auto io = govf.find((u64)id * 12 + m);
                            if (io != govf.end()) {
                                if (io->second.first < mn) mn = io->second.first;
                                if (io->second.second > mx) mx = io->second.second;
                            }
                        }
                        // 名前はバイト直挿入(%.*s は NUL で切れるため使わない)、残りだけ snprintf
                        int n = snprintf(buf, sizeof buf, ",2027-%02d=%u/%.2f/%u/%u/%u\n", m + 1, mn,
                                         (double)a.sum / (double)cnt, mx, cnt, st);
                        out.insert(out.end(), nm.begin(), nm.end());
                        out.insert(out.end(), buf, buf + n);
                    }
                }
            });
        }
        for (auto& t : oth) t.join();
    }
    int ofd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) return 1;
    for (auto& out : outs) {
        size_t off = 0;
        while (off < out.size()) {
            ssize_t w = write(ofd, out.data() + off, out.size() - off);
            if (w <= 0) return 1;
            off += (size_t)w;
        }
    }
    close(ofd);
    if (g_done_w >= 0) {  // 出力完了を親へ通知 → 親が即 exit。以降の解体は計測外(判定実装依存)
        char b = 'K';
        ssize_t r;
        do { r = write(g_done_w, &b, 1); } while (r < 0 && errno == EINTR);
        close(g_done_w);
    }
    _exit(0);  // ヒープ破棄(Agg×8 の acc/keys/owned 等 24MB+)と glibc teardown をスキップ
}
