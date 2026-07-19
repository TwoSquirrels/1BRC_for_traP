#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kStartOf2027 = 1'798'761'600;
constexpr std::uint64_t kStartOf2028 = 1'830'297'600;
constexpr std::array<unsigned, 13> kMonthStartDays{
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365,
};

struct Key {
    std::string channel;
    std::uint8_t month;

    bool operator==(const Key&) const = default;
};

struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        const std::size_t channel_hash = std::hash<std::string>{}(key.channel);
        return channel_hash ^ (static_cast<std::size_t>(key.month) * 0x9e3779b97f4a7c15ULL);
    }
};

struct Stats {
    std::uint32_t min_length = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t total_length = 0;
    std::uint32_t max_length = 0;
    std::uint32_t message_count = 0;
    std::uint32_t total_stamp_count = 0;

    void add(std::uint32_t length, std::uint32_t stamps) noexcept {
        min_length = std::min(min_length, length);
        total_length += length;
        max_length = std::max(max_length, length);
        ++message_count;
        total_stamp_count += stamps;
    }
};

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) noexcept {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && position == end;
}

unsigned month_from_timestamp(std::uint64_t timestamp) noexcept {
    const auto day = static_cast<unsigned>((timestamp - kStartOf2027) / 86'400);
    return static_cast<unsigned>(
        std::upper_bound(kMonthStartDays.begin(), kMonthStartDays.end(), day) -
        kMonthStartDays.begin());
}

bool parse_row(
    std::string_view line,
    std::uint64_t& timestamp,
    std::string_view& channel,
    std::uint32_t& message_length,
    std::uint32_t& stamp_count) noexcept {
    const std::size_t first = line.find(',');
    if (first == std::string_view::npos) {
        return false;
    }
    const std::size_t second = line.find(',', first + 1);
    if (second == std::string_view::npos) {
        return false;
    }
    const std::size_t third = line.find(',', second + 1);
    if (third == std::string_view::npos || line.find(',', third + 1) != std::string_view::npos) {
        return false;
    }

    channel = line.substr(first + 1, second - first - 1);
    return !channel.empty() &&
           parse_integer(line.substr(0, first), timestamp) &&
           parse_integer(line.substr(second + 1, third - second - 1), message_length) &&
           parse_integer(line.substr(third + 1), stamp_count) &&
           timestamp >= kStartOf2027 && timestamp < kStartOf2028 &&
           message_length >= 1;
}

int run(const char* input_path, const char* output_path) {
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        std::cerr << "failed to open input: " << input_path << '\n';
        return 1;
    }

    std::string line;
    if (!std::getline(input, line) ||
        line != "unix_timestamp,channel_path,message_length,stamp_count") {
        std::cerr << "invalid or missing CSV header\n";
        return 1;
    }

    std::unordered_map<Key, Stats, KeyHash> groups;
    groups.reserve(120'000);

    std::uint64_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        std::uint64_t timestamp = 0;
        std::string_view channel;
        std::uint32_t message_length = 0;
        std::uint32_t stamp_count = 0;
        if (!parse_row(line, timestamp, channel, message_length, stamp_count)) {
            std::cerr << "invalid CSV row at line " << line_number << '\n';
            return 1;
        }

        Key key{std::string(channel), static_cast<std::uint8_t>(month_from_timestamp(timestamp))};
        groups[std::move(key)].add(message_length, stamp_count);
    }
    if (input.bad()) {
        std::cerr << "failed while reading input\n";
        return 1;
    }

    std::vector<const std::pair<const Key, Stats>*> ordered;
    ordered.reserve(groups.size());
    for (const auto& group : groups) {
        ordered.push_back(&group);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->first.channel != right->first.channel) {
            return left->first.channel < right->first.channel;
        }
        return left->first.month < right->first.month;
    });

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        std::cerr << "failed to open output: " << output_path << '\n';
        return 1;
    }
    output << std::fixed << std::setprecision(2);
    for (const auto* group : ordered) {
        const Key& key = group->first;
        const Stats& stats = group->second;
        const double average = static_cast<double>(stats.total_length) /
                               static_cast<double>(stats.message_count);
        output << key.channel << ",2027-" << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(key.month) << std::setfill(' ') << '='
               << stats.min_length << '/' << average << '/' << stats.max_length << '/'
               << stats.message_count << '/' << stats.total_stamp_count << '\n';
    }
    if (!output) {
        std::cerr << "failed while writing output\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " INPUT.csv OUTPUT.txt\n";
        return 2;
    }
    return run(argv[1], argv[2]);
}

