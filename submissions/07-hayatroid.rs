// 1BRC for traP の提出プログラム。
// CSV の各行 (unix_timestamp,channel_path,message_length,stamp_count) を読み、
// チャンネルと月の組ごとに min/avg/max/message_count/total_stamp を集計して
// 出力ファイルへ書き出す。
//
// このプログラムの速度を決めるのは何か。計測環境では入力 (最大 26GB) が OS の
// ページキャッシュに乗っているため、ディスクにもメモリ帯域にも律速しない。残るのは
// コア実行そのものであり、その中身は uop と cycle の言葉で数えられる。1 行の処理は
// スレッドあたり約 98 cycle かかる。うち約 66 cycle は発行スループットの床であり、
// 1 行が必要とする約 190 uop (パース約 140、表引き約 20、集計約 25) を、HT で
// 折半された発行帯域 (約 3 uop/cycle) に流し込む時間がそのまま下限になる。残りの
// 約 28 cycle はハッシュ表と統計スロットへのランダムロードの露出であり、作業集合が
// キャッシュ容量を超える分だけ、先読みでも隠しきれないレイテンシが残る。行走査の
// マスクカーソルのような直列依存は、この発行時間の陰に隠れて単独では表面化しない。
//
// したがって設計は 2 軸に絞られる。
//
//   1. 発行圧を削る。行あたりの uop 数がそのまま時間なので、命令列を短くする。
//      境界検出は 64 バイトブロックの構造マスク 1 本に畳み、数値パースは桁数に
//      よらず同じベクタ命令列を通し、キー照合は正規形 32 バイトの比較 1 回にする。
//      データ依存分岐を予測可能な形へ寄せるのもこの軸の一部である (予測ミスは
//      発行済み uop の破棄として効く)。
//   2. ランダムロードを重ね、作業集合を縮める。表アクセスはバッチ処理と先読みで
//      行間に重ね合わせ、毎行書く統計は 8 バイトの加算器に畳んで L2 常駐圏へ
//      収める。
//
// 実行環境には AVX2 を前提とする (計測環境の CPU は対応を保証されている。起動時に
// 1 回だけ検査し、無ければ即座に落とす)。
//
// 以降の構成はデータの流れに沿う。入力の取り込み (mmap)、集計の器 (ホット加算器、
// 極値表、側表、cold 表)、行エンジン (構造マスク、ベクタパース、キー正規形と表、
// バッチ 3 パス)、並列化とマージ、出力と丸め、の順に並べてある。

use std::alloc::{Layout, alloc_zeroed, handle_alloc_error};
use std::collections::HashMap;
use std::os::fd::AsRawFd;
use std::thread;

// ---------------------------------------------------------------------------
// 入力の取り込み。
//
// 入力は fs::read で読まず、mmap で直接マップする。理由はメモリ予算にある。
// 計測環境は入力データのページキャッシュでメモリの大半が埋まっており、プログラムが
// 使えるのは 10GiB 程度しかない。26GB を匿名バッファへコピーするとキャッシュを
// 追い出してディスク読みに転落するが、mmap ならキャッシュ済みページをそのまま
// マップするので追加メモリをほぼ使わない。
//
// mmap と madvise は libc の関数を自前宣言して呼ぶ (std は常に libc をリンクして
// いるので、宣言さえ書けばリンクは通る)。munmap は呼ばない。マップの破棄はプロセス
// 終了時に OS が行うので、終了直前に 26GB を自前でアンマップするのは時間の無駄で
// ある。
// ---------------------------------------------------------------------------
#[allow(non_camel_case_types)]
type c_void = core::ffi::c_void;

unsafe extern "C" {
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: i32,
        flags: i32,
        fd: i32,
        offset: i64,
    ) -> *mut c_void;
    fn madvise(addr: *mut c_void, length: usize, advice: i32) -> i32;
}

const PROT_READ: i32 = 0x1;
const MAP_PRIVATE: i32 = 0x2;
const MADV_SEQUENTIAL: i32 = 0x2;
const MADV_WILLNEED: i32 = 0x3;
const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;

// ---------------------------------------------------------------------------
// 集計の器。
//
// 集計値は 4 つの器に分かれる。毎行書くホット加算器 (8 バイトの Hot)、毎行読むが
// 書くのは稀な極値表 (4 バイトの MinMax)、飽和退避と u16 幅に収まらない行を受ける
// 側表 (全幅の Stats32)、キーが 32 バイトを超える行だけを受ける cold 表である。
// 後二者は実データで頻度がほぼゼロであり、仕様の全域を正確に受けるためだけに
// 存在する。速さはホット側 (加算器と極値表) が、正確性の残りは側表と cold 表が
// 受け持つ。
// ---------------------------------------------------------------------------

// ホットパスが毎行書くのは加算 3 本 (count / total_length / total_stamp) である。
// これを 8 バイトの Hot に詰め、計測環境 (L2 2MB/コア、HT 折半で実質 1MB/スレッド)
// の L2 常駐圏へ毎行書く作業集合を収める。1 スロット 16 バイトだとチャンネル 1 万 ×
// 12 ヶ月で 1.92MB/スレッドとなって L2 を溢れ、共有 L3 を奪い合う。加算器を 8 バイト
// に畳めば 960KB に縮み、ランダムストアの露出が減る。
//
// count と total_stamp を u16、total_length を u32 で持つと、repr(C) により
// total_len@0 / count@4 / stamp@6 の 8 バイトが u64 1 語とバイト配置で一致する。
// 行更新 (total_len += mlen、count += 1、stamp += stamp) は u64 1 本の加算に畳まれる。
//
// u16 幅は仕様の保証を超え得る点が正確性の要である。仕様が保証するのは「1 行の
// message_length は 1 以上」「チャンネル×月の合計 (件数・長さ・スタンプ) が u32 に
// 収まる」ことだけで、単一スレッドの部分和でも count は u16 を跨ぎ得るし、stamp は
// 部分和どころか 1 行の値が u16 を超えることすらある。そこで更新前に飽和判定を
// 置く。いずれかのフィールドが今回の加算で桁上げするなら、ホットエントリの加算器を
// 全幅の側表 (spill) へ加算してゼロクリアし、空の状態から今回の行を積み直す。これで
// u64 1 本の加算は決してフィールド境界をまたがず、稀な飽和だけが側表へ逃げる。
// 加算は結合則を満たすので、部分和がどう分割されても出力時の合成結果は一意である。
//
// min / max は毎行読むが、書くのは新しい極値が来たときだけなので、加算器と分けて
// 4 バイトの MinMax に持つ。分離により毎行ストアするタッチ集合は 8 バイト刻みに
// 保たれ、極値表は読みが主の別ラインとして振る舞う。min の空判定には番兵 0xFFFF を
// 使う。ホットに積む行は mlen < 0xFFFF に絞られる (それ以外は側表へ直行) ので、
// u16 の min は必ず 0xFFFF 未満となり番兵と衝突しない。

/// ホットパスの加算器。8 バイト (total_len@0 / count@4 / stamp@6) が u64 1 語に
/// 一致し、行更新は u64 1 本の加算に畳まれる。align(8) で配列要素を 8 バイト境界に
/// 載せ、u64 アクセスがキャッシュラインをまたがないようにする。
#[repr(C, align(8))]
#[derive(Clone, Copy)]
struct Hot {
    total_len: u32,
    count: u16,
    stamp: u16,
}

