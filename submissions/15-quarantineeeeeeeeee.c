#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define PATH_SIZE 128
#define LENGTH_SIZE 20
#define COUNT_SIZE 20
#define FULL_ADDR_SIZE 170
#define YEAR_START 1798761600u

#define BUF_SIZE 255
#define TIMESTAMP_SIZE 20
#define HASHTABLE_SIZE 32768
#define READ_BUF_SIZE (1024 * 1024)

typedef struct _zipnode {
  uint32_t min;
  uint32_t sum;
  uint32_t max;
  uint32_t count;
  uint32_t stamp;
} zipnode_t;

typedef struct channel_st {
  size_t path_length;
  char path[PATH_SIZE];
  zipnode_t table[12];
} channel_t;

typedef struct {
  uint64_t hash;
  channel_t* channel;
} hash_slot_t;

static const uint8_t month_by_day[365] = {
    [0 ... 30] = 0,    [31 ... 58] = 1,    [59 ... 89] = 2,
    [90 ... 119] = 3,  [120 ... 150] = 4,  [151 ... 180] = 5,
    [181 ... 211] = 6, [212 ... 242] = 7,  [243 ... 272] = 8,
    [273 ... 303] = 9, [304 ... 333] = 10, [334 ... 364] = 11};

static inline uint32_t get_month(uint32_t timestamp) {
  uint32_t day = (timestamp - YEAR_START) / 86400;
  return month_by_day[day];
}

size_t search_channel(hash_slot_t hash_table[], uint64_t path_hash,
                      const char* path_start, size_t path_length) {
  uint64_t index = path_hash & (HASHTABLE_SIZE - 1);
  hash_slot_t slot = hash_table[index];
  while (slot.channel != NULL) {
    if (slot.hash == path_hash && slot.channel->path_length == path_length &&
        memcmp(slot.channel->path, path_start, path_length) == 0) {
      return index;
    }
    index = (index + 1) % HASHTABLE_SIZE;
    slot = hash_table[index];
  }
  return index;
}

int add_message(hash_slot_t hash_table[], uint32_t timestamp,
                uint64_t path_hash, const char* path_start, size_t path_length,
                uint32_t length, uint32_t stamp_count) {
  size_t index = search_channel(hash_table, path_hash, path_start, path_length);
  channel_t* node = hash_table[index].channel;
  if (node == NULL) {
    node = calloc(1, sizeof(channel_t));
    if (node == NULL) {
      return 1;
    }
    if (path_length >= sizeof(node->path)) {
      free(node);
      return 1;
    }
    node->path_length = path_length;
    hash_slot_t* slot = &hash_table[index];
    slot->hash = path_hash;
    slot->channel = node;
    memcpy(node->path, path_start, path_length);
    node->path[path_length] = '\0';
  }
  int ym = get_month(timestamp);
  if (node->table[ym].count == 0) {
    zipnode_t* zipnode = &node->table[ym];
    if (zipnode == NULL) {
      return 1;
    }
    zipnode->min = length;
    zipnode->max = length;
    zipnode->sum = length;
    zipnode->count = 1;
    zipnode->stamp = stamp_count;
  } else {
    zipnode_t* zipnode = &node->table[ym];
    if (length < zipnode->min) {
      zipnode->min = length;
    }
    if (length > zipnode->max) {
      zipnode->max = length;
    }
    zipnode->sum += length;
    zipnode->stamp += stamp_count;
    zipnode->count++;
  }

  return 0;
}

uint32_t parse_uint_field(const char** cursor) {
  const char* p = *cursor;
  uint32_t value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    p++;
  }
  if (*p == ',') p++;
  *cursor = p;
  return value;
}

void read_from_csv(hash_slot_t hash_table[], const char* p, const char* end) {
  while (p < end) {
    uint32_t timestamp = parse_uint_field(&p);
    uint64_t path_hash = UINT64_C(14695981039346656037);

    const char* path_start = p;
    while (*p != ',') {
      path_hash ^= (unsigned char)*p++;
      path_hash *= UINT64_C(1099511628211);
    }
    size_t path_length = p - path_start;
    p++;
    uint32_t length = parse_uint_field(&p);
    uint32_t stamp_count = parse_uint_field(&p);
    p++;

    if (add_message(hash_table, timestamp, path_hash, path_start, path_length,
                    length, stamp_count) != 0) {
      fprintf(stderr, "Failed to add message\n");
    }
  }
}

int main(int argc, char* argv[]) {
  hash_slot_t hash_table[HASHTABLE_SIZE] = {0};

  FILE* infp;
  int n;

  if (argc != 3) {
    fprintf(stderr, "Usage: %s <input_file> <output_file>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int fd = open(argv[1], O_RDONLY);

  struct stat st;
  fstat(fd, &st);
  size_t size = st.st_size;
  void* mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (mapping == MAP_FAILED) {
    fprintf(stderr, "Error: Could not map file %s\n", argv[1]);
    exit(EXIT_FAILURE);
  }

  const char* input_begin = (const char*)mapping;
  const char* input_end = input_begin + size;

  const char* header_end = memchr(input_begin, '\n', size);
  if (header_end == NULL) {
    fprintf(stderr, "Error: Could not find header line in file %s\n", argv[1]);
    munmap(mapping, size);
    exit(EXIT_FAILURE);
  }

  read_from_csv(hash_table, header_end + 1, input_end);

  munmap(mapping, size);

  // if ((infp = fopen(argv[1], "r")) == NULL) {
  //   fprintf(stderr, "Error: Could not open file %s\n", argv[1]);
  //   exit(EXIT_FAILURE);
  // }

  // fseek(infp, 55, SEEK_SET);
  // n = read_from_csv(hash_table, infp);
  // if (n < 0) {
  //   fprintf(stderr, "Error reading CSV file\n");
  //   exit(EXIT_FAILURE);
  // }
  // fclose(infp);

  FILE* outfp;
  if ((outfp = fopen(argv[2], "w")) == NULL) {
    fprintf(stderr, "Error: Could not open file %s\n", argv[2]);
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < HASHTABLE_SIZE; i++) {
    if (hash_table[i].channel != NULL) {
      for (int j = 0; j < 12; j++) {
        if (hash_table[i].channel->table[j].count != 0) {
          zipnode_t* zipnode = &hash_table[i].channel->table[j];
          // channel_path,YYYY-MM=min_length/average_length/max_length/message_count/total_stamp_count
          fprintf(outfp, "%s,2027-%02d=%u/%.2lf/%u/%u/%u\n",
                  hash_table[i].channel->path, j + 1, zipnode->min,
                  (double)zipnode->sum / zipnode->count, zipnode->max,
                  zipnode->count, zipnode->stamp);
          // free(zipnode);
        }
      }
      free(hash_table[i].channel);
    }
  }
  fclose(outfp);
}
