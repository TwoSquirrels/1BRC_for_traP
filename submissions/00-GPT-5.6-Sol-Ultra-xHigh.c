#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <immintrin.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// このファイルは optimized/c/main.c の実行コードを変えず、採用された仕組みと成立条件を日本語で説明した版である。
// 処理は「CSVを行境界で分割する」「各ワーカーがチャンネルと月ごとに集計する」「ワーカーの結果を併合する」「指定形式で出力する」の四段階からなる。
// 1行ごとの処理が10億回繰り返されるため、主な最適化対象はCSVの数値変換、チャンネル検索、ランダムな集計先への書き込みである。
// 素朴なfgets、文字列分割、汎用数値変換、動的ハッシュ表、行ごとの書式出力を、mmap上の直接解析、固定表、SIMD、まとめ書きへ置き換えている。
// 実測値は元コードに残されていた対象EC2の値であり、個別計測のない技法には推測の数値を付けていない。
// 各差分は別時点の全体計測なので単純に足し合わせることはできず、CPU、コンパイラ、入力分布が変われば効果も変わる。
// __builtin_expectはコード配置と分岐予測へのヒント、always_inline、noinline、coldはホットコードの大きさを制御する指定であり、フォールバック処理自体は削除しない。
//
// 最適化テクニック：コンパイル対象CPUを固定する。
// BZHIとPEXTにはBMI2、CRC32にはSSE 4.2、整数の内積にはAVX-VNNI、三項論理演算にはAVX-512が必要である。
// 集計演算そのものは128 bitだが、AVX-512を有効にするとGCCが32本のベクトルレジスタを使えるため、レジスタ退避が減る。
// 1B行の計測では、AVX-512を外した同等コードより約10 ms、汎用スケジューリングより約0〜10 ms短かった。
// 実行環境がこの命令セットを持つことを前提にしたコンテスト専用コードであり、一般配布用なら実行時CPU判定と汎用版が必要になる。
#pragma GCC target("avx2,bmi2,sse4.2,avxvnni,avx512bw,avx512vl,tune=sapphirerapids")

// 通常表は最大10,000キーに対して16,384スロットを固定確保し、負荷率を約61%以下に保つ。
// 2の累乗なので剰余はbit maskになり、解析中の再確保とrehashも不要である。
#define MAP_CAPACITY (1u << 14)
// チャンネルパスは最大5階層×20文字+区切り4文字=104バイトなので、10,000種類でも1,040,000バイトで1 MiBに収まる。
// キーを個別mallocせず連続arenaへ詰めると、allocator呼び出し、ポインタ追跡、メモリ断片化を避けられる。
#define KEY_ARENA_CAPACITY (1u << 20)
#define INPUT_PAGE_SIZE 4096u
#define AGGS_THP_BYTES (2u << 20)
#ifndef FAST_BITS
// 最適化テクニック：既知のチャンネルをハッシュ表の探索なしでIDへ変換する三段の直引き表を使う。
// 第1表を512 KiBと大きくして最初の衝突を減らし、衝突時だけ128 KiBの第2表と第3表を読む。
// 10,000チャンネルをすべて発見した後は行の99.9605%をいずれかの表から直接引ける。
// 三表を同じ大きさにする構成より1B行で約20〜50 ms短く、三表、キー、通常のハッシュ表を2 MiBに収められる配分でもある。
#define FAST_BITS 18
#endif
#define FAST_CAPACITY (1u << FAST_BITS)
#define FAST2_BITS 16
#define FAST2_CAPACITY (1u << FAST2_BITS)
#define FAST3_BITS 16
#define FAST3_CAPACITY (1u << FAST3_BITS)
#ifndef MPH_BUCKET_BITS
// MPHは下位10 bitで1,024個のバケットに分け、バケットごとの乗数で衝突しない配置を探す。
// 10 bitなら公開データの10,000キーを制限回数内で配置でき、シード表も4 KiBに留まる。
// 9 bitでは一つのバケットが大きすぎて配置に失敗し、11 bitでは表の参照コストが増えて1B行で約5〜11 ms遅かった。
#define MPH_BUCKET_BITS 10
#endif
#define MPH_BUCKET_CAPACITY (1u << MPH_BUCKET_BITS)
#define MPH_SLOT_BITS 14
#define MPH_SLOT_CAPACITY (1u << MPH_SLOT_BITS)
#ifndef MPH_ACTIVE_SLOTS
// 14 bitの16,384スロットは10,000 IDを収められる最小の2の累乗である。
// 15,360スロットへ狭めても差は1 ms以内なので、境界処理が単純な全域を使う。
// 14,848以下では公開10,000キーを制限試行内に配置できず、通常表へ戻ってしまう。
#define MPH_ACTIVE_SLOTS MPH_SLOT_CAPACITY
#endif
// MPHのID表は32 KiBであり、一般的なL1データキャッシュと同じ大きさなので、これ以上スロットを増やすと頻繁なL1ミスを招く。
// シードは16 bitにも収まるが、32 bit配列にした配置のほうが1B行で約11〜13 ms短かったため、4 KiBを使う。
// IDを特定のキャッシュラインへ密集させるとキャッシュセットの競合が増えるため、自然な配置を保つ。
// 直引きは10,000チャンネルをすべて発見して入力の種類を閉じられた後だけ有効にし、それ以前とハッシュ衝突時はバイト列まで比較する通常表へ戻る。
#define CONTEST_CHANNELS 10000u
#define YEAR_START 1798761600u
#ifndef HASH_MIDDLE_THRESHOLD
// 16バイトを超えるキーだけ中央8バイトもハッシュへ混ぜ、長い階層パスの偏りを減らす。
// 中央を常に読むと短いキーにも余分なロードが発生するため、公開データで長いキーが少ないことに合わせた閾値である。
// 閾値18〜20も正確だがMPH配置が変わり1B行で約15〜25 ms遅かったため、対象データでは16を使う。
#define HASH_MIDDLE_THRESHOLD 16u
#endif
#ifndef WORKER_LIMIT
#define WORKER_LIMIT 0
#endif

typedef struct {
  // 入力契約により、チャンネルと月ごとの三つの合計値はuint32_tに収まる。
  // 三つの32 bitカウンタと二つの16 bit極値を16バイトに詰めると、1回の128 bit SIMD演算で更新できる。
  // カウンタと極値を別配列にするとロードとストアが分かれ、1B行では2.999秒から3.527秒へ悪化した。
  uint32_t total_len, stamps, messages;
  // 最大値は65535-max_lenとして反転保存する。
  // これにより最小値と最大値の両方を符号なし16 bitのmin命令一つで更新できる。
  uint16_t min_len, inv_max_len;
} Stats;
typedef struct {
  uint32_t min_len, max_len;
} WideStats;
typedef struct {
  Stats month[12];
} ChannelAgg;
typedef struct {
  WideStats month[12];
} WideChannelAgg;
typedef struct {
  // MapEntryは64 bitポインタ、32 bitハッシュ、16 bit長、16 bit IDの合計16バイトで、4要素が1キャッシュラインに収まる。
  // キーはNUL終端せずlenを別に持ち、arenaの1バイトも終端文字へ使わない。
  const char *key;
  uint32_t hash;
  uint16_t len, id;
} MapEntry;
typedef struct {
  MapEntry *entries;
  ChannelAgg *aggs;
  char *keys;
  uint16_t *fast_ids, *fast2_ids, *fast3_ids;
  uint32_t *mph_seeds;
  uint16_t *mph_ids;
  uint32_t size, agg_cap, key_used;
  void *aggs_alloc;
  size_t aggs_mmap_size;
  void *dictionary_alloc;
  // 入力契約は個々のmessage_lengthを16 bitに制限しないため、65535以上の極値だけ32 bitの補助表へ保存する。
  WideChannelAgg *wide;
} FlatMap;
typedef struct {
  const char *begin, *end;
  const char *drop_cursor;
  const FlatMap *dictionary;
  FlatMap map;
  int canonical, cpu;
#ifdef PROFILE
  double elapsed;
#endif
} Worker;

// 各ワーカーは自分専用のFlatMapだけを書き換えるため、通常の行集計にmutexやatomic加算は要らない。
// スレッド間で共有するのは公開後にキー領域が不変になる辞書だけで、公開ポインタの切り替えに限ってatomicを使う。

// 最適化テクニック：最初に全10,000チャンネルを発見したワーカーの辞書を、全ワーカーで共有する。
// 共有後は同じチャンネルが全ワーカーで同じIDを持つため、各ワーカーはキーを保持せずID別集計表だけを更新できる。
// 辞書を作るためだけの直列事前走査が不要になり、1B行では約25〜34 ms短い。
// 入力が10,000種類未満なら公開しないため、小さな正当入力も各ワーカーの通常表を最後に併合して正しく処理できる。
static FlatMap *global_dictionary;
// 実在するFlatMapはアラインメントにより1や2にはならないため、この二値を構築中と構築失敗の番兵に使える。
#define DICTIONARY_BUILDING ((FlatMap *)(uintptr_t)1)
#define DICTIONARY_FAILED ((FlatMap *)(uintptr_t)2)

static const uint32_t month_start[13] = {
    1798761600u, 1801440000u, 1803859200u, 1806537600u, 1809129600u,
    1811808000u, 1814400000u, 1817078400u, 1819756800u, 1822348800u,
    1825027200u, 1827619200u, 1830297600u};
static const char month_label[12][8] = {
    "2027-01", "2027-02", "2027-03", "2027-04", "2027-05", "2027-06",
    "2027-07", "2027-08", "2027-09", "2027-10", "2027-11", "2027-12"};
// 最適化テクニック：Unix秒の先頭8桁から月を引く完全なルックアップ表を使う。
// 先頭8桁は100秒単位の時刻であり、1日は864単位、すべての月境界は32単位でも割り切れる。
// したがって「(先頭8桁-YEAR_START/100)/32」を添字にしても月境界をまたがず、下2桁を読まなくても正確である。
// 素朴な実装の除算や12個の境界との比較を各行から除き、1B行で約40〜60 ms短くした。
static uint8_t month_by_period[(315360u + 31u) / 32u];

