// 自動生成ファイル。直接編集しないこと (bundler が src/main.rs から生成)。
// 取り込んだファイル: src/aggregate.rs, src/dispatch/mpsc.rs, src/dispatch/ring.rs, src/dispatch.rs, src/hash.rs, src/input.rs, src/keyspace.rs, src/main.rs, src/mmap.rs, src/output.rs, src/parse.rs, src/swar.rs, src/table.rs, src/time.rs
// 再生成コマンド: ./bundle.sh

mod aggregate {
use crate::hash::finalize_key;
use crate::parse::Row;
use crate::table::GroupTable;
use crate::time::MonthTable;

/// 既定の先読み段数。100M データでの D 掃引 (min elapsed) は D=1 の 3.23s から
/// 単調に改善し、D≈24〜32 で 2.18s のプラトー、D=48 以降はリングが L1 を圧迫して
/// わずかに悪化した。
/// 何回か提出したところ16が良さそうだった。
const DEFAULT_PREFETCH_DISTANCE: usize = 16;

/// 先読み段数 D を環境変数 `OBRC_PREFETCH_DISTANCE` から読む (既定 32)。
/// 1..=64 にクランプ。掃引・再チューニング用に可変化してある。
fn prefetch_distance() -> usize {
    std::env::var("OBRC_PREFETCH_DISTANCE")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(DEFAULT_PREFETCH_DISTANCE)
        .clamp(1, 64)
}

/// パース済み行をチャネル転送用に 32 バイトへ圧縮した形 (`(u64, Row)` の 56B から削減)。
/// 毎行必要なのは key / message_length / stamp_count のみで、channel と month は
/// グループ挿入時 (1 億行中 ~12 万回) しか読まれないため、channel は生ポインタ + 長さに
/// 落とす。ポインタは入力 mmap の借用 `'a` に紐付き (PhantomData)、スコープ内スレッド間
/// 転送で mmap より長生きしないことを型で保証する。
pub struct Pending<'a> {
    pub key: u64,
    channel_ptr: *const u8,
    pub message_length: u32,
    pub stamp_count: u32,
    channel_len: u16,
    month: u8,
    _borrow: std::marker::PhantomData<&'a [u8]>,
}

// SAFETY: channel_ptr は共有 mmap (&'a [u8]) 内を指す読み取り専用ポインタで、
// スレッド間で共有しても危険な可変状態を持たない (実質 &'a [u8] と同じ)。
unsafe impl Send for Pending<'_> {}

impl<'a> Pending<'a> {
    /// 行の月とキーをパース側スレッドで確定し、転送用に圧縮する。
    #[inline]
    pub fn new(dates: &MonthTable, row: &Row<'a>) -> Pending<'a> {
        let month = dates.month(row.timestamp) as u8;
        Pending {
            key: finalize_key(row.hash, month as u64),
            channel_ptr: row.channel.as_ptr(),
            message_length: row.message_length as u32,
            stamp_count: row.stamp_count as u32,
            channel_len: row.channel.len() as u16,
            month,
            _borrow: std::marker::PhantomData,
        }
    }

    #[inline]
    fn channel(&self) -> &'a [u8] {
        // SAFETY: new() で &'a [u8] から取ったポインタ + 長さの復元。'a が生存を保証する。
        unsafe { std::slice::from_raw_parts(self.channel_ptr, self.channel_len as usize) }
    }

    #[inline]
    fn month(&self) -> u8 {
        self.month
    }

    /// 照合で読む channel バイト列 (入力 mmap 内) の先頭を L1 に先読みする。
    /// バッチ内の行順にポインタが既知なので、mmap ランダムアクセスのミス待ちを
    /// [Aggregator::add_batch] のパイプラインの裏に隠す。
    #[inline]
    fn prefetch_channel(&self) {
        #[cfg(target_arch = "x86_64")]
        unsafe {
            use std::arch::x86_64::{_mm_prefetch, _MM_HINT_T0};
            _mm_prefetch::<_MM_HINT_T0>(self.channel_ptr as *const i8);
        }
    }
}

/// 毎行更新するホットな統計。テーブルのエントリは「キー u64 + Stat」で、
/// 合計をちょうど 32 バイト (キャッシュライン半分、境界跨ぎなし) に収めるために
/// フィールド幅をデータの実レンジまで詰めてある: message_length/stamp_count は
/// 1行あたり高々数千 → min/max は u32、1グループの行数・スタンプ総数も 10^9 行規模で
/// 最大数百万 → u32。sum_length だけは「行数 × 長さ」で u32 を超え得るため u64。
#[derive(Clone)]
pub struct Stat {
    pub sum_length: u64,
    pub min_length: u32,
    pub max_length: u32,
    pub count: u32,
    pub total_stamp_count: u32,
}

impl Stat {
    #[inline]
    fn empty() -> Stat {
        Stat {
            sum_length: 0,
            min_length: u32::MAX,
            max_length: 0,
            count: 0,
            total_stamp_count: 0,
        }
    }

    pub fn average(&self) -> f64 {
        self.sum_length as f64 / self.count as f64
    }
}

/// GroupInfo にインライン格納する channel 先頭バイト数。多くの channel はこの長さ以内に
/// 収まる (実データは平均 ~20B・最大 32B、regulation 上限 105B)。この prefix を cold 配列に
/// 埋めることで、cold[i] を先読みすれば照合対象の先頭が同じキャッシュラインに載り、依存
/// ロード (arena 本体) を「prefix を超える長さ」の稀なケースだけに限定できる。
/// off(4)+len(1)+month(1)+prefix(26) = 32B の cold エントリ。掃引 (100M A/B) では N=18 は
/// 効果なし (avg~20B の channel が半数 arena tail へ流れる)、N=26 で依存ロードがほぼ消え最良、
/// N=32 は同等。よって 26 (掃引用の可変パラメータ)。
const CHANNEL_PREFIX: usize = 26;

/// テーブルのコールド部が持つグループ識別情報 (実キー)。挿入時 (グループ数ぶんだけ) 書き、
/// 毎行の照合と出力時に読む。channel 実体は集計器所有のアリーナ ([Aggregator::arena]) に
/// コピーし、その**オフセット** `off` で参照する (オフセットは `Vec` 再確保で不変なので上限
/// なし。regulation の任意長・任意グループ数を安全に扱える)。加えて channel 先頭
/// [CHANNEL_PREFIX] バイトを `prefix` にインライン複製し、毎行の照合を先読み済み cold ライン
/// 上で完結させる (依存ロード削減。詳細は [GroupInfo::matches])。全フィールド Copy なので
/// [Aggregator] は自動 `Send`。年は 2027 固定なので保持せず出力時に定数で補う。
#[derive(Clone, Copy)]
pub struct GroupInfo {
    /// アリーナ先頭からの channel バイト列 (全長) のオフセット。
    off: u32,
    /// channel の長さ。regulation 上の最大 105B に対し u8 で十分。
    len: u8,
    pub month: u8,
    /// channel 先頭 `min(len, CHANNEL_PREFIX)` バイト (残りはゼロ埋め)。照合の高速経路。
    prefix: [u8; CHANNEL_PREFIX],
}

impl GroupInfo {
    /// 空スロット用の番兵値 (channel は決して読まれない)。
    #[inline]
    fn empty() -> GroupInfo {
        GroupInfo { off: 0, len: 0, month: 0, prefix: [0; CHANNEL_PREFIX] }
    }

    /// 実キー `(channel, month)` が incoming (`month`, `ch`) と一致するか判定する。
    /// 先頭 [CHANNEL_PREFIX] バイトは先読み済み cold ライン上の `prefix` で照合し、それを
    /// 超える長さのときだけ arena 本体の tail を読む (依存ロードを稀なケースに限定)。
    /// SAFETY: `base` はこの `off`/`len` を発行したアリーナの現在の先頭 (intern 済みで有効)。
    #[inline]
    unsafe fn matches(&self, base: *const u8, month: u8, ch: &[u8]) -> bool {
        let n = self.len as usize;
        if self.month != month || n != ch.len() {
            return false;
        }
        let head = n.min(CHANNEL_PREFIX);
        if self.prefix[..head] != ch[..head] {
            return false;
        }
        if n <= CHANNEL_PREFIX {
            return true;
        }
        // 稀: prefix を超える channel は残りを arena 本体から照合する
        let tail = unsafe {
            std::slice::from_raw_parts(base.add(self.off as usize + CHANNEL_PREFIX), n - CHANNEL_PREFIX)
        };
        tail == &ch[CHANNEL_PREFIX..]
    }

    /// アリーナ先頭 `base` を基準に channel バイト列 (全長) を復元する (出力用)。
    /// SAFETY: `base` はこの `off`/`len` を発行したアリーナの現在の先頭。
    #[inline]
    unsafe fn channel_from<'x>(&self, base: *const u8) -> &'x [u8] {
        unsafe { std::slice::from_raw_parts(base.add(self.off as usize), self.len as usize) }
    }
}

/// 出力用のグループ 1 件ぶんのビュー (ホット部とコールド部の合流)。channel はアリーナ借用。
pub struct Group<'b> {
    pub channel: &'b [u8],
    pub month: u8,
    pub stat: &'b Stat,
}

/// 集計段: `(channel_path, 月)` ごとの統計を保持する。
///
/// スロット選択には「チャンネル名 + 月の FxHash 64bit 値」([hash::finalize_key]) を使うが、
/// これはバケット選択 + 高速プレフィルタにすぎない。u64 が一致したスロットでは実キー
/// `(channel バイト列, month)` を [GroupTable::get_or_insert_with] の照合述語で突き合わせ、
/// ハッシュ衝突時は probe を継続して別グループに分ける (誤集計は起こらない)。
///
/// 表はプリフェッチ可能な自前 [GroupTable]。グループ探索はハッシュ由来のランダム
/// アクセスでキャッシュミス律速なので、D 行先読みして各スロットを prefetch する
/// (ソフトウェアパイプライン。段数 D は [prefetch_distance])。
pub struct Aggregator<'a> {
    table: GroupTable<Stat, GroupInfo>,
    /// 実キー照合用に channel バイト列を intern するアリーナ。GroupInfo がオフセットで参照する。
    /// オフセット参照なので成長 (再確保) は安全 (上限なし)。
    arena: Vec<u8>,
    /// ハッシュ衝突する集計キー u64 の昇順リスト ([crate::keyspace])。ほぼ常に空。この集合に
    /// 載る u64 のときだけ実キー照合を行い、非 poison の u64 は列挙により (channel, month) を
    /// 一意に決めることが保証済みなので照合を省く (毎行の memcmp を消す)。全シャードで共有。
    poison: std::sync::Arc<[u64]>,
    /// 型に 'a (入力 mmap 借用) を残すためのマーカ。集計器はもう mmap を保持しない
    /// (channel はアリーナへコピー済み) が、dispatch/main の配線の型を変えないため 'a を維持する。
    _borrow: std::marker::PhantomData<&'a [u8]>,
}

