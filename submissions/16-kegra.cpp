#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <execution>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

constexpr auto buf_sz = 32768;

// struct FileStream {
//     std::ifstream ifs;
//     void file_open(const char *path) { ifs.open(path); }
//     void read_line_ptr(char *buf, size_t sz) { ifs.getline(buf, sz); }
// };

struct FileStream {
    int fd;
    char buf[buf_sz];
    char *bufbegin, *bufend;

    void file_open(const char *path) {
        fd = open(path, O_RDONLY);
        auto sz = read(fd, buf, buf_sz);
        bufbegin = buf;
        bufend = buf + sz;
    }
    void read_line_ptr(char *dstbuf, size_t sz) {
        if (fd == 0) {
            *dstbuf = '\0';
            return;
        }
        auto pos = std::find(bufbegin, bufend, '\n');
        std::memcpy(dstbuf, bufbegin, pos - bufbegin);
        dstbuf += pos - bufbegin;
        if (pos == bufend) {
            auto read_sz = read(fd, buf, buf_sz);
            if (read_sz == 0) {
                fd = 0;
                goto end;
            }
            bufbegin = buf;
            bufend = buf + read_sz;

            pos = std::find(bufbegin, bufend, '\n');
            std::memcpy(dstbuf, bufbegin, pos - bufbegin);
            dstbuf += pos - bufbegin;
        }
    end:
        bufbegin = pos + 1;
        *dstbuf = '\0';
    }
};

struct Entry {
    uint32_t timestamp;
    uint32_t len;
    uint32_t stamps;
};

struct Stat {
    uint32_t len_min = UINT32_MAX, len_max = 0, len_sum = 0, entries = 0, stamps = 0;
};

// #define MEAS

#ifdef MEAS
std::chrono::system_clock::time_point _tm, _tm2;
#define start_meas _tm = std::chrono::system_clock::now();
#define end_meas                                                                                                       \
                                                                                                                       \
    {                                                                                                                  \
        _tm2 = std::chrono::system_clock::now();                                                                       \
        _meas[__LINE__] += std::chrono::duration_cast<std::chrono::nanoseconds>(_tm2 - _tm).count();                   \
    }
#define show_meas                                                                                                      \
    {                                                                                                                  \
        for (const auto [line, tm] : _meas) {                                                                          \
            std::cout << "L" << line << ": " << tm << " ns\n";                                                         \
        }                                                                                                              \
    }

std::map<int, uint64_t> _meas;
#else
#define start_meas ;
#define end_meas ;
#define show_meas ;
#endif

template <class T> struct Allocator {
    typedef T value_type;

    T *p, *pi;
    T *allocate(size_t n) {
        auto pp = p;
        p += n;
        return pp;
    }
    void deallocate(T *dp, size_t n) {}
    Allocator() { pi = p = (T *)std::malloc(256 * 1024 * 1024); }
    ~Allocator() {}
    void reset() { p = pi; }
};

template <class T> struct PAllocator {
    typedef T value_type;
    Allocator<T> *allocator;

    T *allocate(size_t n) { return allocator->allocate(n); }
    void deallocate(T *dp, size_t n) {}
};

struct string_hash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(const char *txt) const { return std::hash<std::string_view>{}(txt); }
    [[nodiscard]] size_t operator()(std::string_view txt) const { return std::hash<std::string_view>{}(txt); }
    [[nodiscard]] size_t operator()(const std::string &txt) const { return std::hash<std::string>{}(txt); }
};