impl Hot {
    #[inline(always)]
    const fn new() -> Self {
        Hot {
            total_len: 0,
            count: 0,
            stamp: 0,
        }
    }
}

/// 極値表。毎行読み、稀に書く 4 バイト。min の番兵は 0xFFFF。
#[repr(C)]
#[derive(Clone, Copy)]
struct MinMax {
    min: u16,
    max: u16,
}

impl MinMax {
    #[inline(always)]
    const fn new() -> Self {
        MinMax {
            min: u16::MAX,
            max: 0,
        }
    }
}

/// 側表 (spill) の全幅アキュムレータ。飽和退避されたホット加算器の部分和と、u16 幅
/// に収まらない稀な行 (mlen >= 0xFFFF または stamp > 0xFFFF) がここへ集まる。頻度は
/// ほぼゼロなので単純さを優先し、スレッドごとに (スロット添字, month) をキーとする
/// std の HashMap で持つ。ホット側と側表はどちらも同一チャンネル×月の合計 (仕様上
/// u32 に収まる) の部分和なので、出力時に u32 で合成しても溢れない。
#[derive(Clone, Copy)]
struct Stats32 {
    count: u32,
    total_length: u32,
    total_stamp: u32,
    min: u32,
    max: u32,
}

impl Stats32 {
    #[inline]
    fn new() -> Self {
        Stats32 {
            count: 0,
            total_length: 0,
            total_stamp: 0,
            min: u32::MAX,
            max: 0,
        }
    }

    #[inline]
    fn add(&mut self, len: u32, stamp: u32) {
        self.count += 1;
        self.total_length += len;
        self.total_stamp += stamp;
        if len < self.min {
            self.min = len;
        }
        if len > self.max {
            self.max = len;
        }
    }

    #[inline]
    fn merge(&mut self, other: &Stats32) {
        self.count += other.count;
        self.total_length += other.total_length;
        self.total_stamp += other.total_stamp;
        if other.min < self.min {
            self.min = other.min;
        }
        if other.max > self.max {
            self.max = other.max;
        }
    }
}

// cold 表: clen > 32 のキーだけを集める補助表。頻度は実データでほぼゼロなので
// 単純さを優先し、キー実体を所有する std の HashMap で持つ。u16 幅に収まらない行も
// 含め、値は全幅の Stats32 で持つので、ホット側のような圧縮と飽和判定は要らない。
struct ColdTable {
    map: HashMap<Vec<u8>, [Stats32; 12]>,
}

impl ColdTable {
    fn new() -> Self {
        ColdTable {
            map: HashMap::new(),
        }
    }

    /// clen > 32 の 1 行を集計する。out-of-line に置いてホットパスを汚さない。
    #[cold]
    #[inline(never)]
    unsafe fn add(&mut self, ptr: *const u8, len: usize, month: usize, mlen: u32, stamp: u32) {
        let key = unsafe { std::slice::from_raw_parts(ptr, len) };
        self.map
            .entry(key.to_vec())
            .or_insert([Stats32::new(); 12])[month]
            .add(mlen, stamp);
    }
}

// ---------------------------------------------------------------------------
// 行エンジン。
//
// 1 行の処理は 4 段からなる。構造マスクで区切り位置を得て、ベクタパースで数値
// 3 フィールドを変換し、channel_path を正規形キーに畳んでハッシュ表を引き、バッチ
// 3 パスで集計する。前半 3 段は冒頭の 2 軸のうち発行圧の削減を、最後のバッチ
// 3 パスはランダムロードの重ね合わせを受け持つ。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 行エンジン 1/4: 区切り位置の一括インデックス化 (simdjson の構造解析ステージと
// 同じ発想)。
//
// フィールドごとにロードと比較を連ねて境界を検出すると、境界 1 つあたり数 uop の
// 検査列がメモリロード依存の連鎖として並び、発行圧と直列依存の両方を太らせる。
// そこでチャンクを 64 バイトブロック単位に走査し、各ブロックの「構造文字 (カンマ
// または改行) ビットマスク」を AVX2 で 1 回作る。行パーサはそのマスクから tzcnt と
// blsr で区切りを O(1) に取り出せるので、境界検出はメモリロードに依存しない
// レジスタ内演算だけになり、コストはブロックあたり一括のマスク生成 (64 バイト ≈
// 2.5 行ぶん) に畳まれる。
//
// カンマと改行を分けず 1 本の構造マスクにできるのは、入力の整形保証による。各行は
// timestamp、channel、mlen、stamp の 4 フィールドで、channel_path はカンマも改行も
// 含まない。従って 1 行の構造文字は常に「カンマ 3 個 → 改行 1 個」であり、種類は
// マスク上の順序で完全に決まる。どれがカンマかを判定する必要が無いので、比較結果を
// OR して movemask を 1 回に畳め、マスクカーソルも 1 本で済んでレジスタ生存が
// 少ない。
//
// 512 ビット幅を使わないのは、計測環境の CPU では 512b ロードが 2 本/サイクルに
// 制限され (256b 以下は 3 本/サイクル)、ハッシュ表プローブのスカラロードと帯域を
// 食い合うためである。256b×2 に留めれば 3 ロード/サイクルの経路が保たれる。
// ---------------------------------------------------------------------------

/// `base+off` から 64 バイトを読み、カンマまたは改行の位置に立てた 64 ビット
/// マスクを返す (bit j は base+off+j が構造文字であることを表す)。256 ビットロード
/// 2 本、比較 2 種の OR、movemask で 32 ビットずつ作って連結する。
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn block_mask_avx2(base: *const u8, off: usize) -> u64 {
    use std::arch::x86_64::*;
    unsafe {
        let p = base.add(off);
        let comma = _mm256_set1_epi8(b',' as i8);
        let nl = _mm256_set1_epi8(b'\n' as i8);
        let lo = _mm256_loadu_si256(p as *const __m256i);
        let hi = _mm256_loadu_si256(p.add(32) as *const __m256i);
        let s_lo = _mm256_or_si256(_mm256_cmpeq_epi8(lo, comma), _mm256_cmpeq_epi8(lo, nl));
        let s_hi = _mm256_or_si256(_mm256_cmpeq_epi8(hi, comma), _mm256_cmpeq_epi8(hi, nl));
        let m_lo = _mm256_movemask_epi8(s_lo) as u32 as u64;
        let m_hi = _mm256_movemask_epi8(s_hi) as u32 as u64;
        m_lo | (m_hi << 32)
    }
}

/// 構造マスク列から次の区切りの絶対位置を取り出す。現ブロックのマスク `smask`
/// が尽きたら 64 バイト先へブロックを送ってマスクを作り直す。取り出した最下位
/// ビットは blsr (`x & (x-1)`) でクリアする。呼び出しは常に「行内に必ず存在する
/// 区切り」に対してのみ行うので、走査が範囲を越えることはない (MARGIN が 64 バイト
/// のオーバーリードを吸収する)。
#[inline(always)]
unsafe fn next_delim(base: *const u8, smask: &mut u64, sblock: &mut usize) -> usize {
    while *smask == 0 {
        *sblock += 64;
        *smask = unsafe { block_mask_avx2(base, *sblock) };
    }
    let pos = *sblock + smask.trailing_zeros() as usize;
    *smask &= *smask - 1;
    pos
}