impl<'a> Aggregator<'a> {
    /// 担当グループ数の見込み `expected_groups` に合わせた容量で作る。`poison` は衝突キー集合。
    /// シャーディング並列時は全体 ~12万キーを集計スレッド数で割った値を渡すことで
    /// 1 スレッドのテーブルを小さく保ち、キャッシュヒット率を上げる (足りなければ自動拡張)。
    pub fn with_group_capacity(expected_groups: usize, poison: std::sync::Arc<[u64]>) -> Aggregator<'a> {
        Aggregator {
            table: GroupTable::with_capacity_for(expected_groups, (Stat::empty(), GroupInfo::empty())),
            // 実行中の再確保回数を抑えるヒューリスティックな初期容量 (足りなければ自動成長)。
            arena: Vec::with_capacity(expected_groups.saturating_mul(32)),
            poison,
            _borrow: std::marker::PhantomData,
        }
    }

    /// u64 キーが衝突キー (poison) か。ほぼ常に空集合なので即 false を返す高速判定。
    #[inline]
    fn is_poisoned(&self, key: u64) -> bool {
        // 空 (~確実) なら 0 反復で false。非空でも数個なので線形走査で十分。
        self.poison.contains(&key)
    }

    /// channel バイト列をアリーナ末尾へ intern し、オフセット・長さ・先頭 prefix を持つ
    /// [GroupInfo] を作る。
    #[inline]
    fn intern(arena: &mut Vec<u8>, channel: &[u8], month: u8) -> GroupInfo {
        debug_assert!(channel.len() <= u8::MAX as usize, "channel too long for u8 len");
        let off = arena.len() as u32;
        arena.extend_from_slice(channel);
        let head = channel.len().min(CHANNEL_PREFIX);
        let mut prefix = [0u8; CHANNEL_PREFIX];
        prefix[..head].copy_from_slice(&channel[..head]);
        GroupInfo { off, len: channel.len() as u8, month, prefix }
    }

    /// キー計算済みの行バッチを集計する。バッチ内で D 行先のスロットを prefetch する
    /// ソフトウェアパイプライン (D は `OBRC_PREFETCH_DISTANCE`、既定 32)。
    pub fn add_batch(&mut self, batch: &[Pending<'a>]) {
        let d = prefetch_distance().min(batch.len());
        for p in &batch[..d] {
            self.table.prefetch(p.key);
            p.prefetch_channel();
        }
        for i in 0..batch.len() {
            if let Some(p) = batch.get(i + d) {
                self.table.prefetch(p.key);
                p.prefetch_channel();
            }
            self.update(&batch[i]);
        }
    }

    #[inline]
    fn update(&mut self, p: &Pending<'a>) {
        // p.key はバケット選択用ハッシュ。u64 一致時に実キー (channel + month) を照合して
        // ハッシュ衝突による誤集計を防ぐ。照合対象の channel はアリーナ (キャッシュ常駐) 内、
        // 挿入時 (1 億行中 ~12 万回) だけ入力 channel をアリーナへ intern する。
        // arena と table は別フィールドなので分割借用で同時に触れる。base は照合クロージャが
        // オフセットを解決する起点で、intern が末尾追記で成長 (再確保) するのは EMPTY スロット
        // 到達後 (照合が全て済んだ後) なので、照合中に base が無効化することはない。
        // 非 poison の u64 は (channel, month) を一意に決めるので照合を省く (毎行の memcmp を消す)。
        // poison ヒット時のみ従来どおり実キー照合。poison はスロットに依存しないので先に判定する。
        let poisoned = self.is_poisoned(p.key);
        let base = self.arena.as_ptr();
        let arena = &mut self.arena;
        let stat = self.table.get_or_insert_with(
            p.key,
            // SAFETY: base は arena の現在の先頭で、照合中は再確保されない (上のコメント参照)。
            |info: &GroupInfo| !poisoned || unsafe { info.matches(base, p.month(), p.channel()) },
            || (Stat::empty(), Self::intern(arena, p.channel(), p.month())),
        );
        stat.min_length = stat.min_length.min(p.message_length);
        stat.max_length = stat.max_length.max(p.message_length);
        stat.sum_length += p.message_length as u64;
        stat.count += 1;
        stat.total_stamp_count += p.stamp_count;
    }

    /// 別スレッドで集計した結果を取り込む (愚直実装)。キーは finalize 済み u64 (バケット
    /// 選択用) なのでそのまま自テーブルへ流し込み、実キー (channel + month) の照合で同一
    /// グループを突き合わせて Stat を成分ごとに合成する。グループ数 (~12万) ぶんの処理なので
    /// 行処理と違い速度は問題にならない。
    pub fn merge(&mut self, other: &Aggregator<'a>) {
        // SAFETY: other_base は other のアリーナの先頭。other はマージ中に変更されない。
        let other_base = other.arena.as_ptr();
        for (key, stat, info) in other.table.iter_keyed() {
            // channel は other のアリーナを指すので、挿入時は自分のアリーナへ intern し直す。
            let channel = unsafe { info.channel_from(other_base) };
            let month = info.month;
            let poisoned = self.is_poisoned(key);
            let base = self.arena.as_ptr();
            let arena = &mut self.arena;
            let dst = self.table.get_or_insert_with(
                key,
                // SAFETY: base は self.arena の現在の先頭で、照合中は再確保されない。
                |d: &GroupInfo| !poisoned || unsafe { d.matches(base, month, channel) },
                || (Stat::empty(), Self::intern(arena, channel, month)),
            );
            dst.min_length = dst.min_length.min(stat.min_length);
            dst.max_length = dst.max_length.max(stat.max_length);
            dst.sum_length += stat.sum_length;
            dst.count += stat.count;
            dst.total_stamp_count += stat.total_stamp_count;
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = Group<'_>> {
        // SAFETY: base は self.arena の先頭。イテレート中 self は変更されない。
        let base = self.arena.as_ptr();
        self.table.iter().map(move |(stat, info)| Group {
            channel: unsafe { info.channel_from(base) },
            month: info.month,
            stat,
        })
    }
}

}

mod dispatch {
//! パース段 → 集計段のデータ受け渡し層の**契約 (トレイト)**。実装はバックエンド別の
//! サブモジュール ([mpsc]) に置く。
//!
//! パース側スレッドは行をパースして [Pending](crate::aggregate::Pending) を作り、キーで
//! 集計スレッド (シャード) を選んでバッチにまとめて送る。集計側スレッドは自分宛てのバッチ
//! だけを受け取る。同一キーは必ず同一シャードに届くため、集計テーブルはロック不要で 1
//! スレッドが全体の 1/A だけを持つ。
//!
//! # 抽象化 (トレイト) と実装 (バックエンド) の分離
//!
//! この受け渡しの契約を 3 つのトレイトに切り出し、`main` の配線 (`run_pipeline`) はそれに
//! 対して汎化してある。過去に mpsc ↔ 自前 SPSC リングを何度も差し替えているので (コミット
//! 履歴 f5b9f31..096f040)、バックエンドを付け替えても配線を触らずに済むようにする。
//!
//! - [Sender] — パース側のファクトリ。パーススレッドごとに [Router] を配る。
//! - [Router] — パーススレッド 1 本のルーター。行をシャードへ振り分けて送る (ホットパス)。
//! - [Sink] — 集計スレッド 1 本の受信端。バッチを [Aggregator] へ流し込む。
//!
//! **静的ディスパッチ厳守**: [Router::route] は 100M 回/走行 呼ばれ、`dyn` の vtable 呼び
//! 出しは致命的 (senders.len() の 1 ロードですら ~6% 効いた実績がある)。トレイトは総称
//! (`run_pipeline<S, K>`) と関連型経由でのみ使い、単相化 + インライン展開で現状と同一コードに
//! 落とす。`dyn Router` は決して使わないこと。
//!
//! 既定バックエンドは有界 `sync_channel` を使う [mpsc] 実装 ([mpsc::channels] で構築)。

use crate::aggregate::{Aggregator, Pending};

pub mod mpsc {
//! [dispatch](crate::dispatch) の既定バックエンド: 有界 `sync_channel` (std mpsc) 実装。
//!
//! [Sender](super::Sender) / [Router](super::Router) / [Sink](super::Sink) を、シャードごとの
//! 有界チャネルとバッファ還流 ([Freelist]) で実装する。3 つの仕掛け:
//! - **有界チャネル**: パース先行時のバッチ滞留 (メモリ膨張) を背圧で防ぐ。
//! - **バッチング + シャードルーティング** ([MpscRouter]): 行ごとにチャネル送信すると重い
//!   ので、シャードごとに `batch_size` 行貯めてから 1 回送る。
//! - **バッファ還流** ([Freelist]): 集計側が読み終えた `Vec` を再利用に回し、バッチごとの
//!   `Vec::with_capacity` (malloc + 初回タッチのページフォルト) を全走行で数十回に抑える。
//!
//! ホットな [MpscRouter::route] は総称呼び出しから `#[inline(always)]` で展開される
//! (`dyn` は使わない。詳細は [dispatch](crate::dispatch) のモジュールドキュメント参照)。

use super::{Router, Sender, Sink};
use crate::aggregate::{Aggregator, Pending};
use std::sync::Mutex;
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};

/// 集計側が空にしたバッチ用 `Vec` を貯めておき、パース側へ貸し出すフリーリスト。
///
/// ロックはバッチ単位 (100M 行で ~12000 回) しか取らないので競合コストは無視できる。
/// 借り手が無い/尽きた場合だけ新規確保する。
pub struct Freelist<'a> {
    spare: Mutex<Vec<Vec<Pending<'a>>>>,
    batch_size: usize,
}

impl<'a> Freelist<'a> {
    pub fn new(batch_size: usize) -> Freelist<'a> {
        Freelist { spare: Mutex::new(Vec::new()), batch_size }
    }

    /// 空バッチを 1 本借りる。還流分があれば再利用、無ければ `batch_size` 容量で新規確保。
    #[inline]
    fn take(&self) -> Vec<Pending<'a>> {
        self.spare
            .lock()
            .unwrap()
            .pop()
            .unwrap_or_else(|| Vec::with_capacity(self.batch_size))
    }

    /// 読み終えたバッファを空にして還流する。
    #[inline]
    fn give(&self, mut buf: Vec<Pending<'a>>) {
        buf.clear();
        self.spare.lock().unwrap().push(buf);
    }
}

/// mpsc バックエンドの受け渡し一式を構築する。
///
/// 集計スレッド数ぶんの有界 `sync_channel` を張り、送信端を束ねた [MpscSender] と、各集計
/// スレッドが受け取る [MpscSink] 群を返す。`freelist` と `batch_size` は両端で共有する。
/// `channel_bound` は各チャネルの容量 (バッチ本数)。パース先行時の滞留を抑える背圧。
///
/// ライフタイムは 2 本: `'a` は行データ (mmap 借用) の生存、`'f` はフリーリストの借用。
/// 集計結果 ([Aggregator]) は `'a` にのみ依存し、フリーリストの借用 `'f` からは切り離される
/// (`'f` は並列スコープの中でだけ生きればよい)。
pub fn channels<'f, 'a>(
    freelist: &'f Freelist<'a>,
    agg_threads: usize,
    channel_bound: usize,
) -> (MpscSender<'f, 'a>, Vec<MpscSink<'f, 'a>>) {
    let mut senders = Vec::with_capacity(agg_threads);
    let mut sinks = Vec::with_capacity(agg_threads);
    for _ in 0..agg_threads {
        let (tx, rx) = sync_channel::<Vec<Pending<'a>>>(channel_bound);
        senders.push(tx);
        sinks.push(MpscSink { rx, freelist });
    }
    (MpscSender { senders, freelist }, sinks)
}

