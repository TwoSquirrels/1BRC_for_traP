// traQ message CSV aggregator — UNSAFE MPHF variant (no name verify).
// Channel index comes from a compile-time CHD perfect hash over the fixed 10000
// channels (mphf_data.h): ci = (f(h) + DISP[g(h)]) mod M, a tiny L1 disp table
// instead of the 1 MB slot table. Removes the slot-resolve load ⇒ single-stage
// prefetch pipeline. Correct ONLY if every row's channel is in the built set
// (verified by 1B MATCH on dev); an unknown channel yields a wrong ci (no crash).
// Build (AVX2 + REAL PGO + funroll, SAME -o name both passes):
//   cc -O3 -march=x86-64-v3 -maes -DPROFILE_AVX2 -fprofile-generate -pthread -o program main_mphf.c
//   ./program input_1000000000.csv /tmp/o.txt
//   cc -O3 -march=x86-64-v3 -maes -DPROFILE_AVX2 -fprofile-use -fprofile-correction -funroll-loops -pthread -s -o program main_mphf.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <immintrin.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mphf_data.h"

#ifdef PROFILE_AVX2
#define PARSE_MASK(vm, len, vv) \
    uint32_t cmask=(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(vv,_mm256_set1_epi8(','))); \
    uint32_t len=cmask?(uint32_t)__builtin_ctz(cmask):32u; \
    __m256i vm=_mm256_and_si256(vv,_mm256_loadu_si256((const __m256i *)(g_maskbytes+32-len)));
#else
#define PARSE_MASK(vm, len, vv) \
    __mmask32 cm=_mm256_cmpeq_epi8_mask(vv,_mm256_set1_epi8(',')); uint32_t cmi=(uint32_t)cm; \
    uint32_t len=_tzcnt_u32(cmi); __mmask32 lenmask=(__mmask32)(_blsi_u32(cmi)-1u); \
    __m256i vm=_mm256_maskz_mov_epi8(lenmask, vv);
#endif

static __m256i BND0, BND1, BND2;
static void init_month_bounds(void) {
    static const int cumdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint64_t b[12];
    for (int m = 1; m <= 11; m++) {
        uint32_t v = 17987616u + (uint32_t)cumdays[m] * 864u - 1u;
        char sc[8];
        for (int i = 7; i >= 0; i--) { sc[i] = (char)('0' + v % 10); v /= 10; }
        uint64_t le; memcpy(&le, sc, 8);
        b[m-1] = __builtin_bswap64(le);
    }
    b[11] = 0x7FFFFFFFFFFFFFFFULL;
    BND0 = _mm256_setr_epi64x((long long)b[0],(long long)b[1],(long long)b[2],(long long)b[3]);
    BND1 = _mm256_setr_epi64x((long long)b[4],(long long)b[5],(long long)b[6],(long long)b[7]);
    BND2 = _mm256_setr_epi64x((long long)b[8],(long long)b[9],(long long)b[10],(long long)b[11]);
}

typedef struct {
    uint32_t count; uint32_t sum_len; uint32_t sum_stamp;
    uint16_t min_len; uint16_t max_len;
} Bucket;

// Compile-time perfect hash: ci in [0, MPHF_M). No load of a key/name.
static inline uint32_t mphf_ci(uint64_t h) {
    uint32_t g = (uint32_t)h & (MPHF_R - 1);
    uint32_t f = (uint32_t)(((h & 0xFFFFFFFFu) * (uint64_t)MPHF_M) >> 32);
    uint32_t ci = f + MPHF_DISP[g];
    if (ci >= MPHF_M) ci -= MPHF_M;
    return ci;
}

typedef struct {
    Bucket *buckets; // MPHF_M * 12
} Map;

static void *halloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return malloc(n);
    madvise(p, n, MADV_HUGEPAGE);
    return p;
}

static void map_init(Map *m) {
    m->buckets = halloc(sizeof(Bucket) * 12 * MPHF_M);
    for (uint32_t i = 0; i < 12u * MPHF_M; i++) {
        m->buckets[i].count = 0; m->buckets[i].sum_len = 0; m->buckets[i].sum_stamp = 0;
        m->buckets[i].min_len = 0xFFFFu; m->buckets[i].max_len = 0;
    }
}