// ---------------------------------------------------------------------------
// 行エンジン 2/4: ベクタパース。
//
// 数値 3 フィールド (timestamp 上位 8 桁、message_length、stamp_count) は、桁数に
// よらず同じ命令列で変換する。可変長のフィールドはシフトと '0' 埋めで 8 桁固定の
// 正規形に寄せてから、3 フィールドまとめて 1 本の SIMD 連鎖に流す。桁ごとの
// ループは桁数というデータ依存の分岐を持ち込み、message_length のように桁数分布が
// 広いフィールドで予測を外すので使わない。
// ---------------------------------------------------------------------------

#[inline(always)]
unsafe fn load_u64(p: *const u8) -> u64 {
    unsafe { (p as *const u64).read_unaligned() }
}

/// `w` に入った ASCII 数字 8 桁 (バイト 0 が最上位桁) を一括変換する。
/// 隣接ペア、4 桁、8 桁の順に乗算とシフトとマスクで畳み込む Lemire のピラミッド法
/// である。桁ごとのループと違って分岐がなく、段数も桁数の対数で済む。
#[inline(always)]
fn parse8(w: u64) -> u32 {
    let d = w - 0x3030_3030_3030_3030;
    let p = (d & 0x00FF_00FF_00FF_00FF).wrapping_mul(10) + ((d >> 8) & 0x00FF_00FF_00FF_00FF);
    let q = (p & 0x0000_FFFF_0000_FFFF).wrapping_mul(100) + ((p >> 16) & 0x0000_FFFF_0000_FFFF);
    ((q & 0xFFFF_FFFF).wrapping_mul(10000) + (q >> 32)) as u32
}

/// 桁数 digits から (シフト量, ゼロ埋めマスク) を引く静的テーブル。
/// 可変長の数値を分岐なしでパースするための道具である。8 バイトロードを上位バイト
/// へ寄せて parse8 の並びに正規化するシフト量と、シフトで空いた下位バイトを
/// ASCII '0' で満たすマスクを、コンパイル時に畳んでおく。
const fn build_shift_fill_table() -> [(u32, u64); 9] {
    const FILL0: u64 = 0x3030_3030_3030_3030; // '0' を 8 レーンに複製した値
    let mut t = [(0u32, 0u64); 9];
    let mut digits = 0usize;
    while digits <= 8 {
        let shift = ((8 - digits) as u32) * 8;
        let fill = if shift >= 64 { 0 } else { FILL0 & ((1u64 << shift) - 1) };
        t[digits] = (shift, fill);
        digits += 1;
    }
    t
}
static SHIFT_FILL_TABLE: [(u32, u64); 9] = build_shift_fill_table();

/// 桁数 digits (<= 8) のフィールドを、parse8 が食える u64 (バイト 0 が最上位桁、
/// 空いた上位桁は '0' 埋め) に正規化する。乗算を使わない前段で、出力をベクタ
/// パースに渡す。
#[inline(always)]
unsafe fn norm8(base: *const u8, start: usize, digits: usize) -> u64 {
    let w = unsafe { load_u64(base.add(start)) };
    let (shift, fill) = unsafe { *SHIFT_FILL_TABLE.get_unchecked(digits) };
    (w << shift) | fill
}

/// 桁数 digits が既知のときの符号なし整数パース。マスクから得た区切り位置で桁数を
/// 決めているので、境界検出は要らない。digits <= 8 は shift/fill テーブルで
/// parse8 に一発で通し、digits >= 9 (u32 全域を取るための稀な経路) だけ残りを
/// 桁ループで畳む。
#[inline(always)]
unsafe fn parse_num(base: *const u8, start: usize, digits: usize) -> u32 {
    let w = unsafe { load_u64(base.add(start)) };
    if digits <= 8 {
        let (shift, fill) = unsafe { *SHIFT_FILL_TABLE.get_unchecked(digits) };
        let w_norm = (w << shift) | fill;
        parse8(w_norm)
    } else {
        let mut v = parse8(w);
        let endp = start + digits;
        let mut j = start + 8;
        while j < endp {
            v = v.wrapping_mul(10).wrapping_add((unsafe { *base.add(j) } - b'0') as u32);
            j += 1;
        }
        v
    }
}

/// 3 フィールドの 8 桁を 1 本の AVX2 連鎖でまとめてパースする (Mula 方式)。
/// 各引数は「バイト 0 が最上位桁」に正規化済みの 8 バイトで、256 ビットレジスタの
/// 8 バイトレーンに ts / mlen / stamp / 0 と詰め、減算 '0' → maddubs (係数 10,1) →
/// madd (係数 100,1) → pack → madd (係数 10000,1) の連鎖で 3 値を同時に畳む。
/// スカラの parse8 は 1 フィールドに整数乗算 2 本 (乗算専用ポート行き) を使う
/// のに対し、この連鎖は SIMD ポートだけで済むため、行あたり 6 本の整数乗算が
/// ホットループから消える。
#[inline]
#[target_feature(enable = "avx2")]
fn parse3x8(ts_w: u64, mlen_norm: u64, stamp_norm: u64) -> (u32, u32, u32) {
    use std::arch::x86_64::*;
    // レーン配置は [0..8) が ts、[8..16) が mlen、[16..24) が stamp、[24..32) は
    // 未使用。
    let x = _mm256_set_epi64x(0, stamp_norm as i64, mlen_norm as i64, ts_w as i64);
    let d = _mm256_sub_epi8(x, _mm256_set1_epi8(0x30));
    let m1 = _mm256_maddubs_epi16(d, _mm256_set1_epi16(0x010A));
    let m2 = _mm256_madd_epi16(m1, _mm256_set1_epi32(0x0001_0064));
    let pk = _mm256_packus_epi32(m2, m2);
    let m3 = _mm256_madd_epi16(pk, _mm256_set1_epi32(0x0001_2710));
    // 下位 128 ビットに ts8 と mlen、上位 128 ビットに stamp が並ぶ。
    let lo64 = _mm_cvtsi128_si64(_mm256_castsi256_si128(m3)) as u64;
    let b = _mm_cvtsi128_si32(_mm256_extracti128_si256(m3, 1)) as u32;
    (lo64 as u32, (lo64 >> 32) as u32, b)
}

// 暦計算 (Howard Hinnant の days_from_civil / civil_from_days)。
//
// タイムスタンプから月への変換は、起動時にこのアルゴリズムから「日番号を月に引く
// テーブル」を 1 回導出しておき、以後は行ごとに ts/86400 とテーブル引きだけで
// 済ませる。月境界の定数を直に並べる代わりにアルゴリズムから導出しているので、
// うるう年も正しく扱われ、入力の年が変わっても YEAR 定数の変更だけで追従できる。
fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = (if y >= 0 { y } else { y - 399 }) / 400;
    let yoe = y - era * 400;
    let mp = if m > 2 { m - 3 } else { m + 9 } as i64;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