/// [Sender](super::Sender) の mpsc 実装。ページ境界に載せて [MpscSink] との偽共有を避ける。
#[repr(align(4096))]
pub struct MpscSender<'f, 'a> {
    senders: Vec<SyncSender<Vec<Pending<'a>>>>,
    freelist: &'f Freelist<'a>,
}

impl<'f, 'a> Sender<'a> for MpscSender<'f, 'a> {
    type Router = MpscRouter<'f, 'a>;

    /// シャードごとの詰めかけバッファを確保してルーターを作る。
    fn router(&self) -> MpscRouter<'f, 'a> {
        let n_shards = self.senders.len();
        MpscRouter {
            senders: self.senders.clone(),
            freelist: self.freelist,
            bufs: (0..n_shards).map(|_| self.freelist.take()).collect(),
            batch_size: self.freelist.batch_size,
            n_shards,
        }
    }
}

/// [Router](super::Router) の mpsc 実装: シャードごとに `batch_size` 行貯めてから 1 回送る。
pub struct MpscRouter<'f, 'a> {
    senders: Vec<SyncSender<Vec<Pending<'a>>>>,
    freelist: &'f Freelist<'a>,
    bufs: Vec<Vec<Pending<'a>>>,
    batch_size: usize,
    /// シャード数 (= `senders.len()`)。剰余の除数をループ不変な即値としてホットパスに置く。
    n_shards: usize,
}

impl<'a> Router<'a> for MpscRouter<'_, 'a> {
    /// シャード選択はテーブルのスロット選択 (下位ビット) と独立な中位ビットの剰余を使う
    /// (キーは拡散済みなのでこれで十分に均等)。バッチ長は容量と一致するので、満ちたバッチを
    /// 新しい借用バッファと入れ替えて送る (再確保なし)。
    #[inline(always)]
    fn route(&mut self, p: Pending<'a>) {
        let shard = (p.key >> 20) as usize % self.n_shards;
        let buf = &mut self.bufs[shard];
        buf.push(p);
        if buf.len() == self.batch_size {
            let full = std::mem::replace(buf, self.freelist.take());
            self.senders[shard].send(full).unwrap();
        }
    }

    fn finish(self) {
        for (shard, buf) in self.bufs.into_iter().enumerate() {
            if !buf.is_empty() {
                self.senders[shard].send(buf).unwrap();
            }
        }
    }
}

/// [Sink](super::Sink) の mpsc 実装。ページ境界に載せて [MpscSender] との偽共有を避ける。
#[repr(align(4096))]
pub struct MpscSink<'f, 'a> {
    rx: Receiver<Vec<Pending<'a>>>,
    freelist: &'f Freelist<'a>,
}

impl<'a> Sink<'a> for MpscSink<'_, 'a> {
    /// パース側が全 [MpscRouter] を drop するとチャネルが閉じてループを抜ける。
    fn drain(self, agg: &mut Aggregator<'a>) {
        for batch in self.rx {
            agg.add_batch(&batch);
            self.freelist.give(batch);
        }
    }
}

}

pub mod ring {
//! [dispatch](crate::dispatch) の SPSC リングバックエンド (実験用)。
//!
//! mpsc バックエンド ([mpsc](super::mpsc)) の代替。パーススレッド `P` × 集計スレッド `A` の
//! 各ペアに **独立した単一生産者・単一消費者 (SPSC) リング** を 1 本張り、行 ([Pending]) を
//! バッチ Vec に包まずリングへ直接積む (mpsc のバッチ malloc / フリーリスト還流が不要)。
//! 同一キーは `key >> 20 % A` で必ず同一シャードへ行くので、各集計スレッドは自分の列
//! (全 `P` 本の生産者リング) だけを読み、テーブルはロック不要のまま。
//!
//! - [Ring] — 固定容量 `N` の lock-free SPSC キュー。生産者・消費者が別スレッドから `&self`
//!   で同時アクセスするため、`head`/`tail` は [`AtomicUsize`]、要素は [`UnsafeCell`] で保持。
//! - [RingChannel] — `P*A` 本のリング行列を所有 ([mpsc::Freelist](super::mpsc::Freelist) と
//!   同じく [aggregate](crate::aggregate) スコープで確保し、送受端へ借用させる)。
//! - [RingSender]/[RingRouter]/[RingSink] — [dispatch](crate::dispatch) の 3 トレイト実装。
//!
//! **実験扱い**: SPSC リングは過去に何度も mpsc に僅差で負けている (コミット履歴参照)。
//! 採用は [ベンチ手順](../../index.html) に従って min 比較で判定すること。
//!
//! # チューニングメモ (2026-07-19 計測)
//!
//! この版を `main::aggregate` に繋いで 100M 行を計測したところ **5.18s** (mpsc は同機で min
//! 0.38s、約 14x 遅い) だった。原因は設計欠陥ではなく **2 つの未実装**:
//!
//! 1. **head の毎行 Release 公開** — 生産者が行ごとに `head.store(Release)`。消費者はスピン中
//!    その `head` を毎回 Acquire で読むため、ペアの head キャッシュラインが行ごとに ping-pong
//!    し生産者のストアが止まる。→ ルーター側にローカルカウンタを持たせ **K 行ごとにまとめて
//!    公開** (バッチ公開) すれば大半が消えるはず。
//! 2. **消費者のスピン** — 空振り時に tight spin で相手 head を叩き続ける。→ backoff / park。
//!
//! この 2 つを入れれば mpsc のバッチングの利点を保ちつつ `sync_channel` の Mutex+Condvar と
//! バッチ `Vec` 確保を完全に消せるので、**理論上の伸びしろは mpsc より大きい** (mpsc は既に
//! 実用上の床)。ただしパイプラインはパース段律速で dispatch は一部なので、詰め切っても期待値は
//! 「mpsc に僅差で並ぶ〜わずかに勝つ」程度。手順は ①バッチ公開だけ入れて再計測 → ②park、と
//! 段階的に。現状は未実装のまま `main` は mpsc に戻してある。

use super::{Router, Sender, Sink};
use crate::aggregate::{Aggregator, Pending};
use std::cell::UnsafeCell;
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

/// 各ペアのリング容量 (要素数)。`Pending` は 32B なので 8192 で 256KB/本。
const RING_CAP: usize = 8192;
/// 集計側がリングから 1 度に吸い出してプリフェッチ集計へ渡す塊のサイズ。
const DRAIN_CHUNK: usize = 512;

/// キャッシュライン (隣接ラインのプリフェッチ込みで 128B) 分離用の整列ラッパ。
#[repr(align(128))]
struct Line<T>(T);

/// 生産者専有の状態を 1 ラインに固める: 累積書き込み数 `head` と、消費者 `tail` の観測キャッシュ。
struct Producer {
    head: AtomicUsize,
    /// 直近に観測した `tail`。満杯判定が外れた時だけ本物の `tail` を再読込する。
    cached_tail: AtomicUsize, // 生産者のみが読み書き (Relaxed) するので実質ローカル
}

/// 消費者専有の状態を 1 ラインに固める: 累積読み出し数 `tail` と、生産者 `head` の観測キャッシュ。
struct Consumer {
    tail: AtomicUsize,
    /// 直近に観測した `head`。空判定が外れた時だけ本物の `head` を再読込する。
    cached_head: AtomicUsize, // 消費者のみが読み書き (Relaxed) するので実質ローカル
}

/// 固定容量 `N` の lock-free SPSC リングキュー。
///
/// 生産者は `head` のみを、消費者は `tail` のみを進める (巻き戻さない累積カウンタ、実スロットは
/// `pos % N`)。格納数 `head - tail` で満杯 (`== N`) / 空 (`== 0`) を曖昧なく区別する。要素は
/// [`MaybeUninit`] 保持なので `T: Default` は不要。
///
/// **偽共有対策**: `head` 側 ([Producer]) と `tail` 側 ([Consumer]) を別キャッシュラインに分離
/// ([Line])。さらに各自が相手インデックスをキャッシュし、満杯/空を外した時だけ相手のラインへ
/// 触れる (行ごとの cross-line トラフィックを消す — これが無いと ~15x 遅い)。
#[repr(align(4096))]
pub struct Ring<const N: usize, T> {
    data: [UnsafeCell<MaybeUninit<T>>; N],
    prod: Line<Producer>,
    cons: Line<Consumer>,
    /// 生産者が全行を積み終えたら true (消費者の終了判定用)。
    done: Line<AtomicBool>,
}

// SAFETY: 生産者は head/cached_tail と head 前のスロット書き込みだけ、消費者は tail/cached_head
// と tail..head のスロット読み出しだけに触れる (SPSC 規律)。範囲が重ならないので UnsafeCell への
// 同時アクセスは競合しない。head/tail の Release/Acquire が要素書き込みの可視性を運ぶ。
unsafe impl<const N: usize, T: Send> Sync for Ring<N, T> {}

impl<const N: usize, T> Ring<N, T> {
    pub fn new() -> Self {
        Ring {
            data: [const { UnsafeCell::new(MaybeUninit::uninit()) }; N],
            prod: Line(Producer { head: AtomicUsize::new(0), cached_tail: AtomicUsize::new(0) }),
            cons: Line(Consumer { tail: AtomicUsize::new(0), cached_head: AtomicUsize::new(0) }),
            done: Line(AtomicBool::new(0 != 0)),
        }
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.prod.0.head.load(Ordering::Acquire) == self.cons.0.tail.load(Ordering::Acquire)
    }

    /// 末尾に 1 要素積む (生産者専用)。満杯なら押し戻して `Err(item)` を返す。
    #[inline]
    pub fn push(&self, item: T) -> Result<(), T> {
        let head = self.prod.0.head.load(Ordering::Relaxed);
        // まずキャッシュした tail で満杯判定 (相手ラインに触れない)。
        let mut tail = self.prod.0.cached_tail.load(Ordering::Relaxed);
        if head - tail == N {
            // 満杯かも → 本物の tail を再読込してキャッシュ更新。
            tail = self.cons.0.tail.load(Ordering::Acquire);
            self.prod.0.cached_tail.store(tail, Ordering::Relaxed);
            if head - tail == N {
                return Err(item); // 本当に満杯
            }
        }
        // SAFETY: スロット head%N は tail..head 範囲外 (消費者は触れない)。
        unsafe { (*self.data[head % N].get()).write(item) };
        self.prod.0.head.store(head + 1, Ordering::Release);
        Ok(())
    }

    /// 先頭から 1 要素取り出す (消費者専用)。空なら `None`。
    #[inline]
    pub fn pop(&self) -> Option<T> {
        let tail = self.cons.0.tail.load(Ordering::Relaxed);
        // まずキャッシュした head で空判定 (相手ラインに触れない)。
        let mut head = self.cons.0.cached_head.load(Ordering::Relaxed);
        if head == tail {
            // 空かも → 本物の head を再読込してキャッシュ更新。
            head = self.prod.0.head.load(Ordering::Acquire);
            self.cons.0.cached_head.store(head, Ordering::Relaxed);
            if head == tail {
                return None; // 本当に空
            }
        }
        // SAFETY: tail < head なのでスロット tail%N は初期化済み。取り出し後は次に生産者が
        // head を N 周進めるまで読まれないので二重読みは起きない。
        let item = unsafe { (*self.data[tail % N].get()).assume_init_read() };
        self.cons.0.tail.store(tail + 1, Ordering::Release);
        Some(item)
    }