int main(int argc, char *argv[]) {
    Allocator<Entry> allocator;
    PAllocator<Entry> pallocator{&allocator};

    FileStream read_stream;
    // std::map<std::string, std::array<Stat, 13>, std::ranges::less> stat_db;
    std::unordered_map<std::string, std::array<Stat, 13>, string_hash, std::equal_to<>> stat_db;
    int perm[4];

    constexpr int month_table[] = {
        1798761600, 1801440000, 1803859200, 1806537600, 1809129600, 1811808000, 1814400000,
        1817078400, 1819756800, 1822348800, 1825027200, 1827619200, 1830297600,
    };

    read_stream.file_open(argv[1]);

    {
        char buf[1024];
        read_stream.read_line_ptr(buf, 1024);
        std::string_view names = std::string_view(buf);
        const char *p = names.data(), *pe = p + names.length();
        for (int i = 0; i < 4; i++) {
            switch (*p) {
            case 'u': // unix_timestamp
                perm[i] = 0;
                break;
            case 'c': // channel_path
                perm[i] = 1;
                break;
            case 'm': // message_length
                perm[i] = 2;
                break;
            case 's': // stamp_count
                perm[i] = 3;
                break;
            default:
                break;
            }
            p = std::find(p, pe, ',') + 1;
        }
    }

    bool end_flag = false;

    while (!end_flag) {
        allocator.reset();
        // std::map<std::string, std::vector<Entry, PAllocator<Entry>>, std::ranges::less> entry_db;
        std::unordered_map<std::string, std::vector<Entry, PAllocator<Entry>>, string_hash, std::equal_to<>> entry_db;

        for (int rc = 0; rc < 200'000; rc++) {
            char buf[256];
            start_meas;
            read_stream.read_line_ptr(buf, 256);
            end_meas;
            std::string_view str = std::string_view(buf);
            if (str.empty()) {
                end_flag = true;
                break;
            }

            Entry entry;

            {
                const char *str_begin[4], *str_end[4];
                start_meas;
                const char *p = str.data(), *pe = p + str.length(), *pse;
                for (int i = 0; i < 4; i++) {
                    pse = std::find(p, pe, ',');
                    str_begin[perm[i]] = p;
                    str_end[perm[i]] = pse;
                    p = pse + 1;
                }
                end_meas;

                start_meas;
                // unix_timestamp
                std::from_chars(str_begin[0], str_end[0], entry.timestamp);
                // channel_path
                auto path = std::string_view(str_begin[1], str_end[1] - str_begin[1]);
                // message_length
                std::from_chars(str_begin[2], str_end[2], entry.len);
                // stamp_count
                std::from_chars(str_begin[3], str_end[3], entry.stamps);
                end_meas;

                start_meas;
                auto it = entry_db.find(path);
                end_meas;
                start_meas;
                if (it == entry_db.end()) {
                    auto [it, res] = entry_db.emplace(path, std::vector<Entry, PAllocator<Entry>>(pallocator));
                    it->second.push_back(entry);
                } else {
                    it->second.push_back(entry);
                }
                end_meas;
            }
        }
        for (auto &[path, entries] : entry_db) {
            start_meas;
            // std::sort(entries.begin(), entries.end(),
            //           [](const Entry &p, const Entry &q) { return p.timestamp < q.timestamp; });
            {
                auto p = entries.begin();
                for (int m = 1; m <= 12; m++) {
                    auto it = entries.begin();
                    for (auto it = p; it != entries.end(); it++) {
                        if (it->timestamp < month_table[m]) {
                            std::swap(*p, *it);
                            p++;
                        }
                    }
                }
            }
            end_meas;

            start_meas;
            auto &stats = stat_db[path];
            end_meas;

            start_meas;
            int month = 1;
            for (const auto &entry : entries) {
                while (entry.timestamp >= month_table[month])
                    month++;

                auto &stat = stats[month];
                stat.entries++;
                stat.len_max = std::max(stat.len_max, entry.len);
                stat.len_min = std::min(stat.len_min, entry.len);
                stat.len_sum += entry.len;
                stat.stamps += entry.stamps;
            }
            end_meas;
        };
    }

    start_meas;
    std::ofstream ofs(argv[2]);
    ofs << std::fixed << std::setprecision(2);
    for (const auto &[path, stats] : stat_db) {
        for (int i = 1; i <= 12; i++) {
            const auto &stat = stats[i];
            if (stat.entries == 0)
                continue;

            ofs << path.data() << ",2027-";
            if (i < 10)
                ofs << '0';
            ofs << i << '=' << stat.len_min << '/';
            ofs << (double(stat.len_sum) / stat.entries) << '/';
            ofs << stat.len_max << '/';
            ofs << stat.entries << '/';
            ofs << stat.stamps;
            ofs << '\n';
        }
    }
    end_meas;

    show_meas;

    return 0;
}