fn civil_from_days(z: i64) -> (i64, u32) {
    let z = z + 719468;
    let era = (if z >= 0 { z } else { z - 146096 }) / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    (y, m as u32)
}

const YEAR: i64 = 2027;

/// 日番号 (2027-01-01 からの経過日数) を月スロット (0..=11) に引くテーブルを作る。
/// うるう年でも溢れないよう 366 要素にしてある。
fn build_month_table(base_day: i64) -> [u8; 366] {
    let mut t = [0u8; 366];
    for d in 0..366i64 {
        let (_, m) = civil_from_days(base_day + d);
        t[d as usize] = (m - 1) as u8;
    }
    t
}

// ---------------------------------------------------------------------------
// 行エンジン 3/4: キー正規形と表。32 バイトのゼロマスク値を正規形とするオープン
// アドレスハッシュ表である。
//
// channel_path の文字集合は NUL を含まない。長さ clen のキーを 32 バイト境界まで
// ゼロ埋めした値を正規形にすると、この 32 バイトの等値がキーの等値と厳密に同値に
// なる。clen より後ろはゼロ、clen より手前は必ず非ゼロなので、先頭から連続する
// 非ゼロバイト数がそのまま長さを表し、長さ比較すら要らない。
//
// 実データのキーはほぼすべて 32 バイト以下なので、キー本体をスロットへ 32 バイトで
// インライン化する。プローブは YMM 1 本のロードと 1 本の比較に畳まれ、キー実体への
// 間接参照もバイト長ごとの分岐も発生しない。clen が 32 を超える稀なキーは cold 表
// へ丸ごと退避する。長さ 32 以下は本表、33 以上は cold と、キー空間が長さで完全に
// 分かれるので両者は決して衝突しない。
//
// スロット添字がそのまま id を兼ねる。slots32 がキー本体、slot_len が長さ (0 は空
// スロット)、hot と mm が集計本体で、いずれも添字で直に引く。kptr/klen/id といった
// 間接層は持たない。集計配列は使用チャンネル分のページしか常駐させないよう、ゼロ
// 初期化で確保して挿入時にだけ番兵込みで書き起こす。
// ---------------------------------------------------------------------------
const CAP: usize = 1 << 15; // 32768 (負荷率 約0.3: 使用チャンネル 1 万 / 32768)
const MASK: usize = CAP - 1;

/// 32 バイト境界に整列したキー本体。整列は intern の YMM ロード/ストアのため。
#[repr(C, align(32))]
#[derive(Clone, Copy)]
struct Key32([u8; 32]);

/// ゼロ初期化した T の連続領域を boxed slice で確保する。物理ページは遅延ゼロ埋め
/// なので、実際に書き込んだページしか常駐しない。全ゼロが有効値である T
/// (Key32 / u8 / [Hot; 12] / [MinMax; 12]) にのみ使う。
fn boxed_zeroed_slice<T>(n: usize) -> Box<[T]> {
    let layout = Layout::array::<T>(n).unwrap();
    unsafe {
        let ptr = alloc_zeroed(layout) as *mut T;
        if ptr.is_null() {
            handle_alloc_error(layout);
        }
        Box::from_raw(std::slice::from_raw_parts_mut(ptr, n))
    }
}

const FXK: u64 = 0x51_7c_c1_b7_27_22_0a_95; // FxHash の乗算定数

/// 32 バイトのマスク済みキーからハッシュを導く。4 つの u64 レーンを rotate と
/// xor で混ぜ、最後に一度だけ FXK を乗じる。全 32 バイトが寄与するので、長い共通
/// プレフィクスを持つキー群でも末尾の差が確実にハッシュへ伝わる。乗算は 1 本だけ。
///
/// 4 レーンは volatile ロードで読む。ホットパスは直前に同じ番地へ YMM を
/// ストアしており、素の読みだとコンパイラがストア転送を YMM からのレーン抽出
/// (vmovq/vpextrq/vextracti128 = p5 シャッフル) に畳んでしまう。volatile は実メモリ
/// ロード (p2/p3) を強制し、希少な p5 ポートを空ける。呼び出し側は必ず 32 バイト整列
/// の Key32 を渡すので、8 バイト境界の要件は満たされる。
#[inline(always)]
unsafe fn hash_key32(p: *const u8) -> u64 {
    let a = unsafe { (p as *const u64).read_volatile() };
    let b = unsafe { (p.add(8) as *const u64).read_volatile() };
    let c = unsafe { (p.add(16) as *const u64).read_volatile() };
    let d = unsafe { (p.add(24) as *const u64).read_volatile() };
    (a ^ b.rotate_left(21) ^ c.rotate_left(42) ^ d.rotate_left(63)).wrapping_mul(FXK)
}

/// `ptr` から 32 バイトを YMM ロードし、clen より後ろのバイトをゼロにした正規形
/// キーを返す。長さマスクは iota ベクタとの符号つき比較で作るのでテーブルを引かない。
/// clen は 1..=32 に絞られている (それ以外は cold 表)。
#[inline]
#[target_feature(enable = "avx2")]
unsafe fn mask_key_avx2(ptr: *const u8, clen: u32) -> std::arch::x86_64::__m256i {
    use std::arch::x86_64::*;
    unsafe {
        let raw = _mm256_loadu_si256(ptr as *const __m256i);
        #[rustfmt::skip]
        let iota = _mm256_setr_epi8(
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        );
        // clen > i のレーンだけ 0xFF、すなわち i < clen のバイトだけを残す。
        let keep = _mm256_cmpgt_epi8(_mm256_set1_epi8(clen as i8), iota);
        _mm256_and_si256(raw, keep)
    }
}

struct Table {
    slots32: Box<[Key32]>,
    slot_len: Box<[u8]>,
    // 毎行 RMW するホット加算器 (8B)。
    hot: Box<[[Hot; 12]]>,
    // 毎行読み、稀に書く極値表 (4B)。
    mm: Box<[[MinMax; 12]]>,
    // 飽和退避と u16 幅に収まらない行を (スロット添字, month) 単位に集める側表。
    spill: HashMap<(u32, u8), Stats32>,
    // clen > 32 のキーを退避する cold 表。
    cold: ColdTable,
}

impl Table {
    fn new() -> Self {
        Table {
            slots32: boxed_zeroed_slice::<Key32>(CAP),
            slot_len: boxed_zeroed_slice::<u8>(CAP),
            hot: boxed_zeroed_slice::<[Hot; 12]>(CAP),
            mm: boxed_zeroed_slice::<[MinMax; 12]>(CAP),
            spill: HashMap::new(),
            cold: ColdTable::new(),
        }
    }