static void die(const char *s) {
  perror(s);
  exit(1);
}
static __attribute__((noinline, cold, no_profile_instrument_function)) void
finish_profile(void) {
// PGO用ビルドでは_exitより前にカウンタを書き出す。
// 通常ビルドの空asmはコンパイラに副作用境界だけを示し、この低頻度関数をホットコードから分離する。
#ifdef GCC_PROFILE_GENERATE
  extern void __gcov_dump(void);
  __gcov_dump();
#else
  __asm__ volatile("" ::: "memory");
#endif
}
#ifdef PROFILE
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
#endif
static uint64_t load64(const char *p) {
  // memcpyを介すと、任意アラインメントのchar列を別型ポインタで読むことによるCの未定義動作を避けられる。
  // サイズが定数なので、GCCは実際には関数呼び出しではなく1回の非整列ロードへ展開する。
  uint64_t x;
  memcpy(&x, p, 8);
  return x;
}
static uint32_t load32(const char *p) {
  uint32_t x;
  memcpy(&x, p, 4);
  return x;
}
static uint16_t load16(const char *p) {
  uint16_t x;
  memcpy(&x, p, 2);
  return x;
}
static inline __attribute__((always_inline)) int
key_equal(const char *a, const char *b, uint32_t n) {
  // 最適化テクニック：短いキーは先頭と末尾の固定幅ロードで比較し、可変長memcmpの呼び出しとループを避ける。
  // 8バイト境界をまたぐ長さでは先頭と末尾が重なるが、同じバイトを二度比べるだけなので結果はmemcmp(a,b,n)==0と同じである。
  // 32バイトを超える部分だけはコード量を増やさないようにmemcmpへ任せる。
  if (n >= 8) {
    if (load64(a) != load64(b))
      return 0;
    if (n == 8)
      return 1;
    if (n <= 16)
      return load64(a + n - 8) == load64(b + n - 8);
    if (load64(a + 8) != load64(b + 8))
      return 0;
    if (n <= 24)
      return load64(a + n - 8) == load64(b + n - 8);
    if (n <= 32)
      return load64(a + 16) == load64(b + 16) &&
             load64(a + n - 8) == load64(b + n - 8);
    return !memcmp(a + 16, b + 16, n - 16);
  }
  if (n >= 4) {
    if (load32(a) != load32(b))
      return 0;
    return n == 4 || load32(a + n - 4) == load32(b + n - 4);
  }
  if (n >= 2) {
    if (load16(a) != load16(b))
      return 0;
    return n == 2 || load16(a + n - 2) == load16(b + n - 2);
  }
  return !n || *a == *b;
}
static uint32_t hash_bytes(const char *p, size_t n) {
  uint64_t first = load64(p);
  uint64_t short_x = _bzhi_u64(first, (unsigned)n * 8u);
  // 最適化テクニック：キー全体を走査せず、長さ、先頭8バイト、末尾8バイト、必要なら中央8バイトをCRC32へ入れる。
  // 16バイト以下なら端だけで全バイトを覆い、長いキーでも異なる階層パスを十分に散らせる。
  // ハッシュ一致後は必ずkey_equalで確認するため、異なるキーが同じ32 bit値になっても集計は混ざらない。
  // このサンプリングは全バイトのハッシュよりロードが少なく、1B行で約40〜80 ms短かった。
  uint64_t long_x = first ^ load64(p + n - 8);
  uint64_t x = short_x;
  // nが8以下かどうかはほぼ半々で予測しにくいため、短い場合と長い場合を先に計算し、CMOVAで分岐せず選ぶ。
  // これにより1B行の時間は約2.70秒から2.42秒へ短縮した。
  // n<8でもp+n-8は同じCSV行の時刻と区切り部分を指し、後述のガードページもあるためロード自体は安全である。
  // そのロード値はCMOVAで捨てられ、短いキーにはBZHIでキー長を超える上位バイトをゼロにしたshort_xだけが使われる。
  __asm__("cmpq $8, %[length]\n\tcmova %[long_hash], %[hash]"
          : [hash] "+r"(x)
          : [length] "r"(n), [long_hash] "r"(long_x)
          : "cc");
  // 中央ロードは長いキーだけに必要なので、出現頻度の低い分岐として残す。
  // 常時ロードする分岐なし版は1B行で命令数が約56億増え、約50 ms遅かった。
  if (__builtin_expect(n > HASH_MIDDLE_THRESHOLD, 0))
    x ^= load64(p + n / 2 - 4);
  // CRC32は暗号学的強度ではなく、少ない命令で32 bitへ拡散する目的で使う。
  // 長さnを初期値にするため、同じサンプルバイトを持つ長さ違いのキーも別の値になりやすい。
  return (uint32_t)_mm_crc32_u64(n, x);
}
static inline uint32_t fast3_index(uint32_t hash) {
  // 第3表では乗法ハッシュの上位bitを使い、第1表の下位bit、第2表の上位bitとは異なる衝突パターンを作る。
  return (hash * UINT32_C(0x9e3779b1)) >> (32 - FAST3_BITS);
}
static void *mmap_dictionary_thp(void) {
  // 最適化テクニック：4 MiBを予約して、その中の2 MiB境界に揃った2 MiBだけを残す。
  // mmapが返す通常の4 KiBアラインメントだけではTransparent Huge Pageの境界を保証できないためである。
  // 前後をmunmapした後にMADV_HUGEPAGEを指定し、TLB変換を数百ページ分から原則1ページ分へ減らす。
  // MADV_HUGEPAGE呼び出しが成功しても巨大ページ化は保証されないが、通常ページのままでも内容の正しさは変わらない。
  void *reservation = mmap(NULL, AGGS_THP_BYTES * 2u,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED)
    die("mmap dictionary hugepage");
  uintptr_t raw = (uintptr_t)reservation;
  uintptr_t aligned =
      (raw + AGGS_THP_BYTES - 1u) & ~(uintptr_t)(AGGS_THP_BYTES - 1u);
  size_t prefix = (size_t)(aligned - raw);
  size_t suffix = (size_t)(raw + AGGS_THP_BYTES * 2u -
                           (aligned + AGGS_THP_BYTES));
  if ((prefix && munmap((void *)raw, prefix)) ||
      (suffix && munmap((void *)(aligned + AGGS_THP_BYTES), suffix)))
    die("munmap dictionary trim");
  if (madvise((void *)aligned, AGGS_THP_BYTES, MADV_HUGEPAGE))
    die("madvise dictionary hugepage");
  return (void *)aligned;
}
static void map_init(FlatMap *m) {
  memset(m, 0, sizeof(*m));
  size_t entries_bytes = (size_t)MAP_CAPACITY * sizeof(MapEntry);
  size_t fast_bytes = (size_t)FAST_CAPACITY * sizeof(uint16_t);
  size_t fast2_bytes = (size_t)FAST2_CAPACITY * sizeof(uint16_t);
  size_t fast3_bytes = (size_t)FAST3_CAPACITY * sizeof(uint16_t);
  size_t dictionary_bytes =
      entries_bytes + KEY_ARENA_CAPACITY + fast_bytes + fast2_bytes +
      fast3_bytes;
  if (dictionary_bytes > AGGS_THP_BYTES) {
    errno = EOVERFLOW;
    die("dictionary hugepage");
  }
  // 通常表256 KiB、キー領域1 MiB、三つの直引き表768 KiBは合計2 MiBで、ちょうど一つの巨大ページに収まる。
  // 互いに近いデータを同じ巨大ページへ置くことでTLBミスを減らし、共有辞書の導入後は1B行で約38 ms短かった。
  char *dictionary = (char *)mmap_dictionary_thp();
  // 無名mmapはゼロ初期化されるため、MapEntryのNULLキーと直引き表の0を空き番兵としてそのまま使える。
  m->dictionary_alloc = dictionary;
  m->entries = (MapEntry *)dictionary;
  m->keys = dictionary + entries_bytes;
  m->fast_ids = (uint16_t *)(m->keys + KEY_ARENA_CAPACITY);
  m->fast2_ids = (uint16_t *)((char *)m->fast_ids + fast_bytes);
  m->fast3_ids = (uint16_t *)((char *)m->fast2_ids + fast2_bytes);
  // 集計表は最初から最大16,384チャンネル分を確保する。
  // 段階的なreallocを避けるだけでなく、後述の遅延更新が保持するStatsポインタを解析中ずっと有効にするためである。
  m->agg_cap = MAP_CAPACITY;
  size_t agg_bytes = (size_t)m->agg_cap * sizeof(ChannelAgg);
  m->aggs_alloc = calloc(1, agg_bytes + 63);
  // ChannelAggは12か月×16バイト=192バイト、すなわち64バイトのキャッシュライン3本分である。
  // 先頭を64バイト境界へ揃えると各チャンネルが余分なラインをまたがず、1B行で約10 ms短かった。
  m->aggs = (ChannelAgg *)(((uintptr_t)m->aggs_alloc + 63) & ~(uintptr_t)63);
  if (!m->aggs_alloc)
    die("calloc");
}
static void map_init_aggs(FlatMap *m, uint32_t channels) {
  memset(m, 0, sizeof(*m));
  size_t agg_bytes = (size_t)channels * sizeof(ChannelAgg);
  if (agg_bytes > AGGS_THP_BYTES) {
    errno = EOVERFLOW;
    die("worker aggs");
  }
  // 共有IDへ変換した後の集計表は10,000×192=1,920,000バイトなので、一つの2 MiB巨大ページに収まる。
  // 行ごとに約1.92 MiBの範囲へランダムアクセスするため、通常の4 KiBページ数百枚よりTLBへの負担が小さい。
  void *reservation = mmap(NULL, AGGS_THP_BYTES * 2u,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED)
    die("mmap aggs");
  uintptr_t raw = (uintptr_t)reservation;
  uintptr_t aligned =
      (raw + AGGS_THP_BYTES - 1u) & ~(uintptr_t)(AGGS_THP_BYTES - 1u);
  size_t prefix = (size_t)(aligned - raw);
  size_t suffix = (size_t)(raw + AGGS_THP_BYTES * 2u -
                           (aligned + AGGS_THP_BYTES));
  if ((prefix && munmap((void *)raw, prefix)) ||
      (suffix && munmap((void *)(aligned + AGGS_THP_BYTES), suffix)))
    die("munmap aggs trim");
  m->agg_cap = channels;
  m->aggs_alloc = (void *)aligned;
  m->aggs_mmap_size = AGGS_THP_BYTES;
  m->aggs = (ChannelAgg *)m->aggs_alloc;
  if (madvise(m->aggs_alloc, m->aggs_mmap_size, MADV_HUGEPAGE))
    die("madvise aggs hugepage");
  // 0はカウンタの初期値だがminの単位元ではないため、二つの極値だけ65535へ初期化する。
  for (uint32_t id = 0; id < channels; id++)
    for (unsigned month = 0; month < 12; month++) {
      m->aggs[id].month[month].min_len = UINT16_MAX;
      m->aggs[id].month[month].inv_max_len = UINT16_MAX;
    }
}
static void map_free(FlatMap *m) {
  if (m->dictionary_alloc) {
    if (munmap(m->dictionary_alloc, AGGS_THP_BYTES))
      die("munmap dictionary");
  } else {
    free(m->entries);
    free(m->keys);
    free(m->fast_ids);
    free(m->fast2_ids);
    free(m->fast3_ids);
  }
  if (m->aggs_mmap_size) {
    if (munmap(m->aggs_alloc, m->aggs_mmap_size))
      die("munmap aggs");
  } else {
    free(m->aggs_alloc);
  }
  free(m->wide);
}
static __attribute__((noinline, cold)) MapEntry *
map_insert(FlatMap *m, MapEntry *e, const char *key, uint32_t len,
           uint32_t hash) {
  if (m->size == m->agg_cap) {
    errno = EOVERFLOW;
    die("channels");
  }
  if (m->key_used + len > KEY_ARENA_CAPACITY) {
    errno = EOVERFLOW;
    die("channel keys");
  }
  // 元のmmap入力を後でMADV_DONTNEEDできるよう、初出キーだけをarenaへコピーする。
  // 二回目以降の同じキーは既存MapEntryを返すので、行数に比例したコピーや割り当ては発生しない。
  e->key = (const char *)memcpy(m->keys + m->key_used, key, len);
  m->key_used += len;
  e->hash = hash;
  e->len = (uint16_t)len;
  e->id = (uint16_t)m->size++;
  // 各表の0は空き、UINT16_MAXは衝突、その他はid*3+1を表す。
  // 同じ添字に二つ目のキーが入った時点で衝突番兵へ固定し、その表から誤ったIDを返さない。
  uint16_t *fast = &m->fast_ids[hash & (FAST_CAPACITY - 1)];
  *fast = *fast ? UINT16_MAX : (uint16_t)(e->id * 3 + 1);
  fast = &m->fast2_ids[hash >> (32 - FAST2_BITS)];
  *fast = *fast ? UINT16_MAX : (uint16_t)(e->id * 3 + 1);
  fast = &m->fast3_ids[fast3_index(hash)];
  *fast = *fast ? UINT16_MAX : (uint16_t)(e->id * 3 + 1);
  // 極値の番兵はキー挿入時に12か月分まとめて設定する。
  // 各集計行で「最初の値か」を分岐する素朴な実装より、低頻度の挿入側へ初期化を追い出したほうが速い。
  for (unsigned i = 0; i < 12; i++) {
    m->aggs[e->id].month[i].min_len = UINT16_MAX;
    m->aggs[e->id].month[i].inv_max_len = UINT16_MAX;
  }
  return e;
}
// 最適化テクニック：通常のハッシュ表探索をnoinlineにし、頻繁に実行する関数のコードとレジスタを圧迫させない。
// 全チャンネル発見後にこの探索へ来る行は0.8%未満なので、呼び出しコストよりホットループ縮小の効果が大きい。
// 1B行ではワーカー処理が2.902秒から2.867秒へ短くなった。
static __attribute__((noinline)) MapEntry *
map_find(FlatMap *m, const char *key, uint32_t len, uint32_t hash) {
  // 線形探索は隣接MapEntryを読むためキャッシュ局所性があり、削除がないので最初の空きで未登録と確定する。
  // MAP_CAPACITYが2の累乗なので、末尾から先頭への循環も剰余除算ではなくAND一命令である。
  uint32_t i = hash & (MAP_CAPACITY - 1);
  for (;;) {
    MapEntry *e = &m->entries[i];
    if (!e->key)
      return map_insert(m, e, key, len, hash);
    // hashとlenは安い絞り込みにすぎず、一致した候補は必ず全バイトを比較する。
    // 32 bitハッシュだけを同一性判定に使う実装は異なるチャンネルを混ぜるため、確率が低くても正しくない。
    if (e->hash == hash && e->len == len && key_equal(e->key, key, len))
      return e;
    i = (i + 1) & (MAP_CAPACITY - 1);
  }
}
static __attribute__((noinline)) const MapEntry *
map_find_readonly(const FlatMap *m, const char *key, uint32_t len,
                  uint32_t hash) {
  uint32_t i = hash & (MAP_CAPACITY - 1);
  for (;;) {
    const MapEntry *e = &m->entries[i];
    if (!e->key) {
      errno = EINVAL;
      die("published dictionary miss");
    }
    if (e->hash == hash && e->len == len && key_equal(e->key, key, len))
      return e;
    i = (i + 1) & (MAP_CAPACITY - 1);
  }
}
static inline __attribute__((always_inline)) size_t
dictionary_find_slot3_plus1(const FlatMap *m, const char *key, uint32_t len,
                            uint32_t hash) {
  // 共有辞書のMPHは「下位10 bitでシード選択、hash×seedの上位14 bitでスロット選択」という2回の配列参照である。
  uint32_t seed = m->mph_seeds[hash & (MPH_BUCKET_CAPACITY - 1)];
  uint32_t slot = (hash * seed) >> (32 - MPH_SLOT_BITS);
  uint16_t mph_id = m->mph_ids[slot];
  // 同じ32 bitハッシュを持つ別キーだけはUINT16_MAXへして、正確なバイト比較へ戻す。
  if (__builtin_expect(mph_id != UINT16_MAX, 1))
    return mph_id;
  return (size_t)map_find_readonly(m, key, len, hash)->id * 3u + 1u;
}
static inline __attribute__((always_inline)) size_t
map_find_slot3_plus1(FlatMap *m, const char *key, uint32_t len,
                     uint32_t hash) {
  // 発見中は第1表を読み、そこで衝突した行だけ第2表、さらに衝突した行だけ第3表を読む。
  // 三表すべてで衝突した場合に限り、通常表で線形探索してキーを正確に比較する。
  if (__builtin_expect(m->size == CONTEST_CHANNELS, 1)) {
    uint16_t id = m->fast_ids[hash & (FAST_CAPACITY - 1)];
    // 後段の表を常に読む分岐なし版は、衝突しない大多数の行にもランダムロードを課すため1B行で約100 ms遅い。
    // ここでは衝突時だけ分岐するほうが、分岐予測ミスの費用を含めてもメモリアクセス総数を減らせる。
    if (__builtin_expect(id == UINT16_MAX, 0))
      id = m->fast2_ids[hash >> (32 - FAST2_BITS)];
    if (__builtin_expect(id == UINT16_MAX, 0))
      id = m->fast3_ids[fast3_index(hash)];
    // 入力契約はチャンネルを最大10,000種類に制限するため、size==10,000以後に未知のキーは現れない。
    // したがって参照される添字は直IDか衝突番兵であり、空きを表す0を返すことはない。
    // 保存値id*3+1は、ChannelAggがキャッシュライン3本分であることを利用したアドレス計算用の符号化である。
    // base+(id*3+1)*64-64 = base+id*192となり、行ごとの減算と整数幅拡張を減らして1B行で約40〜50 ms短くした。
    if (__builtin_expect(id != UINT16_MAX, 1))
      return id;
  }
  return (size_t)map_find(m, key, len, hash)->id * 3u + 1u;
}
static inline __attribute__((always_inline)) void
stats_add(Stats *s, __m128i increment) {
  // 最適化テクニック：一つのStatsを128 bit整数ベクトルとしてロードし、合計、件数、最小、最大をまとめて更新する。
  // incrementの32 bit laneは順に{message_length, stamp_count, 1, 0}である。
  // 通常のCで書けば、total_len+=length、stamps+=stamp_count、messages++、min_len=min(min_len,length)、max_len=max(max_len,length)である。
  // ハードウェアキャッシュの手前に独自の書き戻しキャッシュを置くとタグ判定と競合が増え、1B行で約0.8〜1.0秒遅かった。
  __m128i values = _mm_loadu_si128((const __m128i *)s);
  __m128i result = _mm_add_epi32(values, increment);
  __m128i length = _mm_broadcastw_epi16(increment);
  // candidateの16 bit laneは{FFFF,FFFF,FFFF,FFFF,FFFF,FFFF,length,~length}になる。
  // 先頭6 laneではmin(x,FFFF)==xなので、直前の32 bit加算結果を壊さない。
  // 最後の2 laneではmin(old_min,length)とmin(~old_max,~length)==~max(old_max,length)を同時に計算する。
  // マスクやblendなしのVPMINUW一命令で両極値を更新でき、1B行で約30 ms短くした。
  __m128i candidate = _mm_ternarylogic_epi32(
      length, _mm_setr_epi16(0, 0, 0, 0, 0, 0, 0, -1),
      _mm_setr_epi16(0, 0, 0, 0, 0, 0, -1, 0), 0x35);
  result = _mm_min_epu16(result, candidate);
  _mm_storeu_si128((__m128i *)s, result);
}
static __attribute__((noinline, cold)) void
stats_add_wide(FlatMap *m, Stats *s, size_t id, uint32_t month,
               uint32_t length, uint32_t stamps) {
  // 65535は16 bit極値の番兵と重なるため、それ以上の長さだけ32 bitの低頻度経路で扱う。
  // 合計と件数は元のStatsの32 bitカウンタへ加え、極値だけWideStatsへ分離するので出力の意味は変わらない。
  if (!m->wide) {
    m->wide = (WideChannelAgg *)calloc(MAP_CAPACITY, sizeof(*m->wide));
    if (!m->wide)
      die("wide extrema");
  }
  WideStats *x = &m->wide[id].month[month];
  if (!x->max_len) {
    x->min_len = x->max_len = length;
  } else {
    if (length < x->min_len)
      x->min_len = length;
    if (length > x->max_len)
      x->max_len = length;
  }
  s->total_len += length;
  s->stamps += stamps;
  s->messages++;
}
static void map_merge(FlatMap *dst, const FlatMap *src) {
  if (src->wide && !dst->wide) {
    dst->wide = (WideChannelAgg *)calloc(MAP_CAPACITY, sizeof(*dst->wide));
    if (!dst->wide)
      die("wide merge");
  }
  for (uint32_t i = 0; i < MAP_CAPACITY; i++) {
    const MapEntry *a = &src->entries[i];
    if (!a->key)
      continue;
    MapEntry *b = map_find(dst, a->key, a->len, a->hash);
    for (unsigned j = 0; j < 12; j++) {
      const Stats *x = &src->aggs[a->id].month[j];
      Stats *y = &dst->aggs[b->id].month[j];
      if (!x->messages)
        continue;
      if (!y->messages)
        *y = *x;
      else {
        // 先頭三つの32 bit laneは加算し、末尾二つの16 bit laneはminを取ってblendする。
        // inv_max_len同士のminは、元のmax_len同士のmaxと等しい。
        __m128i xv = _mm_loadu_si128((const __m128i *)x);
        __m128i yv = _mm_loadu_si128((const __m128i *)y);
        __m128i sums = _mm_add_epi32(xv, yv);
        __m128i extrema = _mm_min_epu16(xv, yv);
        _mm_storeu_si128((__m128i *)y,
                         _mm_blend_epi16(sums, extrema, 0xc0));
      }
      if (src->wide) {
        const WideStats *wx = &src->wide[a->id].month[j];
        WideStats *wy = &dst->wide[b->id].month[j];
        if (wx->max_len) {
          if (!wy->max_len)
            *wy = *wx;
          else {
            if (wx->min_len < wy->min_len)
              wy->min_len = wx->min_len;
            if (wx->max_len > wy->max_len)
              wy->max_len = wx->max_len;
          }
        }
      }
    }
  }
}
static void map_merge_ids(FlatMap *dst, const FlatMap *src,
                          uint32_t channels) {
  if (src->wide && !dst->wide) {
    dst->wide = (WideChannelAgg *)calloc(MAP_CAPACITY, sizeof(*dst->wide));
    if (!dst->wide)
      die("wide merge");
  }
  for (uint32_t id = 0; id < channels; id++) {
    for (unsigned month = 0; month < 12; month++) {
      const Stats *x = &src->aggs[id].month[month];
      Stats *y = &dst->aggs[id].month[month];
      if (!x->messages)
        continue;
      if (!y->messages)
        *y = *x;
      else {
        // IDが共通ならキー検索なしで同じSIMD併合を行える。
        // 三つのカウンタを個別加算し二つの極値を個別比較する実装より、併合時間は約4.609 msから1.914 msへ短くなった。
        __m128i xv = _mm_loadu_si128((const __m128i *)x);
        __m128i yv = _mm_loadu_si128((const __m128i *)y);
        __m128i sums = _mm_add_epi32(xv, yv);
        __m128i extrema = _mm_min_epu16(xv, yv);
        _mm_storeu_si128((__m128i *)y,
                         _mm_blend_epi16(sums, extrema, 0xc0));
      }
      if (src->wide) {
        const WideStats *wx = &src->wide[id].month[month];
        WideStats *wy = &dst->wide[id].month[month];
        if (wx->max_len) {
          if (!wy->max_len)
            *wy = *wx;
          else {
            if (wx->min_len < wy->min_len)
              wy->min_len = wx->min_len;
            if (wx->max_len > wy->max_len)
              wy->max_len = wx->max_len;
          }
        }
      }
    }
  }
}
// 最適化テクニック：全10,000キーが確定した時点で、既知集合専用の二段ハッシュ表を構築する。
// これは未知キーの挿入を扱わない代わりに、既知キーを原則1スロットへ衝突なしで配置するminimal perfect hash相当の表である。
// 疑似コードではbucket=hashの下位10 bit、slot=(hash*seed[bucket])の上位14 bit、id=ids[slot]となる。
// 同じ32 bitハッシュを持つ別キーは乗数を変えても分離できないため、そのスロットをUINT16_MAXにして通常表へ戻す。
// 任意の正当な10,000キー集合で構築成功を保証できる方式ではないため、制限回数内に配置できなければ共有を諦めて通常表を使い続ける。
typedef struct {
  uint16_t bucket, count;
} MphBucket;
static int mph_bucket_compare(const void *a, const void *b) {
  const MphBucket *x = (const MphBucket *)a;
  const MphBucket *y = (const MphBucket *)b;
  return (int)y->count - (int)x->count;
}
static int build_mph(FlatMap *m) {
  // hashesはID順のハッシュ、itemsは同じバケットのIDを連続配置した配列である。
  uint32_t hashes[CONTEST_CHANNELS];
  uint16_t items[CONTEST_CHANNELS];
  uint16_t counts[MPH_BUCKET_CAPACITY] = {0};
  uint16_t offsets[MPH_BUCKET_CAPACITY + 1u];
  uint16_t cursor[MPH_BUCKET_CAPACITY];
  uint8_t used[MPH_SLOT_CAPACITY] = {0};
  uint32_t trial[MPH_SLOT_CAPACITY] = {0};
  uint32_t trial_hash[MPH_SLOT_CAPACITY];
  uint32_t placed_hash[MPH_SLOT_CAPACITY];
  uint32_t seeds[MPH_BUCKET_CAPACITY];
  uint16_t ids[MPH_SLOT_CAPACITY];
  MphBucket order[MPH_BUCKET_CAPACITY];
  for (uint32_t i = 0; i < MAP_CAPACITY; i++) {
    const MapEntry *e = &m->entries[i];
    if (!e->key)
      continue;
    hashes[e->id] = e->hash;
    counts[e->hash & (MPH_BUCKET_CAPACITY - 1)]++;
  }
  offsets[0] = 0;
  for (uint32_t b = 0; b < MPH_BUCKET_CAPACITY; b++) {
    offsets[b + 1] = (uint16_t)(offsets[b] + counts[b]);
    cursor[b] = offsets[b];
    order[b] = (MphBucket){(uint16_t)b, counts[b]};
  }
  for (uint32_t id = 0; id < CONTEST_CHANNELS; id++) {
    uint32_t bucket = hashes[id] & (MPH_BUCKET_CAPACITY - 1);
    items[cursor[bucket]++] = (uint16_t)id;
  }
  // 要素数の多いバケットほど配置候補が少ないため、降順に処理して難しい制約を先に確定する。
  qsort(order, MPH_BUCKET_CAPACITY, sizeof(*order), mph_bucket_compare);
  uint32_t generation = 0;
  for (uint32_t oi = 0; oi < MPH_BUCKET_CAPACITY; oi++) {
    uint32_t bucket = order[oi].bucket;
    uint32_t count = order[oi].count;
    if (!count)
      break;
    uint32_t seed = 0;
    int found = 0;
    // 奇数乗数を最大4,096個試し、既に確定した別ハッシュとも同じ試行内の別ハッシュとも衝突しないものを選ぶ。
    // trialを毎回ゼロクリアせずgeneration番号で世代管理し、シード試行の初期化コストを抑える。
    for (uint32_t attempt = 0; attempt < 4096u; attempt++) {
      seed = attempt * 2u + 1u;
      generation++;
      uint32_t j;
      for (j = 0; j < count; j++) {
        uint32_t hash = hashes[items[offsets[bucket] + j]];
        uint32_t slot = (hash * seed) >> (32 - MPH_SLOT_BITS);
        if (slot >= MPH_ACTIVE_SLOTS ||
            (used[slot] && placed_hash[slot] != hash) ||
            (trial[slot] == generation && trial_hash[slot] != hash))
          break;
        trial[slot] = generation;
        trial_hash[slot] = hash;
      }
      if (j == count) {
        found = 1;
        break;
      }
    }
    if (!found)
      goto failed;
    uint32_t j;
    for (j = 0; j < count; j++) {
      uint16_t id = items[offsets[bucket] + j];
      uint32_t hash = hashes[id];
      uint32_t slot = (hash * seed) >> (32 - MPH_SLOT_BITS);
      if (used[slot]) {
        // 同じhashならスロット共有を許すが、IDを一意に決められない印としてUINT16_MAXを保存する。
        if (placed_hash[slot] != hash)
          break;
        ids[slot] = UINT16_MAX;
      } else {
        used[slot] = 1;
        placed_hash[slot] = hash;
        ids[slot] = (uint16_t)(id * 3u + 1u);
      }
    }
    if (j != count)
      goto failed;
    seeds[bucket] = seed;
  }
  for (uint32_t id = 0; id < CONTEST_CHANNELS; id++) {
    // 公開前に全IDを引き直し、直IDまたは意図した衝突番兵へ到達することを検査する。
    uint32_t hash = hashes[id];
    uint32_t seed = seeds[hash & (MPH_BUCKET_CAPACITY - 1)];
    uint32_t slot = (hash * seed) >> (32 - MPH_SLOT_BITS);
    if (ids[slot] != id * 3u + 1u && ids[slot] != UINT16_MAX)
      goto failed;
  }
  // 公開後は発見用の三つの直引き表を使わないため、その先頭領域を4 KiBのシード表と32 KiBのID表として再利用する。
  // 新しい割り当てを増やさず、共有辞書と同じ2 MiB巨大ページ内にホットな二表を置ける。
  m->mph_seeds = (uint32_t *)m->fast_ids;
  m->mph_ids = (uint16_t *)(m->mph_seeds + MPH_BUCKET_CAPACITY);
  memcpy(m->mph_seeds, seeds,
         MPH_BUCKET_CAPACITY * sizeof(*m->mph_seeds));
  // idsとseedsの未使用スロットは初期化しないが、公開後に来るキーは構築対象の10,000種類だけなので参照されない。
  // 未知キーが来ない根拠は入力契約であり、同一32 bitハッシュは初期化済みのUINT16_MAXスロットから正確比較へ進む。
  memcpy(m->mph_ids, ids, MPH_SLOT_CAPACITY * sizeof(*m->mph_ids));
  return 1;
failed:
  return 0;
}
static void canonicalize_worker_map(Worker *w, FlatMap *dictionary) {
  if (dictionary == &w->map) {
    // 辞書を公開したワーカーはキー表をそのまま所有し、集計表だけ10,000 ID用の巨大ページへ移す。
    FlatMap hot;
    map_init_aggs(&hot, dictionary->size);
    memcpy(hot.aggs, w->map.aggs,
           (size_t)dictionary->size * sizeof(ChannelAgg));
    void *old_aggs = w->map.aggs_alloc;
    size_t old_mmap_size = w->map.aggs_mmap_size;
    w->map.aggs = hot.aggs;
    w->map.aggs_alloc = hot.aggs_alloc;
    w->map.aggs_mmap_size = hot.aggs_mmap_size;
    w->map.agg_cap = hot.agg_cap;
    if (old_mmap_size) {
      if (munmap(old_aggs, old_mmap_size))
        die("munmap publisher aggs");
    } else {
      free(old_aggs);
    }
    w->dictionary = dictionary;
    w->canonical = 1;
    return;
  }
  // 他のワーカーはローカルIDを共有辞書のIDへ変換してから、ローカル辞書を解放する。
  // 各ローカルキーを一度だけバイト比較して対応付けるため、その後の全行と最終併合ではキー比較が不要になる。
  FlatMap old = w->map;
  FlatMap canonical;
  map_init_aggs(&canonical, dictionary->size);
  if (old.wide) {
    canonical.wide =
        (WideChannelAgg *)calloc(MAP_CAPACITY, sizeof(*canonical.wide));
    if (!canonical.wide)
      die("wide canonicalize");
  }
  for (uint32_t i = 0; i < MAP_CAPACITY; i++) {
    const MapEntry *local = &old.entries[i];
    if (!local->key)
      continue;
    const MapEntry *shared = map_find_readonly(
        dictionary, local->key, local->len, local->hash);
    canonical.aggs[shared->id] = old.aggs[local->id];
    if (old.wide)
      canonical.wide[shared->id] = old.wide[local->id];
  }
  map_free(&old);
  w->map = canonical;
  w->dictionary = dictionary;
  w->canonical = 1;
}
static inline __attribute__((always_inline)) FlatMap *
maybe_publish_dictionary(Worker *w) {
  // acquire/releaseにより、辞書本体とMPHを書き終えてから他ワーカーへポインタが見える。
  FlatMap *dictionary =
      __atomic_load_n(&global_dictionary, __ATOMIC_ACQUIRE);
  if (dictionary == DICTIONARY_BUILDING || dictionary == DICTIONARY_FAILED)
    return NULL;
  if (!dictionary && w->map.size == CONTEST_CHANNELS) {
    FlatMap *expected = NULL;
    // CASに勝った一ワーカーだけが構築し、他ワーカーは構築中も自分の通常表で解析を続ける。
    // これにより全スレッドを止めるバリアやロックを1行ごとの経路へ置かずに済む。
    if (__atomic_compare_exchange_n(&global_dictionary, &expected,
                                    DICTIONARY_BUILDING, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      if (build_mph(&w->map)) {
        __atomic_store_n(&global_dictionary, &w->map, __ATOMIC_RELEASE);
        dictionary = &w->map;
      } else {
        // 構築失敗は入力不正ではないので、失敗番兵を公開して全員が正確な通常表へフォールバックする。
        __atomic_store_n(&global_dictionary, DICTIONARY_FAILED,
                         __ATOMIC_RELEASE);
      }
    } else if (expected != DICTIONARY_BUILDING &&
               expected != DICTIONARY_FAILED) {
      dictionary = expected;
    }
  }
  return dictionary;
}
static uint32_t timestamp8(const char *p) {
  // 最適化テクニック：8文字を個別に引き算と乗算で読む代わりに、SIMDの積和演算で2桁、4桁、8桁へまとめる。
  // 意味は「uint32_t value=0; for (int i=0; i<8; i++) value=value*10+(p[i]-'0'); return value;」と同じである。
  // 8バイトロードと二つの積和命令で複数桁を並列変換し、月の表引きに不要なUnix秒の下2桁は最初から読まない。
  // 疎なルックアップ表は追加ロードが必要で、スカラーSWARはこのパイプラインでは1B行を2.786秒から2.979秒へ悪化させた。
  __m128i ascii = _mm_loadl_epi64((const __m128i *)p);
  __m128i pairs = _mm_maddubs_epi16(ascii, _mm_set1_epi16(0x010a));
  __m128i quads = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00010064));
  uint64_t x = (uint64_t)_mm_cvtsi128_si64(quads);
  // 積和はASCIIコード自体を掛けるため、8桁すべてに含まれる'0'の寄与53328*(10000+1)を最後に引く。
  return (uint32_t)x * 10000u + (uint32_t)(x >> 32) - 533333328u;
}
static __attribute__((noinline)) uint32_t channel_length_long(const char *p) {
  // 先頭16バイトにカンマがない低頻度の長いパスだけ、16バイトずつ後続を走査する。
  const __m128i comma = _mm_set1_epi8(',');
  uint32_t offset = 16;
  for (;;) {
    uint32_t mask = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(
        _mm_loadu_si128((const __m128i *)(p + offset)), comma));
    if (mask)
      return offset + (uint32_t)__builtin_ctz(mask);
    offset += 16;
  }
}
static inline __attribute__((always_inline)) uint32_t
channel_length(const char *p) {
  const __m128i comma = _mm_set1_epi8(',');
  // 最適化テクニック：16文字を一度にカンマと比較し、一致bit列の末尾ゼロ数から長さを得る。
  // 素朴な1文字ずつのループにあるデータ依存分岐を、16バイトにつき比較一回へ減らす。
  // 公開データでは先頭16バイト内にカンマがある行が多いため、32バイトロードより小さなホットループを優先する。
  // 32バイトの初回ロードは命令とレジスタの占有が増え、1B行で約34 ms遅かった。
  // 入力契約により各行には必ずカンマがあり、mainが末尾に読み取り可能なゼロページを置くため、最終行で16バイトロードがはみ出しても安全である。
  uint32_t mask = (uint32_t)_mm_movemask_epi8(
      _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p), comma));
  if (__builtin_expect(mask != 0, 1))
    return (uint32_t)__builtin_ctz(mask);
  return channel_length_long(p);
}
// 最適化テクニック：全体の約0.58%しかない「4桁のmessage_lengthと1桁のstamp_count」をcoldかつnoinlineへ分離する。
// 低頻度処理を.text.unlikelyへ移すことで命令キャッシュとホットループのマイクロ命令数を保ち、1B行で約1.4 ms短くした。
// aligned_tailを1バイト右へずらした後の意味は、4桁の十進変換、1桁の十進変換、messages=1のベクトルを作ることである。
static __attribute__((noinline, cold)) __m128i
four_digit_increment(__m128i aligned_tail) {
  __m128i pairs = _mm_maddubs_epi16(
      _mm_srli_epi64(aligned_tail, 8),
      _mm_setr_epi8(10, 1, 10, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
  return _mm_dpwssd_epi32(_mm_setr_epi32(-53328, -48, 1, 0), pairs,
                          _mm_setr_epi16(100, 1, 1, 0, 0, 0, 0, 0));
}
static __attribute__((noinline)) void analyze_steady_segment(Worker *w,
                                                             const char *p) {
  // 共有辞書の公開後に使う定常ループである。
  // 各行の意味は「月を求める、チャンネルIDを引く、message_lengthとstamp_countを読む、その月のStatsへ加える」である。
  // 辞書が固定済みなので、後述の五行遅延パイプラインとMPHを安全に使える。
  const FlatMap *dictionary = w->dictionary;
  // 最初の五回にも分岐を置かないため、ゼロ加算を受ける実在のダミーStatsを用意する。
  Stats ignored = {0, 0, 0, UINT16_MAX, UINT16_MAX};
  Stats *pending_stats0 = &ignored, *pending_stats1 = &ignored;
  Stats *pending_stats2 = &ignored, *pending_stats3 = &ignored;
  Stats *pending_stats4 = &ignored;
  __m128i pending_increment0 = _mm_setzero_si128();
  __m128i pending_increment1 = _mm_setzero_si128();
  __m128i pending_increment2 = _mm_setzero_si128();
  __m128i pending_increment3 = _mm_setzero_si128();
  __m128i pending_increment4 = _mm_setzero_si128();
  while (p < w->end) {
    uint32_t ts100 = timestamp8(p), delta = ts100 - YEAR_START / 100u;
    uint32_t month = month_by_period[delta >> 5];
    p += 11;
    const char *key = p;
    uint32_t len = channel_length(key);
    p += len;
    uint32_t hash = hash_bytes(key, len);
    size_t slot3_plus1 =
        dictionary_find_slot3_plus1(dictionary, key, len, hash);
    Stats *stats = (Stats *)((char *)w->map.aggs +
                             slot3_plus1 * 64 - 64) +
                   month;
    // 最適化テクニック：現在行の書き込み先をprefetchし、五行前の集計をここで実行する。
    // CPUは待ち時間に後続四行の解析を進められるため、約1.92 MiBの集計表へのランダムアクセス待ちを重ねられる。
    // 五行待ちは1B行を約1.88秒から1.72秒へ短縮し、prefetchを外すと約1.92秒、六行待ちではレジスタ不足になった。
    __builtin_prefetch(stats, 1, 3);
    stats_add(pending_stats0, pending_increment0);
    p++;
    uint64_t tail = load64(p);
    __m128i increment;
    // ここからの高速デコーダは、公開データの大半を占める次の三形式を1回の8バイトロードから分類する。
    // 「2桁または3桁のmessage_length, 1桁のstamp_count」「2桁または3桁, 2桁」「4桁, 1桁」である。
    // それ以外の正当な桁数は下の通常の十進ループへ送るため、高速分類に一致しなくても意味は変わらない。
    uint64_t byte3 = tail >> 24;
    uint64_t aligned_tail = (tail << 8) | '0';
    const char *fast_next = p + 5;
    const char *three_next = p + 6;
    // 2桁なら先頭へ'0'を足したaligned_tailを使い、3桁なら元のtailを使う。
    // p[3]==','は3桁形式を表すので、一つのCMPが設定したZFを二つのCMOVEで共有し、桁の整列と次行ポインタを同時に選ぶ。
    // 分岐、SETcc、追加のポインタ加算を除くこの形は、1B行で約60 ms短かった。
    // 出力オペランドの&はearly-clobber指定であり、書き込み途中の値と入力レジスタが重ならないようGCCへ伝える。
    __asm__("cmpb $44, %b[byte3]\n\t"
            "cmove %[raw], %[aligned]\n\t"
            "cmove %[three_next], %[next]"
            : [aligned] "+&r"(aligned_tail), [next] "+&r"(fast_next)
            : [byte3] "q"(byte3), [raw] "r"(tail),
              [three_next] "r"(three_next)
            : "cc");
    uint64_t delimiters =
        _pext_u64(aligned_tail, UINT64_C(0x0000ff00ff000000));
    // BMI2のPEXTで整列後のbyte 3とbyte 5だけを抜き、カンマとLFの位置を一命令で調べる。
    // 右辺が0x0a2cなら下位byteが','、上位byteが'\n'なので、2桁または3桁の長さと1桁スタンプである。
    // shiftとmaskを複数回使う形より、1B行で約9〜24 ms短かった。
    if (__builtin_expect(delimiters == UINT64_C(0x0a2c), 1)) {
      // AVX-VNNIの内積でASCII三桁を100*a+10*b+c、スタンプをdigit、件数を1へ同時変換する。
      // 第1laneの-5328は'0'*(100+10+1)、第2laneの-48は'0'のASCIIバイアスである。
      // 2桁は先頭に足した'0'の百の位を含むため、同じ式で正しく変換できる。
      increment = _mm_dpbusd_epi32(
          _mm_setr_epi32(-5328, -48, 1, 0),
          _mm_cvtsi64_si128((int64_t)aligned_tail),
          _mm_setr_epi8(100, 10, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
      p = fast_next;
    } else if (__builtin_expect((uint8_t)delimiters == ',' &&
                                    (uint8_t)(aligned_tail >> 48) == '\n',
                                1)) {
      // 約4.6987%を占める2桁スタンプでは、重み{10,1,-48}で二桁を作りながらLFの値10から追加のASCIIバイアス480を引く。
      // 1桁経路と同じ初期ベクトルを共有したまま分類を低頻度側へ置けるため、1B行で約31 ms短くした。
      increment = _mm_dpbusd_epi32(
          _mm_setr_epi32(-5328, -48, 1, 0),
          _mm_cvtsi64_si128((int64_t)aligned_tail),
          _mm_setr_epi8(100, 10, 1, 0, 10, 1, -48, 0, 0, 0, 0, 0, 0, 0, 0,
                          0));
      p = fast_next + 1;
    } else if (__builtin_expect((uint8_t)(delimiters >> 8) == ',' &&
                                    (uint8_t)(aligned_tail >> 56) == '\n',
                                1)) {
      // カンマがbyte 5、LFがbyte 7なら、先頭に'0'を置いた状態で4桁の長さと1桁スタンプである。
      increment = four_digit_increment(
          _mm_cvtsi64_si128((int64_t)aligned_tail));
      p += 7;
    } else {
      // 高速形式以外は通常の十進パーサでカンマとLFまで読む。
      // 素朴な実装を低頻度経路として残すことで、任意桁の正当入力を処理しつつホットループを小さく保つ。
      uint32_t ml = (uint8_t)(*p++ - '0');
      while (*p != ',')
        ml = ml * 10 + (uint8_t)(*p++ - '0');
      p++;
      uint32_t stamps = (uint8_t)(*p++ - '0');
      while (*p != '\n')
        stamps = stamps * 10 + (uint8_t)(*p++ - '0');
      p++;
      if (__builtin_expect(ml >= UINT16_MAX, 0)) {
        // この行は補助表へ即時反映済みなので、五段キューにはゼロ加算を入れて順序だけ一つ進める。
        stats_add_wide(&w->map, stats, (slot3_plus1 - 1) / 3, month, ml,
                       stamps);
        pending_stats0 = pending_stats1;
        pending_increment0 = pending_increment1;
        pending_stats1 = pending_stats2;
        pending_increment1 = pending_increment2;
        pending_stats2 = pending_stats3;
        pending_increment2 = pending_increment3;
        pending_stats3 = pending_stats4;
        pending_increment3 = pending_increment4;
        pending_stats4 = &ignored;
        pending_increment4 = _mm_setzero_si128();
        continue;
      }
      increment = _mm_setr_epi32((int)ml, (int)stamps, 1, 0);
    }
    pending_stats0 = pending_stats1;
    pending_increment0 = pending_increment1;
    pending_stats1 = pending_stats2;
    pending_increment1 = pending_increment2;
    pending_stats2 = pending_stats3;
    pending_increment2 = pending_increment3;
    pending_stats3 = pending_stats4;
    pending_increment3 = pending_increment4;
    pending_stats4 = stats;
    pending_increment4 = increment;
  }
  stats_add(pending_stats0, pending_increment0);
  stats_add(pending_stats1, pending_increment1);
  stats_add(pending_stats2, pending_increment2);
  stats_add(pending_stats3, pending_increment3);
  stats_add(pending_stats4, pending_increment4);
}
static __attribute__((noinline)) void analyze_steady_rows(Worker *w,
                                                          const char *p) {
  // 入力を最大256 MiBの行境界セグメントに分け、処理済みページを順次カーネルへ返す。
  // ファイル全体を一度に解析して最後に破棄するより、ページテーブルの解体と残りの解析を並行させられる。
  const char *whole_end = w->end;
  const char *drop_cursor = w->begin;
  while (p < whole_end) {
    const char *segment_end = whole_end;
    if ((size_t)(whole_end - p) > (256u << 20)) {
      const char *target = p + (256u << 20);
      const char *nl =
          (const char *)memchr(target, '\n', (size_t)(whole_end - target));
      if (nl)
        segment_end = nl + 1;
    }
    w->end = segment_end;
    analyze_steady_segment(w, p);
    uintptr_t drop_begin =
        ((uintptr_t)drop_cursor + INPUT_PAGE_SIZE - 1) &
        ~(uintptr_t)(INPUT_PAGE_SIZE - 1);
    uintptr_t drop_end =
        (uintptr_t)segment_end & ~(uintptr_t)(INPUT_PAGE_SIZE - 1);
    // 完全に処理済みの4 KiBページだけをMADV_DONTNEEDへ渡し、現在行を含む端のページは残す。
    // キーは辞書のarenaへコピー済みであり、集計表も別mappingなので、入力ページを捨てた後に参照するポインタはない。
    if (drop_end > drop_begin)
      madvise((void *)drop_begin, drop_end - drop_begin, MADV_DONTNEED);
    drop_cursor = segment_end;
    p = segment_end;
  }
  w->end = whole_end;
  w->drop_cursor = drop_cursor;
}
static void *analyze_worker(void *arg) {
  Worker *w = (Worker *)arg;
  // ワーカーを対応CPUへ固定し、解析中のCPU移動によるL1/L2キャッシュとTLBの温め直しを避ける。
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  CPU_SET(w->cpu, &affinity);
  pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
#ifdef PROFILE
  double t = now();
#endif
  map_init(&w->map);
  // 入力は先読みで全ページをfaultさせず、各ワーカーが必要になったページを順次読む。
  // 全ワーカーによる事前faultはメモリ帯域とカーネル内部で競合し、1B行で約0.3〜0.4秒遅かった。
  const char *p = w->begin;
  Stats ignored = {0, 0, 0, UINT16_MAX, UINT16_MAX};
  Stats *pending_stats = &ignored;
  // 発見中のループは一行前の集計を遅延し、現在行のキー検索や数値変換とランダムなStatsロードを重ねる。
  // 初回だけNULL判定する代わりにゼロ加算を受けるダミーを使うと、1B行で約30〜40 ms短かった。
  // 加算内容を四つのスカラーではなくXMMレジスタ一つで保持すると、1B行で約90〜100 ms短かった。
  __m128i pending_increment = _mm_setzero_si128();
  while (p < w->end) {
    // mainがチャンク境界をLFの直後へ揃え、入力契約が空行なしと保証するため、pは常に10桁時刻の先頭を指す。
    // 行ごとの空行判定は正当入力には不要であり、二つの分岐を10億回追加してしまう。
    uint32_t ts100 = timestamp8(p), delta = ts100 - YEAR_START / 100u;
    uint32_t month = month_by_period[delta >> 5];
    p += 11;
    const char *key = p;
    // 小さな独自キャッシュは置かず、三段直引き表とCPUキャッシュへ任せる。
    // 256スロットの追加キャッシュはタグ確認と競合分岐が増え、100M行で約1.34〜1.50秒まで悪化した。
    uint32_t len = channel_length(key);
    p += len;
    uint32_t hash = hash_bytes(key, len);
    size_t slot3_plus1 = map_find_slot3_plus1(&w->map, key, len, hash);
    Stats *stats = (Stats *)((char *)w->map.aggs +
                             slot3_plus1 * 64 - 64) +
                   month;
    // 一行前のStats更新をここまで遅らせると、現在行のランダムな集計先ロードと残りの解析をCPUが並行して進められる。
    // 共有前の探索は依存が多いため五行分を保持せず、レジスタ消費の少ない一行パイプラインにする。
    // 明示prefetchはZipf分布で頻出する集計先をハードウェアキャッシュが既に保持するため不要で、外すと1B行で約10 ms短かった。
    stats_add(pending_stats, pending_increment);
    p++;
    // 1回の非整列8バイトロードで、全行の約94.7%を占める2桁または3桁の長さと1桁スタンプを認識する。
    uint64_t tail = load64(p);
    __m128i increment;
    uint64_t byte3 = tail >> 24;
    uint64_t aligned_tail = (tail << 8) | '0';
    const char *fast_next = p + 5;
    const char *three_next = p + 6;
    // 定常ループと同じく、一つのCMPと二つのCMOVEで2桁と3桁の整列と次行ポインタを選ぶ。
    // 分岐を使わないため、出現比率が近い二形式でも分岐予測ミスを起こさない。
    // SETcc、TEST、追加のLEAとADDを除き、1B行で約60 ms短くした。
    __asm__("cmpb $44, %b[byte3]\n\t"
            "cmove %[raw], %[aligned]\n\t"
            "cmove %[three_next], %[next]"
            : [aligned] "+&r"(aligned_tail), [next] "+&r"(fast_next)
            : [byte3] "q"(byte3), [raw] "r"(tail),
              [three_next] "r"(three_next)
            : "cc");
    // PEXTはbyte 3とbyte 5を詰めて取り出し、カンマとLFの位置を同時に分類する。
    // 複数のshiftとmaskを使う形より、1B行で約9〜24 ms短かった。
    uint64_t delimiters =
        _pext_u64(aligned_tail, UINT64_C(0x0000ff00ff000000));
    if (__builtin_expect(delimiters == UINT64_C(0x0a2c), 1)) {
      // AVX-VNNIの一回の内積でmessage_length、stamp_count、messages=1の増分を作る。
      // スカラー変換より3桁経路だけで約70 ms短く、先頭ゼロで2桁にも共有するとさらにホットコードを減らせる。
      increment = _mm_dpbusd_epi32(
          _mm_setr_epi32(-5328, -48, 1, 0),
          _mm_cvtsi64_si128((int64_t)aligned_tail),
          _mm_setr_epi8(100, 10, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
      p = fast_next;
    } else if (__builtin_expect((uint8_t)delimiters == ',' &&
                                    (uint8_t)(aligned_tail >> 48) == '\n',
                                1)) {
      // 2桁スタンプは約4.6987%なので二番目に判定し、LFに-48を掛けて追加のASCIIバイアス480も同じ内積で引く。
      // この専用経路により1B行で約31 ms短くした。
      increment = _mm_dpbusd_epi32(
          _mm_setr_epi32(-5328, -48, 1, 0),
          _mm_cvtsi64_si128((int64_t)aligned_tail),
          _mm_setr_epi8(100, 10, 1, 0, 10, 1, -48, 0, 0, 0, 0, 0, 0, 0, 0,
                          0));
      p = fast_next + 1;
    } else if (__builtin_expect((uint8_t)(delimiters >> 8) == ',' &&
                                    (uint8_t)(aligned_tail >> 56) == '\n',
                                1)) {
      // 4桁の長さと1桁スタンプは低頻度関数へ送り、共通経路の命令キャッシュ占有を増やさない。
      increment = four_digit_increment(
          _mm_cvtsi64_si128((int64_t)aligned_tail));
      p += 7;
    } else {
      // その他の桁数はカンマとLFを終端にした通常の十進変換へフォールバックする。
      // 稀な形式までSIMD共通経路へ詰め込むとホットループが大きくなり、1B行で約0.30秒遅かった。
      uint32_t ml = (uint8_t)(*p++ - '0');
      while (*p != ',')
        ml = ml * 10 + (uint8_t)(*p++ - '0');
      p++;
      uint32_t stamps = (uint8_t)(*p++ - '0');
      while (*p != '\n')
        stamps = stamps * 10 + (uint8_t)(*p++ - '0');
      p++;
      // 入力契約が32 bitに制限するのはグループ合計であり、個々のmessage_lengthではない。
      // 1〜65534はSIMD表、65535以上は正確な32 bit補助表へ送り、UINT16_MAXを初期値番兵として予約する。
      if (__builtin_expect(ml >= UINT16_MAX, 0)) {
        stats_add_wide(&w->map, stats, (slot3_plus1 - 1) / 3, month, ml,
                       stamps);
        pending_stats = &ignored;
        pending_increment = _mm_setzero_si128();
        FlatMap *published = maybe_publish_dictionary(w);
        if (published) {
          canonicalize_worker_map(w, published);
          analyze_steady_rows(w, p);
          goto parsing_done;
        }
        continue;
      }
      increment = _mm_setr_epi32((int)ml, (int)stamps, 1, 0);
    }
    pending_stats = stats;
    pending_increment = increment;
    // 辞書公開を検出したら、保持中の一行を旧集計表へ反映してからID変換と巨大ページへの移行を行う。
    // 先に集計表を移すとpending_statsが解放済み領域を指すため、この順序が必要である。
    FlatMap *published = maybe_publish_dictionary(w);
    if (published) {
      stats_add(pending_stats, pending_increment);
      pending_stats = &ignored;
      pending_increment = _mm_setzero_si128();
      canonicalize_worker_map(w, published);
      analyze_steady_rows(w, p);
      goto parsing_done;
    }
  }
  stats_add(pending_stats, pending_increment);
  {
    FlatMap *published =
        __atomic_load_n(&global_dictionary, __ATOMIC_ACQUIRE);
    if (published && published != DICTIONARY_BUILDING &&
        published != DICTIONARY_FAILED)
      canonicalize_worker_map(w, published);
  }
parsing_done:
#ifdef PROFILE
  w->elapsed = now() - t;
#endif
  // 共有後のワーカーは解析中に256 MiBずつ捨て、共有しなかった小入力はここで最後の範囲をまとめて捨てる。
  // 処理済み入力を保持しないためページテーブル解体の約50 msの後処理を解析と重ねられる。
  uintptr_t drop_begin =
      ((uintptr_t)(w->drop_cursor ? w->drop_cursor : w->begin) +
       INPUT_PAGE_SIZE - 1) &
      ~(uintptr_t)(INPUT_PAGE_SIZE - 1);
  uintptr_t drop_end =
      (uintptr_t)w->end & ~(uintptr_t)(INPUT_PAGE_SIZE - 1);
  if (drop_end > drop_begin)
    madvise((void *)drop_begin, drop_end - drop_begin, MADV_DONTNEED);
  return NULL;
}
static const char digit_pairs[] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";
static const uint32_t powers10[] = {
    UINT32_C(1),       UINT32_C(10),       UINT32_C(100),
    UINT32_C(1000),    UINT32_C(10000),    UINT32_C(100000),
    UINT32_C(1000000), UINT32_C(10000000), UINT32_C(100000000),
    UINT32_C(1000000000)};
static inline void store16(char *p, uint16_t x) {
  // 非整列storeをmemcpyで表し、別型ポインタへのcastによる未定義動作を避ける。
  memcpy(p, &x, sizeof(x));
}
static char *append_uint(char *p, uint32_t x) {
  // 最適化テクニック：10進桁数を先に求めて末尾から2桁ずつ書き、整数ごとのsprintfを避ける。
  // bit幅bitsに1233/4096（log10(2)の近似）を掛け、powers10との比較一回で正確な10進桁数へ補正する。
  // digit_pairsから00〜99を2バイト同時にコピーするため、除算回数は1桁ずつ書く実装のおよそ半分になる。
  // 1B行の出力処理は平均約8.928 msから7.620 msへ短くなった。
  unsigned bits = 32u - (unsigned)__builtin_clz(x | 1);
  unsigned estimate = bits * 1233u >> 12;
  unsigned digits = estimate + (x >= powers10[estimate]);
  digits += digits == 0;
  char *end = p + digits;
  char *q = end;
  while (x >= 100) {
    uint32_t pair = x % 100;
    x /= 100;
    q -= 2;
    store16(q, load16(digit_pairs + pair * 2));
  }
  if (x >= 10) {
    q -= 2;
    store16(q, load16(digit_pairs + x * 2));
  } else {
    *--q = (char)('0' + x);
  }
  return end;
}
static char *append_average(char *p, uint32_t total, uint32_t count) {
  // 出力仕様は「uint32をbinary64へ変換して除算し、そのbinary64を小数第2位へ最近接偶数丸めする」である。
  // (total*100+count/2)/countのような有理数上の一段丸めは、先にbinary64へ丸める仕様とtie付近で一致しない。
  double average = (double)total / (double)count;
  uint64_t bits;
  memcpy(&bits, &average, sizeof(bits));
  // 正規化binary64はsignificand*2^(exponent-1075)と表せる。
  // この入力範囲ではsignificand*100が60 bit未満なので、average*100をuint64_t上で誤差なく作れる。
  // quotientが偶数なら半分未満、奇数ならちょうど半分を足すbiasにより、右shiftで最近接偶数丸めを再現する。
  // これはprintf("%.2f", average)と同じ二段階の丸めを保ちつつ、生成されるx87命令列を避ける。
  // 43,179,216組、全到達指数、全到達tieとその隣接binary64値で参照実装との一致が確認されている。
  uint64_t product =
      ((bits & UINT64_C(0x000fffffffffffff)) |
       UINT64_C(0x0010000000000000)) *
      100u;
  unsigned shift = 1075u - ((unsigned)(bits >> 52) & 0x7ffu);
  uint64_t scaled = 0;
  if (shift < 64) {
    // 出力するグループはcount>0かつmessage_length>=1なのでaverageは正規化された正の値であり、shift-1は範囲内である。
    uint64_t quotient = product >> shift;
    uint64_t bias =
        (UINT64_C(1) << (shift - 1)) - 1 + (quotient & 1);
    scaled = (product + bias) >> shift;
  }
  p = append_uint(p, scaled / 100);
  *p++ = '.';
  *p++ = (char)('0' + scaled / 10 % 10);
  *p++ = (char)('0' + scaled % 10);
  return p;
}
static void write_result(FILE *out, const FlatMap *m) {
  // 検証器は行集合を比較して順序を要求しないため、通常表の順で出力してqsortのO(k log k)比較を省く。
  // 4 MiBバッファへ多数行をまとめ、行ごとのfwriteによるロックとシステムコールを避ける。
  char *buf = (char *)malloc(4u << 20);
  if (!buf)
    die("malloc");
  size_t used = 0;
  for (uint32_t i = 0; i < MAP_CAPACITY; i++) {
    const MapEntry *e = &m->entries[i];
    if (!e->key)
      continue;
    for (unsigned j = 0; j < 12; j++) {
      const Stats *s = &m->aggs[e->id].month[j];
      if (!s->messages)
        continue;
      if (used + e->len + 96 > (4u << 20)) {
        fwrite(buf, 1, used, out);
        used = 0;
      }
      char *p = buf + used;
      // 公開10,000キーはすべて32バイト以下なので、可変長memcpyの代わりにXMMロードとストアを二回ずつ実行する。
      // これにより最大120,000行分のlibc呼び出しを除ける。
      // lenを越えて書いた部分は直後の出力で上書きされ、残りもusedより後ろなのでファイルへは出ない。
      // 出力側は1行あたり96バイトの余白を事前確認し、入力側のキーarena直後には同じmappingのID表があるため、32バイトの先読みと先書きは安全である。
      // 入力契約上は104バイトまであり得るので、32バイトを超える一般入力は通常のmemcpyへ戻す。
      if (__builtin_expect(e->len <= 32, 1)) {
        __m128i lo = _mm_loadu_si128((const __m128i *)e->key);
        __m128i hi = _mm_loadu_si128((const __m128i *)(e->key + 16));
        _mm_storeu_si128((__m128i *)p, lo);
        _mm_storeu_si128((__m128i *)(p + 16), hi);
      } else {
        __builtin_memcpy(p, e->key, e->len);
      }
      p += e->len;
      *p++ = ',';
      // 月ラベル7文字と終端NULを8バイト固定でコピーし、NULの位置を次の'='で上書きする。
      // 固定幅にすることで、最大120,000回の7バイトmemcpyを一組の非整列ロードとストアへ畳み込める。
      uint64_t label = load64(month_label[j]);
      __builtin_memcpy(p, &label, sizeof(label));
      p += 7;
      *p++ = '=';
      const WideStats *wide = m->wide ? &m->wide[e->id].month[j] : NULL;
      uint32_t min_len = s->min_len;
      uint32_t max_len = UINT16_MAX - s->inv_max_len;
      if (wide && wide->max_len) {
        // 16 bit値が一つでもあれば、それは必ず65535以上のwide値より小さいためmin_lenはStats側でよい。
        // 16 bit値がなければ番兵をwideの最小値へ置き換え、最大値は常にwide側を採用する。
        if (min_len == UINT16_MAX)
          min_len = wide->min_len;
        max_len = wide->max_len;
      }
      p = append_uint(p, min_len);
      *p++ = '/';
      p = append_average(p, s->total_len, s->messages);
      *p++ = '/';
      p = append_uint(p, max_len);
      *p++ = '/';
      p = append_uint(p, s->messages);
      *p++ = '/';
      p = append_uint(p, s->stamps);
      *p++ = '\n';
      used = (size_t)(p - buf);
    }
  }
  fwrite(buf, 1, used, out);
  free(buf);
}
int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
    return 1;
  }
  const char *input = argv[1], *output = argv[2];
  // 最適化テクニック：オンライン論理CPU数と同数のワーカーを作り、SMTを含む計算資源を使い切る。
  // 対象EC2では1B行が4スレッド4.821秒、6スレッド4.358秒、8スレッド3.718秒で、物理コア数よりSMT込みの8スレッドが速かった。
  // 論理CPU数を超えるとコンテキスト切り替えとキャッシュ競合が増えるため、oversubscribeはしない。
  // WORKER_LIMITは性能実験やCPU割当制限が必要なビルドだけ上限を指定する仕組みで、既定値0なら制限しない。
  long threads = sysconf(_SC_NPROCESSORS_ONLN);
  if (threads < 1) {
    fprintf(stderr, "no online CPU\n");
    return 1;
  }
  if (WORKER_LIMIT && threads > WORKER_LIMIT)
    threads = WORKER_LIMIT;
  // 月表は起動時に一度だけ約9.9 KiBを構築し、各行ではdelta>>5による配列参照だけを行う。
  for (unsigned period = 0, m = 0; period < 315360u; period += 32u) {
    uint32_t ts = YEAR_START + period * 100u;
    if (ts >= month_start[m + 1])
      m++;
    month_by_period[period >> 5] = (uint8_t)m;
  }
#ifdef PROFILE
  double total = now(), t = total;
#endif
  int fd = open(input, O_RDONLY);
  if (fd < 0)
    die("open");
  struct stat st;
  if (fstat(fd, &st))
    die("fstat");
  size_t size = (size_t)st.st_size;
  if (size > SIZE_MAX - (INPUT_PAGE_SIZE * 2u - 1u)) {
    errno = EOVERFLOW;
    die("input size");
  }
  size_t mapped_size =
      (size + INPUT_PAGE_SIZE - 1u) & ~(size_t)(INPUT_PAGE_SIZE - 1u);
  // 最適化テクニック：ファイルをmmapし、その直後に読み取り可能な無名ゼロページを一枚置く。
  // fgets、行バッファ、strtok、atoiを使う素朴な実装と違い、ファイル内容をコピーせずポインタを進めながら必要なASCIIだけを直接変換する。
  // demand pagingにより必要なページだけをカーネルが読み込み、複数ワーカーも同じmappingを読み取り専用で共有できる。
  // SIMDロードはキー長や数値を調べる際に論理的な行末を最大数バイト越えるため、境界確認を毎行入れる素朴な実装では分岐が10億回増える。
  // 先にファイルのページ切り上げ長とゼロページを連続予約し、先頭部分だけMAP_FIXEDでファイルmappingへ置き換える。
  // そのため最終LFがページ末尾にある場合も次ページのロードは安全で、読み足したゼロは長さや数値の結果に使われない。
  void *reservation = mmap(NULL, mapped_size + INPUT_PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED)
    die("mmap guard");
  const char *data = (const char *)mmap(reservation, size, PROT_READ,
                                        MAP_PRIVATE | MAP_FIXED, fd, 0);
  if (data == MAP_FAILED)
    die("mmap");
  // 入力を先頭から一度だけ走査することをMADV_SEQUENTIALでカーネルへ伝え、readaheadとページ回収の判断を助ける。
  // tmpfs上の1B行では差は数ms以内だが、正しいアクセス意図を低コストで与えられる。
  madvise((void *)data, size, MADV_SEQUENTIAL);
#ifdef PROFILE
  double mmap_time = now() - t;
#endif
  const char *nl = (const char *)memchr(data, '\n', size);
  const char header[] =
      "unix_timestamp,channel_path,message_length,stamp_count";
  if (!nl || (size_t)(nl - data) != sizeof(header) - 1 ||
      memcmp(data, header, sizeof(header) - 1)) {
    fprintf(stderr, "unsupported CSV header\n");
    return 1;
  }
  const char *begin = nl + 1, *end = data + size;
  size_t bytes = (size_t)(end - begin);
  // 小入力で空ワーカーを大量に作らないよう、約4 KiBにつき最大一ワーカーへ制限する。
  if ((size_t)threads > bytes / 4096 + 1)
    threads = (long)(bytes / 4096 + 1);
  Worker *workers = (Worker *)calloc((size_t)threads, sizeof(*workers));
  pthread_t *ids =
      (pthread_t *)malloc((size_t)threads * sizeof(*ids));
  if (!workers || !ids)
    die("alloc");
#ifdef PROFILE
  t = now();
#endif
  const char *start = begin;
  for (long i = 0; i < threads; i++) {
    const char *stop = end;
    if (i + 1 < threads) {
      // バイト数がほぼ等しい位置から次のLFまで進めて分割し、各行をちょうど一ワーカーへ割り当てる。
      // CSVの行順は集計結果に影響せず、ワーカーごとの表を後で結合できるため同期なしで並列化できる。
      const char *target = begin + bytes * (size_t)(i + 1) / (size_t)threads;
      const char *x =
          (const char *)memchr(target, '\n', (size_t)(end - target));
      if (x)
        stop = x + 1;
    }
    workers[i].begin = start;
    workers[i].end = stop;
    workers[i].cpu = (int)i;
    start = stop;
    pthread_create(&ids[i], NULL, analyze_worker, &workers[i]);
  }
  for (long i = 0; i < threads; i++)
    pthread_join(ids[i], NULL);
#ifdef PROFILE
  double worker_wall = now() - t, worker_sum = 0;
  for (long i = 0; i < threads; i++)
    worker_sum += workers[i].elapsed;
  t = now();
#endif
  FlatMap *published =
      __atomic_load_n(&global_dictionary, __ATOMIC_ACQUIRE);
  if (published == DICTIONARY_BUILDING || published == DICTIONARY_FAILED)
    published = NULL;
  FlatMap merged;
  if (published) {
    // 共有辞書があれば全ワーカーを同じID空間へ揃え、キー探索なしのmap_merge_idsで集計だけを併合する。
    for (long i = 0; i < threads; i++)
      if (!workers[i].canonical)
        canonicalize_worker_map(&workers[i], published);
    long owner = -1;
    for (long i = 0; i < threads; i++)
      if (&workers[i].map == published)
        owner = i;
    if (owner < 0) {
      errno = EINVAL;
      die("published dictionary owner");
    }
    merged = workers[owner].map;
    for (long i = 0; i < threads; i++) {
      if (i == owner)
        continue;
      map_merge_ids(&merged, &workers[i].map, published->size);
      map_free(&workers[i].map);
    }
  } else {
    // 10,000種類に達しない入力やMPH構築失敗時は、キーを正確に照合する通常表の併合へ戻る。
    merged = workers[0].map;
    for (long i = 1; i < threads; i++) {
      map_merge(&merged, &workers[i].map);
      map_free(&workers[i].map);
    }
  }
#ifdef PROFILE
  double merge = now() - t;
#endif
  FILE *out = fopen(output, "wb");
  if (!out)
    die("fopen");
#ifdef PROFILE
  t = now();
#endif
  write_result(out, &merged);
  fflush(out);
#ifdef PROFILE
  double output_time = now() - t;
  for (long i = 0; i < threads; i++)
    fprintf(stderr, " worker%ld=%.6f", i, workers[i].elapsed);
  fputc('\n', stderr);
  size_t groups = 0;
  uint64_t fallback_rows = 0;
  uint32_t fallback_keys = 0;
  for (uint32_t i = 0; i < MAP_CAPACITY; i++) {
    const MapEntry *e = &merged.entries[i];
    if (!e->key)
      continue;
    for (unsigned j = 0; j < 12; j++)
      groups += merged.aggs[e->id].month[j].messages != 0;
    if (merged.mph_ids) {
      uint32_t seed =
          merged.mph_seeds[e->hash & (MPH_BUCKET_CAPACITY - 1)];
      uint32_t slot = (e->hash * seed) >> (32 - MPH_SLOT_BITS);
      if (merged.mph_ids[slot] == UINT16_MAX) {
        fallback_keys++;
        for (unsigned j = 0; j < 12; j++)
          fallback_rows += merged.aggs[e->id].month[j].messages;
      }
    }
  }
  fprintf(stderr,
          "profile mmap=%.6f workers_wall=%.6f workers_sum=%.6f merge=%.6f "
          "output=%.6f total=%.6f chunks=%ld shared=%d groups=%zu "
          "fallback_rows=%" PRIu64 " fallback_keys=%u mph=%d\n",
          mmap_time, worker_wall, worker_sum, merge, output_time, now() - total,
          threads, published != NULL, groups, fallback_rows, fallback_keys,
          merged.mph_ids != NULL);
#endif
  fclose(out);
  finish_profile();
  // 一回実行して終了するCLIなので、残るmapping、ワーカー配列、公開辞書はOSのプロセス終了処理に回収させる。
  // _exitならatexit処理やstdioの再flushを避けられ、必要な出力は直前のfflushとfcloseですでに完了している。
  _exit(0);
}