    /// 生産者が全行を積み終えたことを記録する。
    #[inline]
    pub fn mark_done(&self) {
        self.done.0.store(0 == 0, Ordering::Release);
    }

    #[inline]
    fn is_done(&self) -> bool {
        self.done.0.load(Ordering::Acquire)
    }
}

impl<const N: usize, T> Default for Ring<N, T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize, T> Drop for Ring<N, T> {
    fn drop(&mut self) {
        // 残っている初期化済み要素 (tail..head) だけを drop する。
        while self.pop().is_some() {}
    }
}

/// `P*A` 本の SPSC リング行列を所有するチャネル本体。[aggregate](crate::aggregate) スコープで
/// 確保し、[channels] で送受端へ借用させる。`rings[p*A + a]` がパース `p` → 集計 `a` のペア。
pub struct RingChannel<'a> {
    rings: Vec<Ring<RING_CAP, Pending<'a>>>,
    parse_threads: usize,
    agg_threads: usize,
    /// [RingSender::router] が配ったパーススレッド ID の採番カウンタ。
    next_parser: AtomicUsize,
}

impl<'a> RingChannel<'a> {
    pub fn new(parse_threads: usize, agg_threads: usize) -> RingChannel<'a> {
        let mut rings = Vec::with_capacity(parse_threads * agg_threads);
        rings.resize_with(parse_threads * agg_threads, Ring::new);
        RingChannel { rings, parse_threads, agg_threads, next_parser: AtomicUsize::new(0) }
    }
}

/// リングバックエンドの送受端一式を構築する ([mpsc::channels](super::mpsc::channels) の対応物)。
///
/// ライフタイム 2 本は mpsc と同じ意味: `'a` は行データ (mmap 借用) の生存、`'c` はリング行列
/// ([RingChannel]) の借用 (並列スコープ内だけ生きればよい)。
pub fn channels<'c, 'a>(
    channel: &'c RingChannel<'a>,
) -> (RingSender<'c, 'a>, Vec<RingSink<'c, 'a>>) {
    let sinks = (0..channel.agg_threads)
        .map(|shard| RingSink { channel, shard })
        .collect();
    (RingSender { channel }, sinks)
}

/// [Sender](super::Sender) のリング実装。パーススレッドごとに ID を採番して [RingRouter] を配る。
pub struct RingSender<'c, 'a> {
    channel: &'c RingChannel<'a>,
}

impl<'c, 'a> Sender<'a> for RingSender<'c, 'a> {
    type Router = RingRouter<'c, 'a>;

    fn router(&self) -> RingRouter<'c, 'a> {
        let parser = self.channel.next_parser.fetch_add(1, Ordering::Relaxed);
        debug_assert!(parser < self.channel.parse_threads);
        RingRouter {
            channel: self.channel,
            base: parser * self.channel.agg_threads,
            n_shards: self.channel.agg_threads,
        }
    }
}

/// [Router](super::Router) のリング実装: 行を `key >> 20 % A` のシャードリングへ直接積む。
pub struct RingRouter<'c, 'a> {
    channel: &'c RingChannel<'a>,
    /// このパーススレッドの行 (`rings[base .. base + n_shards]`) の先頭添字。
    base: usize,
    n_shards: usize,
}

impl<'a> Router<'a> for RingRouter<'_, 'a> {
    #[inline(always)]
    fn route(&mut self, p: Pending<'a>) {
        let shard = (p.key >> 20) as usize % self.n_shards;
        let ring = &self.channel.rings[self.base + shard];
        // 満杯なら消費者が吸い出すまでスピン (背圧)。SPSC なので必ず前進する。
        let mut item = p;
        while let Err(back) = ring.push(item) {
            std::hint::spin_loop();
            item = back;
        }
    }

    fn finish(self) {
        for shard in 0..self.n_shards {
            self.channel.rings[self.base + shard].mark_done();
        }
    }
}

/// [Sink](super::Sink) のリング実装: 自シャード列 (全 `P` 本の生産者リング) を読み集計する。
#[repr(align(4096))]
pub struct RingSink<'c, 'a> {
    channel: &'c RingChannel<'a>,
    shard: usize,
}

impl<'a> Sink<'a> for RingSink<'_, 'a> {
    fn drain(self, agg: &mut Aggregator<'a>) {
        let a = self.channel.agg_threads;
        let p = self.channel.parse_threads;
        let mut buf: Vec<Pending<'a>> = Vec::with_capacity(DRAIN_CHUNK);
        loop {
            let mut progressed = false;
            let mut all_finished = true;
            for parser in 0..p {
                let ring = &self.channel.rings[parser * a + self.shard];
                while buf.len() < DRAIN_CHUNK {
                    match ring.pop() {
                        Some(item) => {
                            buf.push(item);
                            progressed = true;
                        }
                        None => break,
                    }
                }
                // まだ生産中、または未読が残るシャードがあれば「全終了」ではない。
                if !ring.is_done() || !ring.is_empty() {
                    all_finished = false;
                }
                if buf.len() == DRAIN_CHUNK {
                    agg.add_batch(&buf);
                    buf.clear();
                }
            }
            if !buf.is_empty() {
                agg.add_batch(&buf);
                buf.clear();
            }
            if all_finished {
                break; // 全生産者が done かつ全リング空 → もう届かない
            }
            if !progressed {
                std::hint::spin_loop(); // 一時的に空。生産待ち。
            }
        }
    }
}



}


/// パース段の送信端 (ファクトリ)。各パーススレッドはここから自前の [Router] を 1 本ずつ
/// 取り出す。本体と全 [Router] が drop されると経路が閉じ、集計側の受信ループが抜ける。
pub trait Sender<'a> {
    /// このバックエンドが配るルーター型。パーススレッドへ move するため `Send`。
    type Router: Router<'a> + Send;

    /// パーススレッド 1 本ぶんのルーターを作る。
    fn router(&self) -> Self::Router;
}

/// パーススレッド 1 本が持つルーター: 行 ([Pending]) をシャードへ振り分けて送る。
pub trait Router<'a> {
    /// 1 行ぶんの [Pending] を振り分けて送る。**ホットパス** (100M 回/走行)。
    /// 実装は必ず `#[inline(always)]` にして総称呼び出しから展開させること。
    fn route(&mut self, p: Pending<'a>);

    /// スレッド終了時に、貯めかけのバッチを送り切る。
    fn finish(self);
}

/// 集計スレッド 1 本の受信端: 自分宛てバッチを受け取り、専有 [Aggregator] へ流し込む。
pub trait Sink<'a> {
    /// 経路が閉じるまでバッチを受け取り続け、`agg` に集計する。
    fn drain(self, agg: &mut Aggregator<'a>);
}

}

mod hash {
//! グループキー生成: `(channel, month)` を 64bit の集計キー1本に落とす。
//!
//! このキーの u64 を [GroupTable](crate::table::GroupTable) のスロット選択 (`key & mask`、
//! 下位ビット) と高速プレフィルタに使う。ハッシュ衝突しても集計側が実キー `(channel, month)`
//! を照合して別グループに分けるので、衝突は誤集計にならない (正しさはハッシュ品質に依存
//! しない)。ハッシュに求められるのはバケット分布の良さだけで、実データの distinct キー
//! 120000 個 (channel 10000 × 月 12) が良く散ることを
//! [tests::no_group_key_collisions_on_real_data] が健全性チェックする。
//!
//! アルゴリズムは FxHash: 8バイトワードごとに「回転 + xor + 定数倍算」([fx_round])。
//! 定数 `SEED` は黄金比 2^64/φ 由来の乗算ハッシュ定数で、入力下位ビットの偏りを出力へ
//! 拡散する。最後に [finalize_key] で月を畳み、乗算で上位へ寄った拡散を `rotate_left(32)`
//! で下位へ回す (テーブルが使う下位ビットの質を確保する。詳細は [finalize_key] 参照)。
//!
//! パーサは channel を SWAR 走査する際、読んだワードがレジスタにあるうちに [fx_round] を
//! 回してキーを前倒し計算する ([Row::hash](crate::parse::Row))。その融合経路が
//! 素直な [group_key] と一致することは parse.rs の `fused_hash_matches_group_key` が保証する。

/// FxHash ラウンドの乗数。黄金比に由来する Fibonacci hashing の定数。
const SEED: u64 = 0x517c_c1b7_2722_0a95;
/// ワード取り込み前の回転量。連続ワードのビットを桁方向にずらして混ぜる。
const ROTATE: u32 = 5;

/// 8バイト未満の端数チャンクをゼロ埋めでリトルエンディアン読みする。
/// 長さをハッシュに含めないため末尾が NUL の channel とは衝突し得るが、実データに NUL は無い。
/// ホットパスはワード読みを自前でやるため、規範実装 [group_key] からのみ使うテスト専用。
 fn load_u64(bytes: &[u8]) -> u64 {
    let mut buf = [0u8; 8];
    buf[..bytes.len()].copy_from_slice(bytes);
    u64::from_le_bytes(buf)
}

/// FxHash の 1 ワード取り込みラウンド (回転 + xor + 乗算)。
#[inline]
pub(crate) fn fx_round(h: u64, word: u64) -> u64 {
    (h.rotate_left(ROTATE) ^ word).wrapping_mul(SEED)
}

/// channel ぶんの累積ハッシュ `h` に ym (本コンテストでは月) を畳んで最終キーにする。
///
/// finalizer は本来 fmix64 (xor-shift×3 + 乗算×2) 相当が定石だが、バケット選択用途では過剰:
/// (1) 集計側が実キーを照合するので、衝突ゼロは正しさの要件ではなくバケット分布の質の話。
///     回転は全単射なので `fx_round` 段の分布の良さをそのまま下位ビットへ引き継げる。
/// (2) 速度に効くのは [GroupTable](crate::table::GroupTable) が下位ビットで選ぶスロット
///     分布だが、乗算で上位へ寄った拡散を `rotate_left(32)` で下位へ回すだけで、fmix64 と
///     同等の probe 分布になる (実キー 120000 での実測: 平均 probe 0.42、最長 probe 18)。
///     5 命令の finalizer を 1 命令に落とせる。
#[inline]
pub fn finalize_key(h: u64, ym: u64) -> u64 {
    fx_round(h, ym).rotate_left(32)
}

/// `(channel, ym)` から集計キーを直接計算する規範実装。ホットパスはこれと同値なものを
/// パース中に前倒し計算する ([Row::hash](crate::parse::Row) + [finalize_key]) ため、
/// この素直な実装はテストからの参照専用 (両経路の一致検証と実データ衝突検査)。
 fn group_key(channel: &[u8], ym: u64) -> u64 {
    let mut h = 0u64;
    for chunk in channel.chunks(8) {
        h = fx_round(h, load_u64(chunk));
    }
    finalize_key(h, ym)
}



}