    /// 32 バイトのマスク済みキーをスロットへ対応づけ、スロット添字 (= id) を返す。
    /// YMM 1 本の比較で一致を判定し、空スロット (slot_len == 0) に挿入する。マスク
    /// 済みキーは全ゼロにならない (clen >= 1) ので、空スロットの全ゼロ値と取り違える
    /// ことはなく、比較だけで一致判定が完結する。
    #[inline]
    #[target_feature(enable = "avx2")]
    unsafe fn intern(&mut self, key: std::arch::x86_64::__m256i, h: u64) -> usize {
        use std::arch::x86_64::*;
        unsafe {
            let mut idx = (h >> 49) as usize & MASK;
            loop {
                let stored =
                    _mm256_loadu_si256(self.slots32.as_ptr().add(idx) as *const __m256i);
                if _mm256_movemask_epi8(_mm256_cmpeq_epi8(stored, key)) == -1 {
                    return idx;
                }
                if *self.slot_len.get_unchecked(idx) == 0 {
                    _mm256_storeu_si256(self.slots32.as_mut_ptr().add(idx) as *mut __m256i, key);
                    // マスク済みキーの最初のゼロバイト位置がそのまま長さになる。
                    let zmask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(key, _mm256_setzero_si256()))
                        as u32;
                    let len = if zmask == 0 { 32 } else { zmask.trailing_zeros() as u8 };
                    *self.slot_len.get_unchecked_mut(idx) = len;
                    *self.hot.get_unchecked_mut(idx) = [Hot::new(); 12];
                    *self.mm.get_unchecked_mut(idx) = [MinMax::new(); 12];
                    return idx;
                }
                idx = (idx + 1) & MASK;
            }
        }
    }

    /// 1 行を集計する。u16 幅に収まらない稀な行は側表へ直行させ、残りは飽和判定の
    /// 上でホット 8 バイトを u64 1 本の加算で更新する。
    #[inline(always)]
    unsafe fn update_row(&mut self, idx: usize, month: usize, mlen: u32, stamp: u32) {
        // 値そのものが u16 フィールドに収まらない行 (mlen が min/max の u16 幅を
        // 超える、または stamp が u16 を超える) は全幅の側表へ直に積み、ホットには
        // 触れない (ほぼ来ないので完全に予測される)。
        if mlen >= 0xFFFF || stamp > 0xFFFF {
            self.spill_row(idx, month, mlen, stamp);
            return;
        }
        // ホット 8 バイトを u64 1 語として読む (total_len@0 / count@4 / stamp@6)。
        let cur = unsafe {
            (self.hot.get_unchecked(idx).as_ptr().add(month) as *const u64).read_unaligned()
        };
        // 飽和判定: いずれかのフィールドが今回の加算で桁上げするなら、退避して
        // 空の状態から積み直す。
        let overflow = ((cur >> 32) as u16) == u16::MAX
            || stamp > (u16::MAX as u32) - ((cur >> 48) as u16 as u32)
            || mlen > u32::MAX - (cur as u32);
        if overflow {
            self.flush_hot(idx, month);
        }
        // count は +1、total_len は +mlen、stamp は +stamp。飽和判定が桁上げの不在を
        // 保証するので、u64 1 本の加算で 3 フィールドを同時に更新できる。
        let addend = (mlen as u64) | (1u64 << 32) | ((stamp as u64) << 48);
        let base = if overflow { 0 } else { cur };
        unsafe {
            (self.hot.get_unchecked_mut(idx).as_mut_ptr().add(month) as *mut u64)
                .write_unaligned(base.wrapping_add(addend));
        }
        unsafe { self.update_minmax(idx, month, mlen as u16) };
    }

    /// min / max の更新。毎行読むが、書くのは新しい極値のときだけである。定常状態
    /// では極値の更新はほぼ止まるので、この 2 分岐は「取られない」側で安定し予測を
    /// 外さない。
    #[inline(always)]
    unsafe fn update_minmax(&mut self, idx: usize, month: usize, m16: u16) {
        let p = unsafe { self.mm.get_unchecked_mut(idx).as_mut_ptr().add(month) };
        unsafe {
            if m16 < (*p).min {
                (*p).min = m16;
            }
            if m16 > (*p).max {
                (*p).max = m16;
            }
        }
    }

    /// 飽和したホットエントリの加算器を側表へ移してゼロクリアする。min / max は
    /// 退避しない。極値は順序非依存の縮約なので、走行中の値を保ったまま出力時に
    /// 側表側と合成すればよい。頻度ゼロ前提の out-of-line。
    #[cold]
    #[inline(never)]
    fn flush_hot(&mut self, idx: usize, month: usize) {
        let (c, tl, st) = unsafe {
            let h = self.hot.get_unchecked_mut(idx).get_unchecked_mut(month);
            let v = (h.count as u32, h.total_len, h.stamp as u32);
            h.total_len = 0;
            h.count = 0;
            h.stamp = 0;
            v
        };
        let e = self
            .spill
            .entry((idx as u32, month as u8))
            .or_insert_with(Stats32::new);
        e.count += c;
        e.total_length += tl;
        e.total_stamp += st;
    }

    /// u16 幅に収まらない 1 行を全幅の側表へ積む。min / max も全幅で持つので
    /// mlen >= 0xFFFF でも正確に受けられる。頻度ゼロ前提の out-of-line。
    #[cold]
    #[inline(never)]
    fn spill_row(&mut self, idx: usize, month: usize, mlen: u32, stamp: u32) {
        self.spill
            .entry((idx as u32, month as u8))
            .or_insert_with(Stats32::new)
            .add(mlen, stamp);
    }
}

// ---------------------------------------------------------------------------
// 行エンジン 4/4: バッチ 3 パスのステージ分割。
//
// 1 行ずつ「パース → 表引き → 集計」を直列に流すと、表のランダムロード (L2/L3
// レイテンシ) が行ごとに完全露出して律速する。かといって複数行をレジスタ上で丸ごと
// 並走させると、行全体の生存状態がレジスタを溢れ、スピルが利得を食い潰す。そこで
// 行の状態をスタック上の小配列に退避し、
//
//   パス 1: BATCH 行をパースし、プローブ先スロットのラインを prefetch する
//   パス 2: intern する (スロットは prefetch 済みでレイテンシがほぼ隠れている)。
//           続けて集計先 hot / mm のラインを prefetch する
//   パス 3: 集計を更新する
//
// と段階ごとに一括処理する。各パスの生存状態は少数の変数に収まり、ランダムロードは
// 常に数十行ぶん先行して発行されるため、レイテンシがバッチ内で重なり合う。ハッシュ
// 結合の文献ではこの形 (group prefetching) でプローブが 3〜4 倍速くなることが
// 知られている。パス 2 と 3 を分けているのは prefetch 距離のためである。intern の
// 直後にその行の集計を触ると先読みが間に合わないが、パスを分ければ集計の先読みから
// その消費まで最大バッチ全幅 (48 行) の距離が取れる。
//
// 高速経路は「範囲の先に十分な余白がある」ことを呼び出し側が保証する契約で動く
// (main の MARGIN 参照)。8 バイトロードも 64 バイトブロック読みも区切りの先まで
// 余分に読むが、この契約によりホットループから境界チェックを完全に排除できる。
// ---------------------------------------------------------------------------
const BATCH: usize = 48;

