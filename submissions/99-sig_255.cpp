#include <cstdint>
#include <cstdio>
#include <array>
#include <string>
#include <map>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;


inline __attribute__((always_inline)) u64 parseu64_44 (const u8* &p) {
	u64 val = 0;
	while (*p != 252) { // ',' - '0'
		val = val * 10 + *p;
		p++;
	}
	return val;
}

inline __attribute__((always_inline)) u64 parseu64_10 (const u8* &p) {
	u64 val = 0;
	while (*p != 218) { // '\n' - '0'
		val = val * 10 + *p;
		p++;
	}
	return val;
}

constexpr u64 MONTH_START[13] = {
	17987616,
	18014400,
	18038592,
	18065376,
	18091296,
	18118080,
	18144000,
	18170784,
	18197568,
	18223488,
	18250272,
	18276192,
	99999999
};

inline __attribute__((always_inline)) u64 timestamp2month (const u8* &p) {
	u64& v = *(u64*)p;
	v = ((v * 10) + (v >> 8)) & 0x00FF'00FF'00FF'00FFULL;
	v = ((v * 100) + (v >> 16)) & 0x0000'FFFF'0000'FFFFULL;
	v = ((v * 10000) + (v >> 32)) & 0x0000'0000'FFFFFFFFULL;
	u64 month = v;
	p += 10;
	month -= 17961796;
	month *= 163200;
	month >>= 32;
	return month + (MONTH_START[month] <= v);
}

inline __attribute__((always_inline)) u64 channel_hash (const u8* &p) {
	u64 hash = 0;
	while (*p != 252) { // ',' - '0'
		hash = hash * 255'255'255'255ULL + *p;
		p++;
	}
	return hash;
}


struct Stat {
	u32 min_length = UINT32_MAX;
	u32 max_length = 0;
	u32 total_length = 0;
	u32 message_count = 0;
	u32 total_stamp_count = 0;
};

struct Entry {
	u64 key = 0;
	std::array<Stat, 12> months{};
};

constexpr u64 BUCKET_SIZE = 1ULL << 18;
constexpr u64 MASK = BUCKET_SIZE - 1;
Entry table[BUCKET_SIZE]; // 100000 / 262144 = 38.1 %
std::map<std::string, u64> channel_names;

inline __attribute__((always_inline)) std::array<Stat, 12>& get_stats(u64 channel, u8* channel_start, u8* channel_end) {
	u64 h = channel & MASK;
	if (table[h].key == channel) {
		return table[h].months;
	}
	while (true) {
		if (table[h].key == 0) {
			table[h].key = channel;
			for (u8* p = channel_start; p < channel_end; p++) {
				*p += '0';
			}
			channel_names.emplace(std::string((const char*)channel_start, channel_end - channel_start), h);
			return table[h].months;
		} else if (table[h].key == channel) {
			return table[h].months;
		} else {
			h = (h + 1) & MASK;
		}
	}
}

u8 buffer[1024 * 1024];

int main(int argc, char* argv[]) {
	FILE* fp = fopen(argv[1], "rb");
	bool first_line = true;
	while (true) {
		u64 read_size = fread(buffer, 1, sizeof(buffer), fp);
		const u8* buf = buffer;
		const u8* buf_end = buffer + read_size;
		if (read_size == 0) break;
		for (u64 i = 0; i < sizeof(buffer); i++) {
			buffer[i] -= '0';
		}
		if (read_size == sizeof(buffer)) {
			while (*(buf_end - 1) != 218) buf_end--; // '\n' - '0'
			fseek(fp, buf_end - buffer - read_size, SEEK_CUR);
		}
		if (first_line) {
			while (*buf != 218) buf++;
			buf++;
			first_line = false;
		}
		while (buf < buf_end) {
			u64 month = timestamp2month(buf);
			buf ++;
			const u8* channel_start = buf;
			u64 channel = channel_hash(buf);
			const u8* channel_end = buf;
			buf ++;
			u32 len = parseu64_44(buf);
			buf++;
			u32 stamp = parseu64_10(buf);
			buf++;
			Stat& stats = get_stats(channel, const_cast<u8*>(channel_start), const_cast<u8*>(channel_end))[month - 1];
			stats.min_length = std::min(stats.min_length, len);
			stats.max_length = std::max(stats.max_length, len);
			stats.total_length += len;
			stats.message_count++;
			stats.total_stamp_count += stamp;
		}
	}
	
	FILE* out_fp = fopen(argv[2], "wb");
	for (const auto& [name, hash] : channel_names) {
		std::array<Stat, 12>& months = table[hash].months;
		for (u32 month = 1; month <= 12; month++) {
			Stat& stats = months[month - 1];
			if (stats.message_count == 0) continue;
			fprintf(out_fp, "%s,2027-%02u=%u/%.2f/%u/%u/%u\n",
				name.c_str(),
				month,
				stats.min_length,
				(double)stats.total_length / stats.message_count,
				stats.max_length,
				stats.message_count,
				stats.total_stamp_count
			);
		}
	}
	fclose(out_fp);
	return 0;
}