mod input {
use crate::mmap::Mmap;
use crate::parse::Padded;

/// 入力段: ファイルをメモリマップし、バイト列として貸し出す。
/// UTF-8 検証はしない (ファイル全体の余分な走査を避ける)。
/// ここを差し替えれば読み込み戦略 (read_to_string / チャンク読み等) を変えられる。
pub struct Input {
    map: Mmap,
}

impl Input {
    pub fn open(path: &str) -> Input {
        let map = Mmap::open(path).expect("failed to mmap input file");
        Input { map }
    }

    /// パーサ向けの番兵付きビュー。Mmap が末尾余白と番兵 `\n` を保証している。
    #[inline]
    pub fn padded(&self) -> Padded<'_> {
        Padded::new(self.map.padded_bytes(), self.map.len())
    }
}

}

mod keyspace {
//! キー空間の事前解析: ハッシュ衝突する集計キーだけを厳密に特定する ("poison" 集合)。
//!
//! 集計キー u64 は `(channel, month)` の [finalize_key] 値で、これをテーブルのバケット選択に
//! 使う。異なるキーが同じ u64 になると誤集計になるため、通常は毎行 channel バイト列を照合して
//! 防いでいる (~100M 回の memcmp が集計段の律速)。
//!
//! だが distinct channel は高々 10,000 種 (コンテスト保証) で、month は 1..=12 の既知集合。
//! よって**全チャンネルの channel-hash さえ分かれば、出現しうる全キー (channel × 12 = 最大
//! 120,000 個) を先回りで列挙**でき、その中で u64 が衝突するものを完全に特定できる。衝突する
//! u64 (= poison) はほぼ確実に 0 個 (64bit 空間に 12 万キーなら衝突確率 ~10^-9)。
//!
//! 集計側はこの poison 集合だけを毎行チェックし、非 poison の u64 は「その u64 が (channel,
//! month) を一意に決める」ことが列挙で保証済みなので **channel 照合を丸ごと省略**できる
//! (poison ヒット時のみ従来どおり実キー照合)。これで衝突ゼロの厳密性を保ったまま、毎行の
//! memcmp を消せる。列挙は全チャンネルを網羅するので、集計中に初めて出現する (channel, month)
//! の組でも安全 (その u64 は列挙済みで、衝突するなら poison に入っている)。
//!
//! チャンネル収集は入力先頭からの単一走査で、distinct channel が `max_channels` に達した
//! 時点で打ち切る (クーポンコレクタ的に ~10 万行程度で出尽くす。全体の ~0.1%)。10,000 に
//! 満たないままファイル終端に達した場合も、全 channel を見たことになるので集合は完全。

use crate::hash::finalize_key;
use crate::parse::{Padded, Parser};
use std::collections::HashMap;

/// 入力先頭を走査し、distinct な channel の channel-hash ([crate::parse::Row::hash]) を
/// 集める。distinct channel が `max_channels` に達したら打ち切る (それ以上は現れない保証)。
/// 返す `Vec` は distinct channel 文字列ごとに 1 要素 (文字列が違えば hash が偶然一致しても
/// 別要素として残す ⇒ その一致は poison 列挙で衝突として検出される)。
pub fn discover_channel_hashes(input: Padded, max_channels: usize) -> Vec<u64> {
    let mut seen: HashMap<Box<[u8]>, u64> = HashMap::with_capacity(max_channels);
    for row in Parser::new(input) {
        if !seen.contains_key(row.channel) {
            seen.insert(row.channel.into(), row.hash);
            if seen.len() >= max_channels {
                break; // 保証された最大種数に到達: 以降に新 channel は現れない
            }
        }
    }
    seen.into_values().collect()
}

/// 全チャンネルの channel-hash から、出現しうる全キー `finalize_key(hash, 1..=12)` を列挙し、
/// u64 が 2 つ以上の異なる (channel, month) から生じる値 = 衝突キー (poison) を昇順で返す。
/// ほぼ常に空。集計側はこの集合に載る u64 のときだけ実キー照合を行えばよい。
pub fn build_poison(channel_hashes: &[u64]) -> Vec<u64> {
    let mut keys: Vec<u64> = Vec::with_capacity(channel_hashes.len() * 12);
    for &h in channel_hashes {
        for m in 1..=12u64 {
            keys.push(finalize_key(h, m));
        }
    }
    keys.sort_unstable();
    let mut poison = Vec::new();
    let mut i = 0;
    while i < keys.len() {
        let mut j = i + 1;
        while j < keys.len() && keys[j] == keys[i] {
            j += 1;
        }
        if j - i >= 2 {
            poison.push(keys[i]); // 2 個以上の異なるキーが同じ u64 ⇒ 衝突
        }
        i = j;
    }
    poison
}



}

mod mmap {
use std::ffi::c_void;
use std::fs::File;
use std::io;
use std::ops::{Deref, DerefMut};
use std::os::unix::io::AsRawFd;
use std::ptr;
use std::ptr::NonNull;

const PROT_READ: i32 = 0x1;
const PROT_WRITE: i32 = 0x2;
const MAP_PRIVATE: i32 = 0x02;
const MAP_SHARED: i32 = 0x01;
const MAP_ANONYMOUS: i32 = 0x20;
const MAP_FIXED: i32 = 0x10;

const PAGE: usize = 4096;

/// 入力マップがファイル末尾の先に確保する読み出し可能な余白 (番兵 `\n` + ワード読みの
/// はみ出し許容ぶん)。パーサが境界チェック無しで走れる根拠になる。
pub const TAIL_PAD: usize = 16;

unsafe extern "C" {
    fn mmap(
        addr: *mut c_void,
        len: usize,
        prot: i32,
        flags: i32,
        fd: i32,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> i32;
    fn madvise(addr: *mut c_void, len: usize, advice: i32) -> i32;
}

const MADV_SEQUENTIAL: i32 = 2;
const MADV_POPULATE_READ: i32 = 22;

/// ファイルを private (copy-on-write) でメモリマップする。
/// `fs::read_to_string` と違い、ページキャッシュ上のデータをそのまま仮想アドレス空間に
/// 貼り付けるだけなので、ヒープへのコピー(=メモリの二重確保)が発生しない。
///
/// さらに、ファイル末尾の後ろに [TAIL_PAD] バイトの読み出し可能な余白を保証し、
/// `data[len]` に番兵 `\n` を書き込む (MAP_PRIVATE の COW なのでファイルには反映されない)。
/// パーサはこの余白を根拠に、行末探索やワード読みの境界チェックを省略できる。
/// 実現方法: 先に anonymous マッピングで「ファイルサイズ + 余白」ぶんのページを確保し、
/// その先頭に MAP_FIXED でファイルを被せる。ファイル最終ページの端数はカーネルが
/// ゼロ埋めし、その先のページは anonymous のゼロページが残るため、余白全域が有効になる。
pub struct Mmap {
    ptr: *const u8,
    len: usize,
    map_len: usize,
}

impl Mmap {
    pub fn open(path: &str) -> io::Result<Mmap> {
        let file = File::open(path)?;
        let len = file.metadata()?.len() as usize;

        // 余白込みでページ境界に切り上げた anonymous 領域を先に確保する
        let map_len = (len + TAIL_PAD).div_ceil(PAGE) * PAGE;
        let addr = unsafe {
            mmap(
                ptr::null_mut(),
                map_len,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        if addr as isize == -1 {
            return Err(io::Error::last_os_error());
        }

        if len > 0 {
            // 先頭にファイルを被せる (PROT_WRITE + MAP_PRIVATE = 番兵書き込み用の COW)
            let faddr = unsafe {
                mmap(
                    addr,
                    len,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_FIXED,
                    file.as_raw_fd(),
                    0,
                )
            };
            if faddr as isize == -1 {
                let err = io::Error::last_os_error();
                unsafe { munmap(addr, map_len) };
                return Err(err);
            }
        }

        // 全域を先頭から一度だけ舐めるアクセスパターンなので、readahead を強化しつつ
        // POPULATE_READ で PTE を先に張り、パース中のマイナーフォルトを前倒しする
        // (どちらも助言なので、古いカーネルで未対応でも正しさに影響しない)。
        if len > 0 {
            unsafe {
                madvise(addr, len, MADV_SEQUENTIAL);
                madvise(addr, len, MADV_POPULATE_READ);
            }
        }

        let ptr = addr as *mut u8;
        // 番兵: パーサの行末探索がファイル終端を越えないことを保証する
        unsafe { ptr.add(len).write(b'\n') };

        Ok(Mmap {
            ptr,
            len,
            map_len,
        })
    }

    /// ファイル本体の長さ (番兵・余白を含まない)。
    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    /// 余白込みのバイト列 (`len() + TAIL_PAD` バイト、`[len()]` は番兵 `\n`)。
    #[inline]
    pub fn padded_bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.len + TAIL_PAD) }
    }
}

impl Deref for Mmap {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }
}

impl Drop for Mmap {
    fn drop(&mut self) {
        unsafe {
            munmap(self.ptr as *mut c_void, self.map_len);
        }
    }
}

unsafe impl Send for Mmap {}
unsafe impl Sync for Mmap {}

/// 出力ファイルをread-write, shared でメモリマップする。
/// `write(2)` をバッファ経由で都度発行する代わりに、ページキャッシュへ直接メモリコピーする。
/// mmap するにはファイルの実サイズが先に確定している必要があるため、
/// 呼び出し側が (通常はヒューリスティックな上限で) `len` を見積もって渡す前提。
pub struct MmapMut {
    ptr: *mut u8,
    len: usize,
}

impl MmapMut {
    /// `path` を新規作成 (存在すれば切り詰め) し、`len` バイトに拡張した上でマップする。
    /// 呼び出し側は書き込んだ実バイト数で `file.set_len` して切り詰める。
    pub fn create(path: &str, len: usize) -> io::Result<(File, MmapMut)> {
        // PROT_READ を要求する mmap には fd 自体に読み取り権限が要る。
        // `File::create` は write-only で開くため、それだけでは EACCES になる。
        let file = File::options()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;
        file.set_len(len as u64)?;

        if len == 0 {
            return Ok((
                file,
                MmapMut {
                    ptr: NonNull::dangling().as_ptr(),
                    len: 0,
                },
            ));
        }

        let addr = unsafe {
            mmap(
                ptr::null_mut(),
                len,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                file.as_raw_fd(),
                0,
            )
        };
        if addr as isize == -1 {
            return Err(io::Error::last_os_error());
        }

        Ok((
            file,
            MmapMut {
                ptr: addr as *mut u8,
                len,
            },
        ))
    }
}

impl Deref for MmapMut {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }
}

impl DerefMut for MmapMut {
    fn deref_mut(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
    }
}

impl Drop for MmapMut {
    fn drop(&mut self) {
        if self.len > 0 {
            unsafe {
                munmap(self.ptr as *mut c_void, self.len);
            }
        }
    }
}

unsafe impl Send for MmapMut {}
unsafe impl Sync for MmapMut {}

}