/// バッチのパス 2/3。パス 1 が埋めた行状態配列を受け取り、intern して集計ラインを
/// prefetch し、続けて集計を更新する。
#[inline(always)]
unsafe fn flush_batch(
    table: &mut Table,
    n: usize,
    key32_a: &[Key32; BATCH],
    hash_a: &[u64; BATCH],
    month_a: &[u8; BATCH],
    mlen_a: &[u32; BATCH],
    stamp_a: &[u32; BATCH],
    id_a: &mut [usize; BATCH],
) {
    use std::arch::x86_64::{__m256i, _MM_HINT_T0, _mm256_loadu_si256, _mm_prefetch};

    // パス 2: intern と集計ラインの prefetch。加算器 (hot) と極値表 (mm) は別配列
    // なので 2 本先読みし、パス 3 でどちらのランダムロードも露出しないようにする。
    for k in 0..n {
        let key = unsafe { _mm256_loadu_si256(key32_a[k].0.as_ptr() as *const __m256i) };
        let id = unsafe { table.intern(key, hash_a[k]) };
        id_a[k] = id;
        let line = unsafe {
            (table.hot.as_ptr().add(id) as *const u8)
                .add(month_a[k] as usize * core::mem::size_of::<Hot>())
        };
        unsafe { _mm_prefetch(line as *const i8, _MM_HINT_T0) };
        let mm_line = unsafe {
            (table.mm.as_ptr().add(id) as *const u8)
                .add(month_a[k] as usize * core::mem::size_of::<MinMax>())
        };
        unsafe { _mm_prefetch(mm_line as *const i8, _MM_HINT_T0) };
    }

    // パス 3: 集計更新。飽和と幅超えの側表行きは update_row 内 (ほぼ取られず
    // 完全予測)。
    for k in 0..n {
        unsafe { table.update_row(id_a[k], month_a[k] as usize, mlen_a[k], stamp_a[k]) };
    }
}

/// 高速経路の本体。バッチ 3 パス構造で、パス 1 の境界検出に構造マスクを、数値
/// パースにベクタ連鎖を使う。
#[target_feature(enable = "avx2")]
unsafe fn process_fast(
    base: *const u8,
    start: usize,
    end: usize,
    month_table: &[u8; 366],
    base_day: i64,
    table: &mut Table,
) {
    use std::arch::x86_64::{__m256i, _MM_HINT_T0, _mm256_storeu_si256, _mm_prefetch};

    // バッチ内の行状態。レジスタ圧を避けるため、配列に退避した生存状態である。
    // キー本体を正規形で持つので、生ポインタや長さ (clen) をバッチに残す必要は
    // ない。mlen / stamp はパス 3 の update_row が食う。
    let mut key32_a = [Key32([0u8; 32]); BATCH];
    let mut hash_a = [0u64; BATCH];
    let mut month_a = [0u8; BATCH];
    let mut mlen_a = [0u32; BATCH];
    let mut stamp_a = [0u32; BATCH];
    let mut id_a = [0usize; BATCH];

    let slots32_ptr = table.slots32.as_ptr();

    // 構造マスクのカーソル。現ブロックの基点とマスクの 2 変数だけを持つ。
    let mut sblock = start;
    let mut smask = unsafe { block_mask_avx2(base, start) };

    let mut i = start;
    while i < end {
        // パス 1: パースとスロット prefetch。
        let mut n = 0usize;
        while n < BATCH && i < end {
            // timestamp は行頭固定 10 桁で、区切り探索が要らない。上位 8 桁は
            // 後段の parse3x8 でベクタパースするため、ここでは下位 2 桁だけ読む。
            let ts_w = unsafe { load_u64(base.add(i)) };
            let d9 = (unsafe { *base.add(i + 8) } - b'0') as u64;
            let d10 = (unsafe { *base.add(i + 9) } - b'0') as u64;

            // timestamp 終端のカンマ。位置は行頭 +10 と分かっているが、マスクを行に
            // 同期させ続けるため、値を使わなくても必ず 1 ビット消費する。
            let ts_comma = unsafe { next_delim(base, &mut smask, &mut sblock) };
            let ch_start = ts_comma + 1;

            // channel 終端のカンマ。境界検出はレジスタ内演算だけで済む。
            let ch_comma = unsafe { next_delim(base, &mut smask, &mut sblock) };
            let clen = (ch_comma - ch_start) as u32;

            // message_length と stamp_count の範囲をマスクから確定する。
            let mlen_start = ch_comma + 1;
            let mlen_comma = unsafe { next_delim(base, &mut smask, &mut sblock) };
            let mlen_digits = mlen_comma - mlen_start;
            let stamp_start = mlen_comma + 1;
            let nl = unsafe { next_delim(base, &mut smask, &mut sblock) };
            let stamp_digits = nl - stamp_start;

            // 次行へ。改行の次バイトが次行の先頭になる。
            i = nl + 1;

            // ts 上位 8 桁と mlen と stamp を 1 本のベクタ連鎖でまとめて
            // パースする。9 桁以上 (u32 全域を取るための稀な入力) のときだけ桁数
            // ループを持つ経路へ丸ごと退避する (ほぼ来ないので完全に予測される)。
            let (ts8, mlen, stamp) = if mlen_digits <= 8 && stamp_digits <= 8 {
                let mlen_norm = unsafe { norm8(base, mlen_start, mlen_digits) };
                let stamp_norm = unsafe { norm8(base, stamp_start, stamp_digits) };
                parse3x8(ts_w, mlen_norm, stamp_norm)
            } else {
                let mlen = unsafe { parse_num(base, mlen_start, mlen_digits) };
                let stamp = unsafe { parse_num(base, stamp_start, stamp_digits) };
                (parse8(ts_w), mlen, stamp)
            };
            let ts = ts8 as u64 * 100 + d9 * 10 + d10;

            // 月の決定。86400 での除算は定数除算なのでコンパイラが乗算に落とす。
            let day = (ts / 86400) as i64 - base_day;
            let month = unsafe { *month_table.get_unchecked(day as usize) };

            // clen > 32 のキーは cold 表へ丸ごと退避する (ほぼ来ないので
            // 完全に予測される)。バッチには積まないので n も進めない。
            if clen > 32 {
                unsafe {
                    table
                        .cold
                        .add(base.add(ch_start), clen as usize, month as usize, mlen, stamp)
                };
                continue;
            }

            // 32 バイトのゼロマスク値を正規形キーにし、そこからハッシュを導く。
            // キーは 1 ストアでバッチ配列に置き、4 ロードでハッシュのレーンを取る。
            let key = unsafe { mask_key_avx2(base.add(ch_start), clen) };
            unsafe { _mm256_storeu_si256(key32_a[n].0.as_mut_ptr() as *mut __m256i, key) };
            let h = unsafe { hash_key32(key32_a[n].0.as_ptr()) };

            // プローブ先のスロットラインを今のうちに取り寄せておく。パス 2 が
            // ここへ戻ってくる頃には L1/L2 に届いている。衝突で隣スロットへ歩く
            // 場合はあるが、線形探索の隣は同じか次のラインなので先読みの恩恵は残る。
            let idx = (h >> 49) as usize & MASK;
            unsafe { _mm_prefetch(slots32_ptr.add(idx) as *const i8, _MM_HINT_T0) };

            hash_a[n] = h;
            month_a[n] = month;
            mlen_a[n] = mlen;
            stamp_a[n] = stamp;
            n += 1;
        }

        // パス 2/3。
        unsafe {
            flush_batch(table, n, &key32_a, &hash_a, &month_a, &mlen_a, &stamp_a, &mut id_a);
        }
    }
}

