#pragma GCC optimize("unroll-loops")

#define _GNU_SOURCE

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define OUT_FILE_SIZE (10 * 1024 * 1024ul)
#define MAX_THREADS 8

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

static inline void write_2digits(char* p, unsigned int val) {
  *(uint16_t*)p = *(const uint16_t*)&digit_pairs[val * 2];
}

static inline uint32_t fast_digit_count(uint32_t x) {
  static uint64_t table[] = {
      4294967296,  8589934582,  8589934582,  8589934582,  12884901788,
      12884901788, 12884901788, 17179868184, 17179868184, 17179868184,
      21474826480, 21474826480, 21474826480, 21474826480, 25769703776,
      25769703776, 25769703776, 30063771072, 30063771072, 30063771072,
      34349738368, 34349738368, 34349738368, 34349738368, 38554705664,
      38554705664, 38554705664, 41949672960, 41949672960, 41949672960,
      42949672960, 42949672960};

  int log2 = 31 - __builtin_clz(x | 1);

  return (x + table[log2]) >> 32;
}

static inline unsigned write_num(char* buffer, unsigned num) {
  if (num == 0) {
    buffer[0] = '0';
    return 1;
  }

  unsigned len = fast_digit_count(num);

  unsigned i = len;
  while (num >= 100) {
    i -= 2;
    write_2digits(&buffer[i], num % 100);
    num /= 100;
  }

  if (num >= 10) {
    i -= 2;
    write_2digits(&buffer[i], num);
  } else {
    buffer[i - 1] = '0' + num;
  }

  return len;
}

#define HASH_MAP_SIZE (1 << 15)
#define HASH_MAP_MASK (HASH_MAP_SIZE - 1)

struct MonthData {
  uint32_t min;
  uint32_t max;
  uint32_t count;
  uint32_t sum;
  uint32_t sum2;
};

struct HashEntry {
  size_t hash;
  const char* path;
  unsigned len;
  struct MonthData months[12];
};

struct HashMap {
  struct HashEntry entries[HASH_MAP_SIZE];
  int size;
};

static inline size_t hash(const char* str, unsigned int len) {
  size_t hash = 0x517cc1b727220a95;
  unsigned int i = 0;

  for (; i + 8 <= len; i += 8) {
    size_t chunk = *(const size_t*)(str + i);
    hash = (hash ^ chunk) * 0x517cc1b727220a95;
  }

  for (; i < len; i++) {
    hash = (hash ^ (unsigned char)str[i]) * 0x517cc1b727220a95;
  }

  return hash;
}

static inline struct HashEntry* hashmap_get_or_insert(struct HashMap* map,
                                                      const char* path,
                                                      unsigned int len,
                                                      size_t hash) {
  unsigned int idx = hash & HASH_MAP_MASK;

  while (1) {
    if (map->entries[idx].path == NULL) {
      map->entries[idx].hash = hash;
      map->entries[idx].path = path;
      map->entries[idx].len = len;
      map->size++;
      for (int m = 0; m < 12; m++) {
        map->entries[idx].months[m].min = UINT32_MAX;
      }
      return &map->entries[idx];
    }

    if (map->entries[idx].hash == hash && map->entries[idx].len == len &&
        memcmp(map->entries[idx].path, path, len) == 0) {
      return &map->entries[idx];
    }

    idx = (idx + 1) & HASH_MAP_MASK;
  }
}

static inline void merge_data(struct MonthData* target,
                              const struct MonthData* source) {
  if (source->min < target->min) {
    target->min = source->min;
  }
  if (source->max > target->max) {
    target->max = source->max;
  }
  target->count += source->count;
  target->sum += source->sum;
  target->sum2 += source->sum2;
}

static const unsigned char month_table[365] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  5,
    5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11};

static inline int get_month(uint32_t unix_time) {
  uint32_t yday = (unix_time - 1798761600u) / 86400u;
  return month_table[yday];
}

struct thread_args {
  int id;
};

size_t map_size;
char* data;

struct HashMap hashmap[MAX_THREADS] = {};

static void* thread_func(void* arg) {
  struct thread_args* args = arg;
  const int id = args->id;

  size_t i = map_size * id / MAX_THREADS;
  const size_t end = map_size * (id + 1) / MAX_THREADS;

  if (id == 0 || data[i - 1] != '\n') {
    while (data[i] != '\n') {
      i++;
    }
    i++;
  }

  while (i < end) {
    uint32_t unix_time =
        (data[i] & 0xF) * 1000000000u + (data[i + 1] & 0xF) * 100000000u +
        (data[i + 2] & 0xF) * 10000000u + (data[i + 3] & 0xF) * 1000000u +
        (data[i + 4] & 0xF) * 100000u + (data[i + 5] & 0xF) * 10000u +
        (data[i + 6] & 0xF) * 1000u + (data[i + 7] & 0xF) * 100u +
        (data[i + 8] & 0xF) * 10u + (data[i + 9] & 0xF);
    i += 11;

    int month = get_month(unix_time);

    const char* path = &data[i];
    char* path_end = rawmemchr(path, ',');
    unsigned size = path_end - path;
    i += size + 1;

    size_t h = hash(path, size);
    struct HashEntry* entry =
        hashmap_get_or_insert(&hashmap[id], path, size, h);

    uint32_t num1 = 0;
    do {
      num1 = num1 * 10 + (data[i] & 0x0F);
      i++;
    } while (data[i] != ',');
    i++;

    uint32_t num2 = 0;
    do {
      num2 = num2 * 10 + (data[i] & 0x0F);
      i++;
    } while (data[i] != '\n');
    i++;

    struct MonthData* target = &entry->months[month];

    if (num1 < target->min) target->min = num1;
    if (num1 > target->max) target->max = num1;
    target->count += 1;
    target->sum += num1;
    target->sum2 += num2;
  }

  return NULL;
}