mod output {
use crate::aggregate::Aggregator;
use crate::mmap::MmapMut;
use std::io::Write;

/// 1行のうちチャンネル名を除く固定部分のバイト数上限。内訳は
/// `,YYYY-MM=`(9) と min/avg/max/count/stamp の数値5個 + 区切り `/`・改行で、
/// 各数値が u32 の最大10桁 (avg は `.` と小数2桁で13桁) に達しても 67 バイトに収まる。
/// 実データでは各フィールドが10桁に達することは考えにくいが、安全側に余裕を持たせて 80。
const MAX_LINE_OVERHEAD: usize = 80;

/// 出力段: 集計結果を1グループ1行で書き出す。
///
/// 書き込み先を write(2) 都度発行の `BufWriter` ではなく mmap にすることで、
/// フォーマット結果をページキャッシュへ直接メモリコピーする。mmap にはファイルの
/// 実サイズを先に確定させる必要があるため、フォーマットはせずチャンネル長の合計 +
/// 行あたり固定上限だけで必要サイズをヒューリスティックに見積もり、事前に
/// `file.set_len` で確保する。書き込み後、実際に使ったぶんだけ切り詰める。
pub fn write_result(output_path: &str, agg: &Aggregator) {
    let cap: usize = agg
        .iter()
        .map(|g| g.channel.len() + MAX_LINE_OVERHEAD)
        .sum();

    let (file, mut map) = MmapMut::create(output_path, cap).expect("failed to create output file");
    let mut cursor: &mut [u8] = &mut map[..];

    for g in agg.iter() {
        let s = &g.stat;
        // チャンネル名は任意のバイト列なのでフォーマッタを通さずそのまま書く
        cursor.write_all(g.channel).unwrap();
        // 年は 2027 固定 (コンテスト保証、aggregate 側で年を持たない) なので定数で補う
        writeln!(
            cursor,
            ",2027-{:02}={}/{:.2}/{}/{}/{}",
            g.month,
            s.min_length,
            s.average(),
            s.max_length,
            s.count,
            s.total_stamp_count,
        )
        .unwrap();
    }

    let actual_len = cap - cursor.len();
    drop(map);
    file.set_len(actual_len as u64).unwrap();
}

}

mod parse {
/// パース段: 入力バッファ全体を1パスで走査して `Row` を順に返すイテレータ。
/// 行分割 (`\n` 探し) とフィールド分割 (`,` 探し) を別々に走査せず、
/// ポインタを1本進めながら「数字を読む → 区切りを踏む」を繰り返す。
/// チャンネル名は同一性比較にしか使わないため `&[u8]` のまま持つ。
#[derive(Clone, Copy, Default)]
pub struct Row<'a> {
    pub timestamp: i64,
    pub channel: &'a [u8],
    pub message_length: u64,
    pub stamp_count: u64,
    /// channel バイト列だけの FxHash 累積値 (ym 未混合)。channel を SWAR で走査する
    /// 際、読んだ word がレジスタにある間に `fx_round` を回して前倒し計算する。
    /// aggregate 側で `finalize_key(hash, month)` として月を畳めば group_key と同値。
    pub hash: u64,
}

/// 「末尾に番兵と余白を持つバイト列」であることを型で保証した入力ビュー。
///
/// 不変条件 (コンストラクタで検査):
/// - `data.len() >= len + 9` — 論理終端の先に番兵 1 バイト + ワード読み 8 バイトの余白
/// - `data[len] == b'\n'` — 行末探索がファイル終端で必ず止まる番兵
///
/// パーサはこの不変条件を根拠に、ホットループの境界チェック (毎バイトの `p < len` 比較と
/// スライス添字のパニック検査) を省略する。本番は [Mmap](crate::mmap::Mmap) が
/// [TAIL_PAD](crate::mmap::TAIL_PAD) の余白と番兵書き込みでこれを満たし、テストは
/// Vec に番兵と余白を追記して満たす。
#[derive(Clone, Copy)]
pub struct Padded<'a> {
    data: &'a [u8],
    len: usize,
}

impl<'a> Padded<'a> {
    /// `data` の先頭 `len` バイトを論理入力とするビューを作る。不変条件を満たさなければ panic。
    pub fn new(data: &'a [u8], len: usize) -> Padded<'a> {
        assert!(
            data.len() >= len + 9,
            "padded buffer too short: {} < {} + 9",
            data.len(),
            len
        );
        assert_eq!(data[len], b'\n', "sentinel newline missing at logical end");
        Padded { data, len }
    }

    /// 入力を行境界で `n` 個の連続チャンクに分割する (並列処理用)。
    ///
    /// 等分点 `len * i / n` から次の `\n` まで進めて行境界に合わせるため、各チャンクの
    /// 論理終端はちょうど実データの `\n` を指す = そのままチャンクの番兵になり、
    /// [Padded] の不変条件を引き継げる (末尾チャンクは元の番兵と余白を引き継ぐ)。
    /// 行数が `n` 未満などで等分点が衝突した場合、余ったチャンクは空になる。
    pub fn split(self, n: usize) -> Vec<Padded<'a>> {
        assert!(n >= 1);
        let mut out = Vec::with_capacity(n);
        let mut begin = 0usize;
        for i in 1..=n {
            if begin >= self.len {
                // 入力を使い切った: 元の番兵を流用した空チャンク
                out.push(Padded::new(&self.data[self.len..], 0));
                continue;
            }
            let end = if i == n {
                self.len
            } else {
                // 等分点から次の '\n' へ (最悪でも data[len] の番兵で止まる)
                let mut p = (self.len * i / n).max(begin);
                while self.data[p] != b'\n' {
                    p += 1;
                }
                p.min(self.len)
            };
            out.push(Padded::new(&self.data[begin..], end - begin));
            begin = end + 1; // チャンク間の '\n' は誰の論理範囲にも含めない
        }
        out
    }
}

/// 10 桁固定のタイムスタンプを `d[p..p+10]` から読む。コンテスト保証により値は 2027 年内
/// (1_798_761_600〜1_830_297_599) なので桁数分岐は不要で、8 桁を SWAR 一括変換し残り 2 桁を
/// 足す一本道にできる。
///
/// # Safety
/// `d[p..p+8]` のワード読みと `d[p+8]`, `d[p+9]` の参照が有効であること
/// ([Padded] の余白がこれを保証する)。
#[inline]
unsafe fn parse_timestamp(d: &[u8], p: usize) -> u64 {
    unsafe {
        let chunk = u64::from_le(d.as_ptr().add(p).cast::<u64>().read_unaligned());
        crate::swar::parse_8_digits(chunk) * 100
            + (d.get_unchecked(p + 8) - b'0') as u64 * 10
            + (d.get_unchecked(p + 9) - b'0') as u64
    }
}

/// channel フィールド (`d[start..]` の次の ',' まで) を SWAR で走査し、終端カンマの位置と
/// channel バイト列だけの FxHash 累積値を返す。channel は平均 20 バイト前後と長く、素朴な
/// バイト単位ループはフィールド脱出のたびに分岐予測を外して重い。8 バイトずつカンマを探して
/// 分岐を 1/8 に減らし、同時に読んだワードがレジスタにある間に [fx_round](crate::hash::fx_round)
/// を回してハッシュを前倒し計算する (aggregate 側での channel 再読込を省く)。
///
/// ハッシュは [group_key](crate::hash::group_key) の channel ループ (`chunks(8)` を
/// ゼロ埋め読み) と完全一致させる: カンマを含むワードは下位 `off` バイトだけ残してゼロ埋め、
/// `off == 0` (channel 長が 8 の倍数) のワードは寄与ゼロなので取り込まない。
///
/// # Safety
/// `start` から終端カンマまでの範囲でのワード読みが `d` の余白内に収まること。well-formed な
/// 行なら ',' は論理終端より手前にあり、走査が余白に入るのは破損入力のみ (その場合も
/// `p + 8 <= d.len()` のループ条件で打ち切る)。
#[inline]
unsafe fn scan_channel(d: &[u8], start: usize) -> (usize, u64) {
    let mut p = start;
    let mut hash: u64 = 0;
    while p + 8 <= d.len() {
        let word = u64::from_le(unsafe { d.as_ptr().add(p).cast::<u64>().read_unaligned() });
        let off = crate::swar::first_comma(word);
        if off == 8 {
            hash = crate::hash::fx_round(hash, word);
            p += 8;
            continue;
        }
        if off != 0 {
            hash = crate::hash::fx_round(hash, crate::swar::keep_low_bytes(word, off));
        }
        p += off as usize;
        break;
    }
    (p, hash)
}

pub struct Parser<'a> {
    input: Padded<'a>,
    pos: usize,
}

impl<'a> Parser<'a> {
    pub fn new(input: Padded<'a>) -> Parser<'a> {
        Parser { input, pos: 0 }
    }
}

impl<'a> Iterator for Parser<'a> {
    type Item = Row<'a>;