/// 完全境界チェックつきのスカラ処理。ファイル末尾付近 (と極小入力) では高速
/// 経路の先読みがマップ範囲を越え得るため、末尾の一定範囲だけはこの安全版で処理
/// する。二段構えにすることで、大部分を占める高速経路から境界チェックを取り除き
/// つつ全体の正しさを保つ。
#[target_feature(enable = "avx2")]
unsafe fn process_scalar(
    body: &[u8],
    mut i: usize,
    end: usize,
    month_table: &[u8; 366],
    base_day: i64,
    table: &mut Table,
) {
    use std::arch::x86_64::{__m256i, _mm256_loadu_si256};
    while i < end {
        let mut ts: u64 = 0;
        while body[i] != b',' {
            ts = ts * 10 + (body[i] - b'0') as u64;
            i += 1;
        }
        i += 1;
        let ch_start = i;
        while body[i] != b',' {
            i += 1;
        }
        let clen = i - ch_start;
        i += 1;
        let mut mlen: u32 = 0;
        while body[i] != b',' {
            mlen = mlen * 10 + (body[i] - b'0') as u32;
            i += 1;
        }
        i += 1;
        let mut stamp: u32 = 0;
        while body[i] != b'\n' {
            stamp = stamp * 10 + (body[i] - b'0') as u32;
            i += 1;
        }
        i += 1;

        let day = (ts / 86400) as i64 - base_day;
        let month = month_table[day as usize] as usize;

        // 末尾域はマップ範囲の先読みを避けるため、キーを整列バッファへ複製して
        // 正規形を組み立てる。高速経路の mask_key_avx2 と同一のキー/ハッシュに
        // なるので id も一致する。clen > 32 は cold 表へ。集計は高速経路と同じ
        // update_row を通り、飽和と幅超えの扱いも一致する。
        if clen > 32 {
            unsafe { table.cold.add(body.as_ptr().add(ch_start), clen, month, mlen, stamp) };
            continue;
        }
        let mut kb = Key32([0u8; 32]);
        kb.0[..clen].copy_from_slice(&body[ch_start..ch_start + clen]);
        let key = unsafe { _mm256_loadu_si256(kb.0.as_ptr() as *const __m256i) };
        let h = unsafe { hash_key32(kb.0.as_ptr()) };
        let idx = unsafe { table.intern(key, h) };
        unsafe { table.update_row(idx, month, mlen, stamp) };
    }
}

// ---------------------------------------------------------------------------
// 並列化とマージ。
//
// 入力は行境界に沿ってスレッド数分に等分し (分割は main が行う)、各スレッドが
// 独立の Table へ集計して最後に 1 つへ畳み込む。共有状態を持たないので、行処理は
// ロックもアトミックも使わない。マージの仕事量はキー最大 1 万 × 表の数程度で、
// 行数に比べれば無視できる。
// ---------------------------------------------------------------------------

/// スレッドごとの表を 1 つに畳み込む。各表の占有スロットを舐めてキー本体を merged
/// へ intern し直し、加算器と極値表と側表を、gid で引く全幅アキュムレータへ合成
/// する。cold 表は所有キーで直接マージする。intern は冪等なので、側表の張り替えは
/// 同じキーを引き直すだけで済み、ローカル id から gid への対応表は要らない。返り値
/// は「キーを持つ merged 表」と「gid で引く全幅集計」の対である。
///
/// 合成が正確なのは部分和の性質による。件数・長さ・スタンプの各部分和は同一
/// チャンネル×月の合計 (仕様上 u32 に収まる) の一部で、加算は結合則を満たすので、
/// 飽和退避でどう分割されていても総和は一意になる。min / max は冪等かつ順序非依存
/// の縮約なので、ホット側の走行極値と側表の全幅極値を任意の順で畳んでよい (min の
/// 番兵 0xFFFF だけは「ホットに積まれた行なし」の印なので合成から除く)。u16 幅に
/// 収まらない行は側表にのみ全幅で入っているから、取りこぼしはない。
#[target_feature(enable = "avx2")]
unsafe fn merge_tables(tables: &[Table]) -> (Table, Box<[[Stats32; 12]]>) {
    use std::arch::x86_64::{__m256i, _mm256_loadu_si256};
    let mut merged = Table::new();
    // 全幅アキュムレータ。min の番兵 u32::MAX を含むので、ゼロ確保ではなく new()
    // で埋める。
    let mut acc: Box<[[Stats32; 12]]> = vec![[Stats32::new(); 12]; CAP].into_boxed_slice();
    for t in tables {
        for idx in 0..CAP {
            let len = unsafe { *t.slot_len.get_unchecked(idx) };
            if len == 0 {
                continue;
            }
            let kp = unsafe { t.slots32.get_unchecked(idx).0.as_ptr() };
            let key = unsafe { _mm256_loadu_si256(kp as *const __m256i) };
            let h = unsafe { hash_key32(kp) };
            let gid = unsafe { merged.intern(key, h) };
            let dst = unsafe { acc.get_unchecked_mut(gid) };
            for m in 0..12 {
                let cell = unsafe { t.hot.get_unchecked(idx).get_unchecked(m) };
                dst[m].count += cell.count as u32;
                dst[m].total_length += cell.total_len;
                dst[m].total_stamp += cell.stamp as u32;
                let mm = unsafe { t.mm.get_unchecked(idx).get_unchecked(m) };
                if mm.min != u16::MAX && (mm.min as u32) < dst[m].min {
                    dst[m].min = mm.min as u32;
                }
                if (mm.max as u32) > dst[m].max {
                    dst[m].max = mm.max as u32;
                }
                if let Some(s) = t.spill.get(&(idx as u32, m as u8)) {
                    dst[m].merge(s);
                }
            }
        }
        for (channel, arr) in &t.cold.map {
            let dst = merged
                .cold
                .map
                .entry(channel.clone())
                .or_insert([Stats32::new(); 12]);
            for m in 0..12 {
                dst[m].merge(&arr[m]);
            }
        }
    }
    (merged, acc)
}

// ---------------------------------------------------------------------------
// 出力と丸め。
//
// 出力全体を 1 つのバッファに組み立て、write 1 回で書き出す。avg だけは仕様が
// C の printf("%.2f") と同じ丸めを要求するので std の整形に任せ、残りは自前の
// 整数整形で済ませる。
// ---------------------------------------------------------------------------

/// 12 の月スロットそれぞれに対応する (年, 月) の出力ラベルを作る。
fn build_labels() -> [(i64, u32); 12] {
    let mut labels = [(0i64, 0u32); 12];
    for m in 0..12u32 {
        let days = days_from_civil(YEAR, m + 1, 1);
        labels[m as usize] = civil_from_days(days);
    }
    labels
}