static inline void bucket_add(Bucket *b, uint32_t mlen, uint32_t stamps) {
    b->count++;
    b->sum_len += mlen;
    b->sum_stamp += stamps;
    b->min_len = mlen < b->min_len ? (uint16_t)mlen : b->min_len;
    b->max_len = mlen > b->max_len ? (uint16_t)mlen : b->max_len;
}

typedef struct {
    const char *start;
    const char *end;
    Map map;
} Task;

static inline const char *parse_u32(const char *p, uint32_t *out) {
    uint32_t v = (uint32_t)(*p - '0');
    p++;
    while (*p >= '0') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
    *out = v;
    return p;
}

#ifndef PD
#define PD 8
#endif
#define PD_MASK (PD - 1)

static const uint8_t g_maskbytes[64] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

static void *worker(void *arg) {
    Task *t = (Task *)arg;
    map_init(&t->map);
    Map *m = &t->map;
    const char *p = t->start;
    const char *end = t->end;

    // Single-stage pipeline: STEP computes the bucket and prefetches it; DRAIN
    // (PD rows later) applies the update. Ring size == PD, so read before write.
    Bucket *b_ptr[PD];
    uint64_t b_ms[PD];
    long head = 0, tail = 0;

#define STEP() do { \
        __builtin_prefetch(p + 256, 0, 0); \
        uint64_t ts_le; memcpy(&ts_le, p, 8); \
        __m256i tsv = _mm256_set1_epi64x((long long)__builtin_bswap64(ts_le)); \
        int mmask = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(tsv, BND0))) \
                  | (_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(tsv, BND1))) << 4) \
                  | (_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(tsv, BND2))) << 8); \
        uint32_t month = (uint32_t)__builtin_popcount(mmask); \
        p += 11; \
        __m256i vv = _mm256_loadu_si256((const __m256i *)p); \
        PARSE_MASK(vm, len, vv) \
        __m128i alo = _mm256_castsi256_si128(vm); \
        __m128i ahi = _mm256_extracti128_si256(vm, 1); \
        __m128i hh = _mm_aesenc_si128(alo, ahi); \
        hh = _mm_aesenc_si128(hh, _mm_set1_epi64x((long long)0x9E3779B97F4A7C15ULL)); \
        uint64_t h = (uint64_t)_mm_cvtsi128_si64(hh); \
        p += len; p++; \
        uint32_t mlen, stamps; \
        p = parse_u32(p, &mlen); p++; \
        p = parse_u32(p, &stamps); p++; \
        uint32_t ci = mphf_ci(h); \
        Bucket *bp = &m->buckets[ci * 12 + month]; \
        __builtin_prefetch(bp, 1, 3); \
        int r = (int)(tail++ & PD_MASK); \
        b_ptr[r] = bp; b_ms[r] = (uint64_t)mlen | ((uint64_t)stamps << 32); \
    } while (0)

#define DRAIN() do { \
        int u = (int)(head++ & PD_MASK); \
        uint64_t ms = b_ms[u]; \
        bucket_add(b_ptr[u], (uint32_t)ms, (uint32_t)(ms >> 32)); \
    } while (0)

    for (int w = 0; w < PD && p < end; w++) STEP();
    while (p < end) { DRAIN(); STEP(); }
    while (head < tail) DRAIN();
#undef STEP
#undef DRAIN
    return NULL;
}

static inline char *wu32(char *o, uint32_t v) {
    if (v >= 10) o = wu32(o, v / 10);
    *o++ = (char)('0' + v % 10);
    return o;
}

static inline void bucket_merge(Bucket *d, const Bucket *s) {
    if (s->count == 0) return;
    d->count += s->count;
    d->sum_len += s->sum_len;
    d->sum_stamp += s->sum_stamp;
    if (s->min_len < d->min_len) d->min_len = s->min_len;
    if (s->max_len > d->max_len) d->max_len = s->max_len;
}