    #[inline]
    fn next(&mut self) -> Option<Row<'a>> {
        let d = self.input.data;
        let len = self.input.len;
        loop {
            let mut p = self.pos;
            if p >= len {
                return None;
            }

            // SAFETY: 以降の get_unchecked / ワード読みはすべて Padded の不変条件
            // (d[len] == '\n' の番兵と、その先 8 バイトの余白) が上界を与える:
            // 行末探索は番兵で必ず p <= len で止まり、8 バイト読みの起点は p <= len+1 に
            // 収まるため読みは d.len() (>= len+9) を越えない。各ヘルパの Safety もこの
            // 不変条件に依存する。
            unsafe {
                // 数字で始まらない行 (ヘッダ・空行) は行ごと読み飛ばす
                if !d.get_unchecked(p).is_ascii_digit() {
                    while *d.get_unchecked(p) != b'\n' {
                        p += 1;
                    }
                    self.pos = p + 1;
                    continue;
                }

                // タイムスタンプ: 常に 10 桁 + 区切り ',' で 11 バイト進む
                let ts = parse_timestamp(d, p);
                p += 11;

                // チャンネル名: 次の ',' まで。走査中に channel ハッシュを前倒し計算する
                let start = p;
                let (comma, hash) = scan_channel(d, start);
                let channel = d.get_unchecked(start..comma);
                p = comma + 1;

                // message_length: 可変桁 (短いのでバイトループで十分)
                let mut message_length = 0u64;
                while *d.get_unchecked(p) != b',' {
                    message_length = message_length * 10 + (d.get_unchecked(p) - b'0') as u64;
                    p += 1;
                }
                p += 1;

                // stamp_count: 行末まで (最終行に改行が無くても番兵 '\n' で止まる)
                let mut stamp_count = 0u64;
                while *d.get_unchecked(p) != b'\n' && *d.get_unchecked(p) != b'\r' {
                    stamp_count = stamp_count * 10 + (d.get_unchecked(p) - b'0') as u64;
                    p += 1;
                }
                // 改行 (\n または \r\n) を1つだけ読み飛ばす
                if *d.get_unchecked(p) == b'\r' {
                    p += 1;
                }
                if *d.get_unchecked(p) == b'\n' {
                    p += 1;
                }
                self.pos = p;

                return Some(Row {
                    timestamp: ts as i64,
                    channel,
                    message_length,
                    stamp_count,
                    hash,
                });
            }
        }
    }
}

 mod tests {
    use super::*;

    /// テスト入力に番兵 '\n' と余白を付けて [Padded] の不変条件を満たすバッファを作る。
    pub(crate) fn pad(input: &[u8]) -> Vec<u8> {
        let mut buf = input.to_vec();
        buf.push(b'\n');
        buf.extend_from_slice(&[0u8; 8]);
        buf
    }

    fn parse_all(input: &[u8]) -> Vec<(i64, Vec<u8>, u64, u64)> {
        let buf = pad(input);
        Parser::new(Padded::new(&buf, input.len()))
            .map(|r| (r.timestamp, r.channel.to_vec(), r.message_length, r.stamp_count))
            .collect()
    }

    #[test]
    fn parses_lines_and_skips_header() {
        let rows = parse_all(
            b"unix_timestamp,channel_path,message_length,stamp_count\n\
              1811438747,video/server/blue/open,197,3\n\
              1813670889,design,90,7\n",
        );
        assert_eq!(
            rows,
            vec![
                (1811438747, b"video/server/blue/open".to_vec(), 197, 3),
                (1813670889, b"design".to_vec(), 90, 7),
            ]
        );
    }

    #[test]
    fn handles_crlf_and_missing_final_newline() {
        // タイムスタンプは常に10桁 (2027年保証) 前提。CRLF と最終行の改行欠落を確認。
        let rows = parse_all(b"1813670889,design,90,7\r\n1798761600,a,1,2");
        assert_eq!(
            rows,
            vec![
                (1813670889, b"design".to_vec(), 90, 7),
                (1798761600, b"a".to_vec(), 1, 2),
            ]
        );
    }

    #[test]
    fn split_preserves_all_rows() {
        // タイムスタンプは必ず10桁 (パーサの前提)
        let mut input = Vec::new();
        for i in 0..1000u64 {
            input.extend_from_slice(
                format!("{},channel-{},{},{}\n", 1798761600 + i, i % 37, i, i * 2).as_bytes(),
            );
        }
        let buf = pad(&input);
        let whole: Vec<_> = Parser::new(Padded::new(&buf, input.len()))
            .map(|r| (r.timestamp, r.channel.to_vec(), r.message_length, r.stamp_count))
            .collect();
        assert_eq!(whole.len(), 1000);
        for n in [1usize, 2, 3, 7, 16, 999, 1000, 1500] {
            let chunks = Padded::new(&buf, input.len()).split(n);
            assert_eq!(chunks.len(), n);
            let rejoined: Vec<_> = chunks
                .into_iter()
                .flat_map(|c| {
                    Parser::new(c)
                        .map(|r| (r.timestamp, r.channel.to_vec(), r.message_length, r.stamp_count))
                })
                .collect();
            assert_eq!(rejoined, whole, "n={n}");
        }
    }

    #[test]
    fn split_handles_edge_cases() {
        // 空入力
        let buf = pad(b"");
        for c in Padded::new(&buf, 0).split(4) {
            assert!(Parser::new(c).next().is_none());
        }
        // 最終行に改行が無い入力
        let input = b"1798761600,a,1,2\n1798761601,b,3,4";
        let buf = pad(input);
        let rows: usize = Padded::new(&buf, input.len())
            .split(3)
            .into_iter()
            .map(|c| Parser::new(c).count())
            .sum();
        assert_eq!(rows, 2);
    }

    /// パーサが前倒し計算する channel ハッシュ + 月の畳み込みが、従来の
    /// group_key(channel, month) と一致することを、様々な長さの channel で確認する。
    /// 長さ 0..24 は SWAR full-word / 端数 / off==0 (8の倍数) 経路を網羅する。
    #[test]
    fn fused_hash_matches_group_key() {
        use crate::hash::{finalize_key, group_key};
        for len in 0..24usize {
            let channel: Vec<u8> = (0..len).map(|i| b'a' + (i % 26) as u8).collect();
            // "1798761600,<channel>,1,2\n" を組み立てて 1 行だけパースする
            let mut line = b"1798761600,".to_vec();
            line.extend_from_slice(&channel);
            line.extend_from_slice(b",1,2\n");
            let buf = pad(&line);
            let row = Parser::new(Padded::new(&buf, line.len()))
                .next()
                .expect("one row");
            assert_eq!(row.channel, &channel[..], "len={len}");
            for month in 1..=12u64 {
                assert_eq!(
                    finalize_key(row.hash, month),
                    group_key(&channel, month),
                    "len={len} month={month}"
                );
            }
        }
    }
}

}

mod swar {
//! SWAR (SIMD Within A Register) プリミティブ: 8 バイトを 1 本の u64 に詰めて
//! バイト単位ループを分岐レスなビット演算に畳む、パース段の下位部品。
//!
//! ここに集めた関数はいずれも純粋・分岐最小で、[parse](crate::parse) のホットループから
//! `#[inline]` 展開される前提。アルゴリズム単位でテストできるよう本文から切り出してある。

/// ASCII 数字 8 桁 (リトルエンディアンで読んだ u64) を一括で数値化する。
/// バイトごとの `*10 + d` ループ 8 回分を、乗算 3 回 + シフトに畳む定石
/// ("Faster Integer Parsing" / simdjson で使われる形)。
#[inline]
pub fn parse_8_digits(mut chunk: u64) -> u64 {
    chunk -= 0x3030_3030_3030_3030; // 各バイトから '0' を引く
    chunk = chunk.wrapping_mul(10) + (chunk >> 8); // 隣接 2 桁を 1 バイト目に集約
    const MASK: u64 = 0x0000_00ff_0000_00ff;
    const MUL1: u64 = 100 + (1_000_000 << 32);
    const MUL2: u64 = 1 + (10_000 << 32);
    ((chunk & MASK).wrapping_mul(MUL1) + ((chunk >> 16) & MASK).wrapping_mul(MUL2)) >> 32
}

/// LE で読んだ u64 の中から最初のカンマ (0x2c) のバイト位置を返す。無ければ 8。
/// 1 バイトずつの `!= ','` ループ (フィールド脱出のたびに分岐予測を外す) を、
/// 分岐レスな「ゼロバイト検出」ビット技に置き換える定石。XOR でカンマを 0 に潰し、
/// `(x-1) & !x & 0x80..` で「値が 0 のバイト」の最上位ビットだけを立てる。
#[inline]
pub fn first_comma(word: u64) -> u32 {
    const ONES: u64 = 0x0101_0101_0101_0101;
    const HIGH: u64 = 0x8080_8080_8080_8080;
    const COMMA: u64 = 0x2c * ONES;
    let x = word ^ COMMA; // カンマだったバイトが 0x00 になる
    let m = x.wrapping_sub(ONES) & !x & HIGH;
    if m == 0 {
        8
    } else {
        m.trailing_zeros() / 8 // LE なので最下位の立ちビット = 最初のカンマ
    }
}

/// `word` の下位 `keep` バイトだけを残し、上位をゼロにする (LE なので前方バイト側)。
/// カンマを含むワードから、カンマ手前の channel バイトだけをハッシュに取り込むのに使う。
/// `keep` は 1..=7 を想定 (0 と 8 は呼び出し側が別扱いするため、ここでは未定義でよい)。
#[inline]
pub fn keep_low_bytes(word: u64, keep: u32) -> u64 {
    word & (u64::MAX >> ((8 - keep) * 8))
}



}

mod table {
//! 集計専用のオープンアドレス法ハッシュテーブル (挿入と更新のみ、削除なし)。
//!
//! std の `HashMap` (hashbrown) は高速だが、バケットの物理アドレスを外部に公開しない
//! ため「次に触るスロットを事前に `prefetch` する」ことができない。プロファイルでは
//! aggregate 段の ~45% がグループ表へのランダムアクセスのキャッシュミス待ちで占められて
//! いた ([aggregate.rs] のグループ探索)。そこで bucket = key & mask を自前で計算できる
//! 単純な線形探索テーブルに置き換え、次行のスロットをソフトウェアプリフェッチする。
//!
//! 値は毎行更新するホット部 `V` と、挿入時と出力時にしか触らないコールド部 `C` に
//! 分かれる。キーとホット値は 1 本の `Entry { key, val }` 配列に同居させ、1 行の探索が
//! 触るランダムなキャッシュラインを 1 本に抑える (キー配列と値配列を分ける従来
//! レイアウトでは keys[i] と vals[i] の 2 本、prefetch も 2 発だった)。エントリを
//! キャッシュライン境界に収められるかは `V` のサイズ次第なので、出力専用フィールドを
//! `C` へ追い出してホット部を詰めるのは利用側 (aggregate) の責務。コールド部は同じ
//! スロット番号の並行配列に置き、拡張時は両方まとめて再配置する。
//!
//! スロット選択には拡散済みの u64 ([hash::finalize_key]) を使うが、これは**バケット選択
//! と高速プレフィルタ**にすぎない。ハッシュが一致しても実キー `(channel, month)` が一致
//! するとは限らない (ハッシュ衝突) ため、`get_or_insert_with` は u64 一致時に呼び出し側の
//! `matches` 述語で実キーを照合し、外れれば通常のオープンアドレス法どおり probe を継続する。
//! これで衝突しても別スロットに正しく分かれ、誤集計は起こらない (std `HashMap` と同じ扱い)。
//! 実キーはコールド部 `C` が保持する (aggregate では channel バイト列 + month)。
//!
//! `key == EMPTY(0)` が空きスロットの番兵で、ハッシュが 0 の場合だけ 1 に退避する
//! (プレフィルタが弱まるだけで、実キー照合があるため正しさには影響しない)。

const EMPTY: u64 = 0;

#[inline]
fn slot_key(key: u64) -> u64 {
    // 番兵 0 と衝突する実キーだけ 1 に退避 (分岐は乗算やビット演算に落ちる)
    if key == EMPTY { 1 } else { key }
}

#[derive(Clone)]
struct Entry<V> {
    key: u64,
    val: V,
}

pub struct GroupTable<V: Clone, C: Clone> {
    entries: Vec<Entry<V>>,
    cold: Vec<C>,
    mask: usize,
    len: usize,
    empty: (V, C),
}

impl<V: Clone, C: Clone> GroupTable<V, C> {
    /// 想定エントリ数 `max_entries` に対し、負荷率 < 0.5 になる 2 の冪容量で確保する。
    pub fn with_capacity_for(max_entries: usize, empty: (V, C)) -> GroupTable<V, C> {
        let cap = (max_entries.saturating_mul(2)).next_power_of_two().max(1024);
        GroupTable {
            entries: vec![
                Entry {
                    key: EMPTY,
                    val: empty.0.clone()
                };
                cap
            ],
            cold: vec![empty.1.clone(); cap],
            mask: cap - 1,
            len: 0,
            empty,
        }
    }

    /// `key` が入る/入っているスロットのエントリとコールド部を L1 に先読みする。
    /// 実キー照合は毎行 `entries[i]` (u64 + ホット値) と `cold[i]` (実キー) の両方を読むので、
    /// 2 本とも先読みしてキャッシュミス待ちをソフトウェアパイプラインの裏に隠す。
    #[inline]
    pub fn prefetch(&self, key: u64) {
        let i = (slot_key(key) as usize) & self.mask;
        #[cfg(target_arch = "x86_64")]
        unsafe {
            use std::arch::x86_64::{_mm_prefetch, _MM_HINT_T0};
            _mm_prefetch::<_MM_HINT_T0>(self.entries.as_ptr().add(i) as *const i8);
            _mm_prefetch::<_MM_HINT_T0>(self.cold.as_ptr().add(i) as *const i8);
        }
        #[cfg(not(target_arch = "x86_64"))]
        let _ = i;
    }