struct merge_args {
  int from;
  int to;
};

static void* merge_thread_func(void* arg) {
  struct merge_args* args = arg;
  int from = args->from;
  int to = args->to;

  for (int i = 0; i < HASH_MAP_SIZE; i++) {
    if (hashmap[from].entries[i].path != NULL) {
      struct HashEntry* entry_src = &hashmap[from].entries[i];
      struct HashEntry* entry_dest = hashmap_get_or_insert(
          &hashmap[to], entry_src->path, entry_src->len, entry_src->hash);

      for (int m = 0; m < 12; m++) {
        merge_data(&entry_dest->months[m], &entry_src->months[m]);
      }
    }
  }

  return NULL;
}

int main(int argc, char* argv[]) {
  int infd = open(argv[1], O_RDONLY);

  struct stat st;
  if (fstat(infd, &st) < 0) {
    perror("fstat");
    return 1;
  }
  size_t file_size = st.st_size;

  map_size = file_size;
  // int page_size = getpagesize();
  // map_size = (file_size + page_size - 1) & ~(page_size - 1);

  data = mmap(NULL, map_size, PROT_READ, MAP_SHARED, infd, 0);

  if (data == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  madvise(data, map_size, MADV_SEQUENTIAL);

  pthread_t threads[MAX_THREADS];
  struct thread_args args[MAX_THREADS];

  for (int id = 0; id < MAX_THREADS; id++) {
    args[id].id = id;
    pthread_create(&threads[id], NULL, thread_func, &args[id]);
  }

  for (int id = 0; id < MAX_THREADS; id++) {
    pthread_join(threads[id], NULL);
  }

  for (int t = 1; (1 << t) <= MAX_THREADS; t++) {
    struct merge_args merge_args[MAX_THREADS];
    pthread_t merge_threads[MAX_THREADS];

    for (int i = 0; i < MAX_THREADS; i += (1 << t)) {
      merge_args[i].to = i;
      merge_args[i].from = i + (1 << (t - 1));
      pthread_create(&merge_threads[i], NULL, merge_thread_func,
                     &merge_args[i]);
    }

    for (int i = 0; i < MAX_THREADS; i += (1 << t)) {
      pthread_join(merge_threads[i], NULL);
    }
  }

  int outfd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
  ftruncate(outfd, OUT_FILE_SIZE);

  char* out_data =
      mmap(NULL, OUT_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, outfd, 0);

  if (out_data == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  madvise(out_data, OUT_FILE_SIZE, MADV_SEQUENTIAL);

  size_t pos = 0;

  for (int i = 0; i < HASH_MAP_SIZE; i++) {
    if (hashmap[0].entries[i].path != NULL) {
      struct HashEntry* entry = &hashmap[0].entries[i];
      for (int m = 0; m < 12; m++) {
        struct MonthData merged_data = entry->months[m];

        if (merged_data.count > 0) {
          memcpy(&out_data[pos], entry->path, entry->len);
          pos += entry->len;
          strcpy(&out_data[pos], ",2027-");
          pos += 6;
          write_2digits(&out_data[pos], (m + 1));
          pos += 2;
          out_data[pos++] = '=';
          pos += write_num(&out_data[pos], merged_data.min);
          out_data[pos++] = '/';

          double tmp = (double)merged_data.sum / merged_data.count;
          size_t rounded = tmp * 100;
          double frac = fma(tmp, 100.0, -(double)rounded);
          if (frac >= 0.5) {
            rounded++;
          }
          if (frac == 0.5) {
            rounded &= ~1;
          }
          pos += write_num(&out_data[pos], rounded / 100);
          out_data[pos++] = '.';
          write_2digits(&out_data[pos], rounded % 100);
          pos += 2;
          out_data[pos++] = '/';
          pos += write_num(&out_data[pos], merged_data.max);
          out_data[pos++] = '/';
          pos += write_num(&out_data[pos], merged_data.count);
          out_data[pos++] = '/';
          pos += write_num(&out_data[pos], merged_data.sum2);
          out_data[pos++] = '\n';
        }
      }
    }
  }

  ftruncate(outfd, pos);

  munmap(out_data, OUT_FILE_SIZE);
  return 0;
}