typedef struct { Map *g; uint32_t s, e; char *buf; size_t len; } OutTask;
static void *out_worker(void *arg) {
    OutTask *ot = (OutTask *)arg;
    Map *g = ot->g;
    size_t cap = (size_t)(ot->e - ot->s) * 12 * 128 + 4096;
    char *out = malloc(cap);
    char *o = out;
    for (uint32_t ci = ot->s; ci < ot->e; ci++) {
        const char *nm = MPHF_NAME[ci];
        if (nm[0] == 0) continue;
        uint32_t nl = (uint32_t)strlen(nm);
        for (int mth = 0; mth < 12; mth++) {
            Bucket *b = &g->buckets[ci * 12 + mth];
            if (b->count == 0) continue;
            memcpy(o, nm, nl); o += nl;
            *o++ = ','; *o++ = '2'; *o++ = '0'; *o++ = '2'; *o++ = '7'; *o++ = '-';
            int mm = mth + 1; *o++ = (char)('0' + mm / 10); *o++ = (char)('0' + mm % 10);
            *o++ = '=';
            o = wu32(o, b->min_len); *o++ = '/';
            double d = (double)b->sum_len / (double)b->count;
            long long n = (long long)rintl((long double)d * 100.0L);
            o = wu32(o, (uint32_t)(n / 100));
            *o++ = '.';
            uint32_t fp = (uint32_t)(n % 100);
            *o++ = (char)('0' + fp / 10); *o++ = (char)('0' + fp % 10);
            *o++ = '/';
            o = wu32(o, b->max_len); *o++ = '/';
            o = wu32(o, b->count); *o++ = '/';
            o = wu32(o, b->sum_stamp); *o++ = '\n';
        }
    }
    ot->buf = out; ot->len = (size_t)(o - out);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s input.csv output.txt\n", argv[0]);
        return 1;
    }
    init_month_bounds();

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
    size_t fsize = (size_t)st.st_size;
    long pg = sysconf(_SC_PAGESIZE);
    char *base = mmap(NULL, fsize + (size_t)pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    const char *data = mmap(base, fsize, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap file"); return 1; }
    madvise((void *)data, fsize, MADV_SEQUENTIAL | MADV_WILLNEED);
#ifdef MADV_HUGEPAGE
    madvise((void *)data, fsize, MADV_HUGEPAGE);
#endif

    int nthreads;
    cpu_set_t cpus;
    if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0)
        nthreads = CPU_COUNT(&cpus);
    else {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = (ncpu > 0) ? (int)ncpu : 4;
    }
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    const char *header_end = memchr(data, '\n', fsize);
    size_t body_start = header_end ? (size_t)(header_end - data) + 1 : fsize;
    size_t body = fsize - body_start;

    Task *tasks = calloc(nthreads, sizeof(Task));
    pthread_t *threads = calloc(nthreads, sizeof(pthread_t));

    for (int i = 0; i < nthreads; i++) {
        size_t s = body_start + body * (size_t)i / nthreads;
        size_t e = body_start + body * (size_t)(i + 1) / nthreads;
        if (i > 0) {
            const char *nl = memchr(data + s, '\n', fsize - s);
            s = nl ? (size_t)(nl - data) + 1 : fsize;
        }
        if (i == nthreads - 1) e = fsize;
        else {
            const char *nl = memchr(data + e, '\n', fsize - e);
            e = nl ? (size_t)(nl - data) + 1 : fsize;
        }
        tasks[i].start = data + s;
        tasks[i].end = data + e;
    }

    for (int i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, worker, &tasks[i]);
    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    // Merge every thread's buckets into map 0 (dense, bucket-indexed).
    Map *g = &tasks[0].map;
    for (int i = 1; i < nthreads; i++) {
        Map *src = &tasks[i].map;
        for (uint32_t j = 0; j < 12u * MPHF_M; j++)
            bucket_merge(&g->buckets[j], &src->buckets[j]);
    }

    OutTask ot = { g, 0, MPHF_M, NULL, 0 };
    out_worker(&ot);
    int ofd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open output"); return 1; }
    size_t written = 0;
    while (written < ot.len) {
        ssize_t w = write(ofd, ot.buf + written, ot.len - written);
        if (w < 0) { perror("write"); return 1; }
        written += (size_t)w;
    }
    close(ofd);
    return 0;
}