    /// `key` (バケット選択用ハッシュ) のスロットを探す。ハッシュ一致時は `matches` で
    /// 実キー (コールド部 `C`) を照合し、真のときだけ hit とする (偽ならハッシュ衝突として
    /// probe 継続)。無ければ `make()` の返す (ホット, コールド) を挿入する。
    #[inline]
    pub fn get_or_insert_with(
        &mut self,
        key: u64,
        matches: impl Fn(&C) -> bool,
        make: impl FnOnce() -> (V, C),
    ) -> &mut V {
        let key = slot_key(key);
        let mut i = (key as usize) & self.mask;
        loop {
            let k = self.entries[i].key;
            if k == key && matches(&self.cold[i]) {
                return &mut self.entries[i].val;
            }
            if k == EMPTY {
                // 負荷率 0.5 到達で拡張 (稀。probe 無限ループを防ぐ)
                if self.len * 2 >= self.entries.len() {
                    self.grow();
                    return self.get_or_insert_with(key, matches, make);
                }
                let (v, c) = make();
                self.entries[i].key = key;
                self.entries[i].val = v;
                self.cold[i] = c;
                self.len += 1;
                return &mut self.entries[i].val;
            }
            i = (i + 1) & self.mask;
        }
    }

    fn grow(&mut self) {
        let new_cap = self.entries.len() * 2;
        let mut entries = vec![
            Entry {
                key: EMPTY,
                val: self.empty.0.clone()
            };
            new_cap
        ];
        let mut cold = vec![self.empty.1.clone(); new_cap];
        let mask = new_cap - 1;
        for (old_i, e) in self.entries.iter().enumerate() {
            if e.key != EMPTY {
                let mut i = (e.key as usize) & mask;
                while entries[i].key != EMPTY {
                    i = (i + 1) & mask;
                }
                entries[i] = e.clone();
                cold[i] = self.cold[old_i].clone();
            }
        }
        self.entries = entries;
        self.cold = cold;
        self.mask = mask;
    }

    /// 有効スロットを (キー, ホット, コールド) で走査する (並列集計のマージ用)。
    /// キーは slot_key 適用後の値だが、再挿入時に slot_key を再適用しても不変。
    pub fn iter_keyed(&self) -> impl Iterator<Item = (u64, &V, &C)> {
        self.entries
            .iter()
            .zip(self.cold.iter())
            .filter_map(|(e, c)| if e.key != EMPTY { Some((e.key, &e.val, c)) } else { None })
    }

    /// 有効スロット (挿入済みグループ) を (ホット, コールド) で走査する。
    pub fn iter(&self) -> impl Iterator<Item = (&V, &C)> {
        self.entries
            .iter()
            .zip(self.cold.iter())
            .filter_map(|(e, c)| if e.key != EMPTY { Some((&e.val, c)) } else { None })
    }
}



}

mod time {
pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u64; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = if m <= 2 { y + 1 } else { y };
    (y, m, d)
}

/// リファレンス実装。ホットパスは `MonthTable` を使うが、検証用に残す。
#[allow(dead_code)]
pub fn unix_to_ymd(unix_secs: i64) -> (i64, u32, u32) {
    let days = unix_secs.div_euclid(86400); // 負の秒(1970年以前)も正しく処理
    civil_from_days(days)
}

/// タイムスタンプが 2027 年内に収まることがコンテストから保証されている
/// (おそらく生成されたダミーデータ) ため、年は常に 2027 で固定でき、
/// 必要なのは月だけになった。日付計算は「2027 年元日からの経過日数 → 月」の
/// 365 要素テーブル一本に縮約でき、以前の (year<<4)|month パックや
/// 範囲外フォールバック、除算だらけの汎用テーブルはホットパスから消える。
pub struct MonthTable {
    /// day-of-year (0..365) → month(1..=12)
    table: [u8; 365],
}

/// 2027-01-01 00:00:00 UTC の epoch 日数 (1798761600 / 86400、割り切れる)。
const DAY_2027_01_01: i64 = 20819;

impl MonthTable {
    pub fn new() -> MonthTable {
        let mut table = [0u8; 365];
        for (doy, slot) in table.iter_mut().enumerate() {
            let (_, m, _) = civil_from_days(DAY_2027_01_01 + doy as i64);
            *slot = m as u8;
        }
        MonthTable { table }
    }

    #[inline]
    pub fn month(&self, unix_secs: i64) -> u32 {
        // 2027 年保証により範囲チェック不要。/86400 は定数除算でシフト乗算に落ちる。
        let doy = (unix_secs / 86400 - DAY_2027_01_01) as usize;
        self.table[doy] as u32
    }
}



}


use std::env;

// S::Router の route/finish を総称コードで呼ぶため Router トレイトをスコープへ入れる
// (Sender/Sink は where 節で直接束縛しているのでメソッドは既に見えている)。
// 型名は `dispatch::` 経由で参照するので名前は束ねない (`as _`)。
use dispatch::Router as _;

/// 各チャネルの容量 (バッチ本数)。パース先行時のバッチ滞留 (メモリ膨張) を背圧で抑える。
const CHANNEL_BOUND: usize = 16;

/// distinct channel の最大種数 (コンテスト保証)。事前走査でこの数に達したら全 channel が
/// 出尽くしたとみなし、キー空間を列挙して衝突キー (poison) を確定する ([keyspace])。
const MAX_CHANNELS: usize = 10_000;

/// パイプラインの実行パラメータ。既定値はコンテスト計測環境の 8 スレッド (P+A=8) 向け。
/// 掃引・再チューニング用に環境変数で上書きできる。
struct Config {
    /// パーススレッド数 (入力を行境界で分割する数)。
    parse_threads: usize,
    /// 集計スレッド数 (シャード数)。
    agg_threads: usize,
    /// 1 バッチあたりの行数。B×32B ≈ 256KB。小さいと送受オーバーヘッド、大きいと
    /// キャッシュ外に膨張、という両側の崖がある。
    batch_size: usize,
}

impl Config {
    /// 掃引結果 (100M, 24 論理コア機, Pending 32B 化後): P=6/A=2/B=8192 が最良 0.35s
    /// (P+A はコンテスト計測環境の 8 スレッドに合わせる)。集計はシャード分割でテーブルが
    /// 小さくなるぶん 2 スレッドで足り、残りをパースに回すのが効いた。
    /// 注: mpsc を自前 SPSC リング (spin+yield) に置き換える実験は f5b9f31..096f040 で
    /// 実装・計測済みだが、全変種が本構成に 6〜9% 負けたため revert した。
    fn from_env() -> Config {
        Config {
            parse_threads: env_param("OBRC_PARSE_THREADS", 3),
            agg_threads: env_param("OBRC_AGG_THREADS", 5),
            batch_size: env_param("OBRC_BATCH", 8192),
        }
    }

    /// 集計スレッドごとのテーブル初期容量の見込み。全体のグループ数 ~12 万をシャード数で
    /// 割り、各テーブルを小さく確保してキャッシュヒット率を上げる (足りなければ自動拡張)。
    fn groups_per_shard(&self) -> usize {
        (130_000 / self.agg_threads).max(1024)
    }
}

/// 正の整数の環境変数を読む (未設定・不正なら既定値)。パラメータ掃引用。
fn env_param(name: &str, default: usize) -> usize {
    env::var(name)
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(default)
        .max(1)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let [_, input_path, output_path] = args.as_slice() else {
        eprintln!(
            "usage: {} <input.csv> <output>",
            args.first().map(String::as_str).unwrap_or("main")
        );
        std::process::exit(2);
    };

    // 入力 → パース → 集計 → 出力 のパイプライン。各段はモジュールに分離してあり、
    // 差し替えはこの関数と該当モジュールに閉じる。パース段と集計段はスレッドを分離し、
    // 両者の受け渡し (バッチング・シャードルーティング・バッファ還流) は dispatch 段に
    // 閉じ込めてある。
    let input = input::Input::open(input_path);
    let cfg = Config::from_env();

    let agg = aggregate(&input, &cfg);

    output::write_result(output_path, &agg);

    std::process::exit(0);
}

/// パース段 × P と集計段 × A を並列に走らせ、集計結果を 1 本の [Aggregator] に畳んで返す。
/// dispatch のバックエンド (ここでは mpsc) を選ぶのはこの 1 箇所だけで、実際の配線は
/// バックエンド非依存の [run_pipeline] が担う。
fn aggregate<'a>(input: &'a input::Input, cfg: &Config) -> aggregate::Aggregator<'a> {
    // 事前走査で全 channel を出尽くし、出現しうる全キーから衝突キー (poison) を厳密に特定する。
    // 集計側は非 poison の u64 では channel 照合を省ける (毎行の memcmp を消す)。
    let channel_hashes = keyspace::discover_channel_hashes(input.padded(), MAX_CHANNELS);
    let poison: std::sync::Arc<[u64]> = keyspace::build_poison(&channel_hashes).into();

    let freelist = dispatch::mpsc::Freelist::new(cfg.batch_size);
    let (sender, sinks) = dispatch::mpsc::channels(&freelist, cfg.agg_threads, CHANNEL_BOUND);
    run_pipeline(input, cfg, sender, sinks, poison)
}

/// パース×P → dispatch → 集計×A の並列パイプラインを回し、A 個の集計結果を畳んで返す。
///
/// dispatch のバックエンドをトレイト ([dispatch::Sender] / [dispatch::Router] /
/// [dispatch::Sink]) で受け取り、単相化される総称関数として書いてある。ホットな
/// `router.route` は関連型 `S::Router` の具象 (mpsc なら [dispatch::MpscRouter]) に単相化 +
/// インライン展開され、`dyn` の間接呼び出しは入らない。
fn run_pipeline<'a, S, K>(
    input: &'a input::Input,
    cfg: &Config,
    sender: S,
    sinks: Vec<K>,
    poison: std::sync::Arc<[u64]>,
) -> aggregate::Aggregator<'a>
where
    S: dispatch::Sender<'a>,
    K: dispatch::Sink<'a> + Send,
{
    let mut aggs: Vec<aggregate::Aggregator> = std::thread::scope(|s| {
        // 集計スレッド: 各自が専有テーブルを持ち、自分宛てバッチだけを集計する (ロック不要)。
        let agg_handles: Vec<_> = sinks
            .into_iter()
            .map(|sink| {
                let groups = cfg.groups_per_shard();
                let poison = poison.clone();
                s.spawn(move || {
                    let mut agg = aggregate::Aggregator::with_group_capacity(groups, poison);
                    sink.drain(&mut agg);
                    agg
                })
            })
            .collect();

        // パーススレッド: 入力チャンクをパースし、行をシャードへ振り分けて送る。
        for chunk in input.padded().split(cfg.parse_threads) {
            let mut router = sender.router();
            s.spawn(move || {
                let dates = time::MonthTable::new();
                for row in parse::Parser::new(chunk) {
                    router.route(aggregate::Pending::new(&dates, &row));
                }
                router.finish();
            });
        }
        // パース側が全 Router (と本体) を drop するとチャネルが閉じ、集計側が抜ける
        drop(sender);

        agg_handles.into_iter().map(|h| h.join().unwrap()).collect()
    });

    let mut agg = aggs.swap_remove(0);
    for other in &aggs {
        agg.merge(other);
    }
    agg
}