/// `buf` に ",YYYY-MM=min/avg/max/count/total_stamp\n" を追記する。
#[inline]
fn append_line(
    buf: &mut Vec<u8>,
    year: i64,
    month: u32,
    min: u32,
    avg: f64,
    max: u32,
    count: u32,
    total_stamp: u32,
) {
    buf.push(b',');
    append_u32(buf, year as u32);
    buf.push(b'-');
    if month < 10 {
        buf.push(b'0');
    }
    append_u32(buf, month);
    buf.push(b'=');
    append_u32(buf, min);
    buf.push(b'/');
    append_avg(buf, avg);
    buf.push(b'/');
    append_u32(buf, max);
    buf.push(b'/');
    append_u32(buf, count);
    buf.push(b'/');
    append_u32(buf, total_stamp);
    buf.push(b'\n');
}

#[inline]
fn append_u32(buf: &mut Vec<u8>, v: u32) {
    let mut tmp = [0u8; 10];
    let mut n = v;
    let mut i = 10;
    loop {
        i -= 1;
        tmp[i] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    buf.extend_from_slice(&tmp[i..]);
}

/// `v` を小数第 2 位までの固定小数点表記にする。仕様は C の printf("%.2f") と
/// 同じ丸め (f64 の真値に基づく最近接偶数丸め) を要求しており、Rust の
/// `format!("{:.2}")` はこれに一致するので、この 1 箇所だけ std の整形に任せる。
#[inline]
fn append_avg(buf: &mut Vec<u8>, v: f64) {
    use std::io::Write as _;
    write!(buf, "{:.2}", v).unwrap();
}

// ---------------------------------------------------------------------------
// エントリポイント。入力を mmap し、範囲を分割して並列に集計し、マージして
// 出力する。
// ---------------------------------------------------------------------------
fn main() {
    // 高速経路は AVX2 前提で組んである。計測環境では常に真であり、この検査は
    // 前提が破れた環境で黙って壊れないための 1 回きりの防壁である。
    assert!(
        std::arch::is_x86_feature_detected!("avx2"),
        "this binary requires AVX2"
    );

    let mut args = std::env::args_os().skip(1);
    let input = args.next().expect("usage: program <input.csv> <output.txt>");
    let output = args.next().expect("usage: program <input.csv> <output.txt>");

    // 入力を読み取り専用で開いて mmap する。
    let file = std::fs::File::open(&input).expect("failed to open input file");
    let file_len = file.metadata().expect("fstat failed").len() as usize;

    let base_day = days_from_civil(YEAR, 1, 1);
    let month_table = build_month_table(base_day);
    let labels = build_labels();

    if file_len == 0 {
        std::fs::write(&output, b"").expect("failed to write output file");
        return;
    }

    let map_ptr = unsafe {
        mmap(
            core::ptr::null_mut(),
            file_len,
            PROT_READ,
            MAP_PRIVATE,
            file.as_raw_fd(),
            0,
        )
    };
    assert!(map_ptr != MAP_FAILED, "mmap failed");
    unsafe {
        madvise(map_ptr, file_len, MADV_SEQUENTIAL);
        madvise(map_ptr, file_len, MADV_WILLNEED);
    }

    let map = unsafe { std::slice::from_raw_parts(map_ptr as *const u8, file_len) };

    // ヘッダー行を読み飛ばす。仕様上ちょうど 1 行あることが保証されている。
    let body_start = match map.iter().position(|&b| b == b'\n') {
        Some(p) => p + 1,
        None => file_len,
    };
    let body = &map[body_start..];
    let body_len = body.len();
    let base = body.as_ptr();

    // 末尾に安全域を確保する。高速経路は区切りの先まで余分に読むため、ファイル
    // 終端の手前 MARGIN バイト (最長行と先読み幅の合計より大きい) だけは境界チェック
    // つきのスカラ版で処理し、先読みがマップ範囲を越えないことを保証する。
    const MARGIN: usize = 160;
    let fast_end = if body_len > MARGIN {
        let mut t = body_len - MARGIN;
        while t > 0 && body[t - 1] != b'\n' {
            t -= 1;
        }
        t
    } else {
        0
    };

    let nthreads = thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);

    // 高速領域 [0, fast_end) を行境界に沿ってスレッド数分に等分する。
    let mut ranges: Vec<(usize, usize)> = Vec::with_capacity(nthreads);
    if fast_end > 0 {
        let approx = fast_end / nthreads;
        let mut start = 0usize;
        for t in 0..nthreads {
            if start >= fast_end {
                break;
            }
            let end = if t == nthreads - 1 {
                fast_end
            } else {
                let mut e = (start + approx).min(fast_end);
                while e < fast_end && body[e - 1] != b'\n' {
                    e += 1;
                }
                e
            };
            ranges.push((start, end));
            start = end;
        }
    }

    let base_addr = base as usize;
    let month_table_ref = &month_table;

    let mut tables: Vec<Table> = thread::scope(|scope| {
        let handles: Vec<_> = ranges
            .iter()
            .map(|&(s, e)| {
                scope.spawn(move || {
                    let mut table = Table::new();
                    let bp = base_addr as *const u8;
                    unsafe { process_fast(bp, s, e, month_table_ref, base_day, &mut table) };
                    table
                })
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });

    // 末尾の安全域を処理する。fast_end が 0 になる極小入力もここが拾う。
    let mut tail_table = Table::new();
    if fast_end < body_len {
        unsafe {
            process_scalar(
                body,
                fast_end,
                body_len,
                &month_table,
                base_day,
                &mut tail_table,
            )
        };
    }
    tables.push(tail_table);

    // スレッドごとの表を 1 つに統合する。
    let (merged, acc) = unsafe { merge_tables(&tables) };

    // 出力を組み立てる。採点は行順を無視するので、整列はせずスロット順のまま出す。
    let nkeys = merged.slot_len.iter().filter(|&&l| l != 0).count() + merged.cold.map.len();
    let mut buf: Vec<u8> = Vec::with_capacity(nkeys * 12 * 48 + 64);
    for idx in 0..CAP {
        let len = merged.slot_len[idx] as usize;
        if len == 0 {
            continue;
        }
        let channel = &merged.slots32[idx].0[..len];
        let arr = &acc[idx];
        for m in 0..12 {
            // 全幅アキュムレータには加算器と極値表と側表が合成済みで、count > 0
            // なら min / max は必ず実値で埋まっている。直に整形するだけでよい。
            let s = &arr[m];
            if s.count == 0 {
                continue;
            }
            let (year, month) = labels[m];
            let avg = s.total_length as f64 / s.count as f64;
            buf.extend_from_slice(channel);
            append_line(&mut buf, year, month, s.min, avg, s.max, s.count, s.total_stamp);
        }
    }

    // cold 表のチャンネル (clen > 32) を出す。全幅 Stats32 を直に整形する。
    for (channel, arr) in &merged.cold.map {
        for m in 0..12 {
            let e = &arr[m];
            if e.count == 0 {
                continue;
            }
            let (year, month) = labels[m];
            let avg = e.total_length as f64 / e.count as f64;
            buf.extend_from_slice(channel);
            append_line(&mut buf, year, month, e.min, avg, e.max, e.count, e.total_stamp);
        }
    }

    std::fs::write(&output, &buf).expect("failed to write output file");

    // 後始末はしない。表や出力バッファの解放とマップの破棄はプロセス終了時に OS が
    // まとめて行うので、ここで Drop を走らせるのは時間の無駄である。
    std::process::exit(0);
}
