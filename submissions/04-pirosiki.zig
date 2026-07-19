const std = @import("std");
const Io = std.Io;
const config = @import("config");

/// printf("%.2f")相当の丸め: f64が表す正確な値の100倍をround-half-evenで整数化する。
/// {d:.2}は最短10進表現経由のhalf-up、average*100.0は乗算自体が丸めを起こすため、
/// どちらも期待出力とずれる。ビットを分解して整数演算で正確に比較する。
fn averageCents(average: f64) u64 {
    const bits: u64 = @bitCast(average);
    const raw_exp: i32 = @intCast((bits >> 52) & 0x7FF);
    std.debug.assert(raw_exp != 0 and raw_exp != 0x7FF); // 正規化数のみ想定
    const mantissa: u128 = (bits & ((1 << 52) - 1)) | (1 << 52);
    const exp: i32 = raw_exp - 1075; // average = mantissa * 2^exp
    const scaled: u128 = mantissa * 100;
    if (exp >= 0) return @intCast(scaled << @intCast(exp));
    const shift: u7 = @intCast(-exp);
    const cents: u64 = @intCast(scaled >> shift);
    const remainder = scaled - (@as(u128, cents) << shift);
    const half = @as(u128, 1) << (shift - 1);
    if (remainder > half or (remainder == half and cents % 2 == 1)) return cents + 1;
    return cents;
}

// ルール上、channel×monthごとの各累計値はu32に収まる。
const StatsVector = @Vector(4, u32);

const Stats = extern struct {
    total_length: u32 = 0,
    message_count: u32 = 0,
    total_stamp_count: u32 = 0,
    // 下位16bitがmin_length、上位16bitがmax_length。
    length_range: u32 = std.math.maxInt(u16),

    inline fn minLength(self: *const Stats) u16 {
        return @truncate(self.length_range);
    }

    inline fn maxLength(self: *const Stats) u16 {
        return @truncate(self.length_range >> 16);
    }
};

const YearStats = struct {
    months: [12]Stats = @splat(.{}),
};

const fixed_year: std.time.epoch.Year = 2027;
const year_start_timestamp: u64 = 1_798_761_600;
const seconds_per_day: u64 = 86_400;
const MonthBoundaryVector = @Vector(4, u64);

fn asciiTimestampPrefix(timestamp: u64) u64 {
    var prefix = timestamp / 100;
    var divisor: u64 = 10_000_000;
    var result: u64 = 0;
    for (0..8) |_| {
        result = (result << 8) | ('0' + (prefix / divisor) % 10);
        prefix %= divisor;
        divisor /= 10;
    }
    return result;
}

const month_boundaries: [3]MonthBoundaryVector = table: {
    const days_per_month = [_]u8{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    var result: [3]MonthBoundaryVector = @splat(@splat(std.math.maxInt(u64)));
    var timestamp = year_start_timestamp;
    for (days_per_month[0..11], 0..) |day_count, boundary_index| {
        timestamp += @as(u64, day_count) * seconds_per_day;
        result[boundary_index / 4][boundary_index % 4] = asciiTimestampPrefix(timestamp);
    }
    break :table result;
};

comptime {
    if (@sizeOf(Stats) != 16) @compileError("Stats must remain a compact 16-byte record");
    if (@sizeOf(YearStats) != 192) @compileError("YearStats must contain twelve compact Stats records");
}

const PackedChannelKey = [4]u64;

inline fn mixChannelSignature(folded_key: u64, channel_length: usize) u64 {
    // frozen化するときに全channelの(index, fingerprint)が一意か検証するため、
    // ここでは暗号学的なhashよりも毎行の命令数を優先する。
    var signature = folded_key ^ @as(u64, @intCast(channel_length));
    signature *%= 0x9E3779B97F4A7C15;
    signature ^= signature >> 32;
    return signature | 1;
}

inline fn channelSignature(key: PackedChannelKey, channel_length: usize) u64 {
    return mixChannelSignature(key[0] ^ key[1] ^ key[2] ^ key[3], channel_length);
}

const HotStatsCache = struct {
    const discovery_entry_count = 8192;
    const frozen_entry_count = 32_768;
    const signature_table_count = 16_384;
    const frozen_stats_offset_bits = 17;
    const frozen_stats_offset_mask = (1 << frozen_stats_offset_bits) - 1;

    const DiscoveryEntry = struct {
        key: [2]u64 = @splat(0),
        stats: *YearStats = undefined,
        channel_length: u8 = std.math.maxInt(u8),
    };

    const Storage = union(enum) {
        discovery: [discovery_entry_count]DiscoveryEntry,
        // 上位15bitがfingerprint、下位17bitが16B単位のstats offset + 1。
        // pointerを直接持たず、32K entriesを128KiBに抑える。
        frozen: [frozen_entry_count]u32,
    };

    storage: Storage = .{ .discovery = @splat(.{}) },
    freeze_attempted: bool = false,
    stats_base_address: usize = 0,

    inline fn discoveryEntryIndex(key: [2]u64) usize {
        const mixed = (key[0] ^ std.math.rotl(u64, key[1], 23)) *% 0x9E3779B97F4A7C15;
        return @intCast(mixed >> (64 - 13));
    }

    inline fn frozenEntryIndex(signature: u64) usize {
        return @intCast(signature >> (64 - 15));
    }

    inline fn frozenFingerprint(signature: u64) u15 {
        return @truncate(signature);
    }

    inline fn getDiscovery(
        self: *HotStatsCache,
        channel_path: []const u8,
        key: PackedChannelKey,
    ) ?*YearStats {
        if (channel_path.len > 16) return null;
        const short_key: [2]u64 = key[0..2].*;
        const entry = &self.storage.discovery[discoveryEntryIndex(short_key)];
        if (entry.channel_length == channel_path.len and
            entry.key[0] == short_key[0] and
            entry.key[1] == short_key[1])
        {
            return entry.stats;
        }
        return null;
    }

    inline fn getFrozen(
        self: *HotStatsCache,
        signature: u64,
    ) ?*YearStats {
        const entry = self.storage.frozen[frozenEntryIndex(signature)];
        if (@as(u15, @truncate(entry >> frozen_stats_offset_bits)) == frozenFingerprint(signature)) {
            const stats_offset_units_plus_one = entry & frozen_stats_offset_mask;
            const stats_offset = @as(usize, stats_offset_units_plus_one - 1) << 4;
            return @ptrFromInt(self.stats_base_address + stats_offset);
        }
        return null;
    }

    inline fn putDiscovery(
        self: *HotStatsCache,
        channel_path: []const u8,
        key: PackedChannelKey,
        stats: *YearStats,
    ) void {
        if (channel_path.len > 16) return;
        const short_key: [2]u64 = key[0..2].*;
        self.storage.discovery[discoveryEntryIndex(short_key)] = .{
            .key = short_key,
            .stats = stats,
            .channel_length = @intCast(channel_path.len),
        };
    }

    inline fn putFrozen(
        self: *HotStatsCache,
        signature: u64,
        stats: *YearStats,
    ) void {
        const stats_offset = @intFromPtr(stats) - self.stats_base_address;
        const stats_offset_units_plus_one: u32 = @intCast((stats_offset >> 4) + 1);
        self.storage.frozen[frozenEntryIndex(signature)] =
            (@as(u32, frozenFingerprint(signature)) << frozen_stats_offset_bits) |
            stats_offset_units_plus_one;
    }

    fn freeze(self: *HotStatsCache, records: []const ChannelRecord, stats_base: *YearStats) bool {
        if (self.freeze_attempted) return false;
        self.freeze_attempted = true;

        var signatures: [signature_table_count]u64 = @splat(0);
        for (records) |record| {
            const signature = channelSignature(record.key, record.channel_length);
            const lookup_key = (@as(u64, @intCast(frozenEntryIndex(signature))) << 15) |
                frozenFingerprint(signature);
            var index: usize = @intCast((lookup_key *% 0x9E3779B97F4A7C15) >> (64 - 14));
            while (signatures[index] != 0) : (index = (index + 1) & (signature_table_count - 1)) {
                if (signatures[index] == lookup_key) return false;
            }
            signatures[index] = lookup_key;
        }

        self.stats_base_address = @intFromPtr(stats_base);
        self.storage = .{ .frozen = @splat(0) };
        return true;
    }
};

const ChannelRecord = struct {
    key: PackedChannelKey,
    channel_length: u32,
};

const ChannelSlot = struct {
    hash: u64,
    stats_index: u16,
};

comptime {
    if (@sizeOf(ChannelRecord) != 40) @compileError("ChannelRecord must remain a compact 40-byte record");
    if (@sizeOf(ChannelSlot) != 16) @compileError("ChannelSlot must remain a compact 16-byte slot");
}

const ChannelMap = struct {
    controls: []u16 = &.{},
    // probe中に触るhashとstats_indexだけをcompactなslotへ置き、完全keyはcold側へ分離する。
    slots: []ChannelSlot = &.{},
    records: []ChannelRecord = &.{},
    stats: []YearStats = &.{},
    channel_ptrs: [][*]const u8 = &.{},
    count: usize = 0,
    hash_collision: bool = false,
    frozen: bool = false,

    const max_channel_count = 10_000;
    const capacity = 16_384;

    inline fn controlTag(hash: u64) u16 {
        // indexに使う下位14bitとは独立した上位16bitを使い、0は空slot用に残す。
        const tag: u16 = @truncate(hash >> 48);
        return if (tag == 0) 1 else tag;
    }

    inline fn channelPath(self: *const ChannelMap, stats_index: usize) []const u8 {
        return self.channel_ptrs[stats_index][0..self.records[stats_index].channel_length];
    }

    inline fn keysEqual(
        self: *const ChannelMap,
        record: *const ChannelRecord,
        stats_index: usize,
        channel_path: []const u8,
        key: PackedChannelKey,
    ) bool {
        if (record.channel_length != channel_path.len) return false;
        inline for (0..key.len) |i| {
            if (record.key[i] != key[i]) return false;
        }
        if (channel_path.len <= @sizeOf(PackedChannelKey)) return true;
        const entry_path = self.channelPath(stats_index);
        return std.mem.eql(
            u8,
            entry_path[@sizeOf(PackedChannelKey)..],
            channel_path[@sizeOf(PackedChannelKey)..],
        );
    }

    fn init(self: *ChannelMap, allocator: std.mem.Allocator) !void {
        self.stats = try allocator.alloc(YearStats, max_channel_count);
        self.channel_ptrs = try allocator.alloc([*]const u8, max_channel_count);
        self.controls = try allocator.alloc(u16, capacity);
        @memset(self.controls, 0);
        self.slots = try allocator.alloc(ChannelSlot, capacity);
        self.records = try allocator.alloc(ChannelRecord, max_channel_count);
    }

    fn getOrPut(
        self: *ChannelMap,
        allocator: std.mem.Allocator,
        channel_path: []const u8,
        key: PackedChannelKey,
    ) !*YearStats {
        if (self.slots.len == 0) try self.init(allocator);

        const hash = std.hash.Wyhash.hash(0, channel_path);
        const control = controlTag(hash);
        const mask = capacity - 1;
        var index: usize = @intCast(hash & mask);

        while (true) {
            const slot_control = self.controls[index];
            if (slot_control == 0) {
                if (self.count == max_channel_count) return error.TooManyChannels;
                const stats_index: u16 = @intCast(self.count);
                const stats = &self.stats[stats_index];
                stats.* = .{};
                self.channel_ptrs[stats_index] = channel_path.ptr;
                self.records[stats_index] = .{
                    .key = key,
                    .channel_length = @intCast(channel_path.len),
                };
                self.slots[index] = .{
                    .hash = hash,
                    .stats_index = stats_index,
                };
                self.controls[index] = control;
                self.count += 1;
                // 10,000種類に達した後は、入力ルール上未知channelが現れない。
                // full hashの衝突も発見されていなければ完全key比較を安全に省略できる。
                if (self.count == max_channel_count and !self.hash_collision) self.frozen = true;
                return stats;
            }
            if (slot_control == control) {
                const slot = &self.slots[index];
                if (slot.hash == hash) {
                    if (self.frozen or self.keysEqual(
                        &self.records[slot.stats_index],
                        slot.stats_index,
                        channel_path,
                        key,
                    )) {
                        return &self.stats[slot.stats_index];
                    }
                    self.hash_collision = true;
                }
            }
            index = (index + 1) & mask;
        }
    }
};

fn writeYearStats(
    out: *Io.Writer,
    channel_path: []const u8,
    year: std.time.epoch.Year,
    year_stats: *const YearStats,
) !void {
    for (0..year_stats.months.len) |month_index| {
        if (year_stats.months[month_index].message_count == 0) continue;
        const year_month = @as(u32, year) * 100 + @as(u32, @intCast(month_index + 1));
        try writeStats(out, channel_path, year_month, &year_stats.months[month_index]);
    }
}

fn writeStats(out: *Io.Writer, channel_path: []const u8, year_month: u32, stats: *const Stats) !void {
    const average: f64 = @as(f64, @floatFromInt(stats.total_length)) /
        @as(f64, @floatFromInt(stats.message_count));
    const cents = averageCents(average);
    try out.print("{s},{d}-{d:0>2}={d}/{d}.{d:0>2}/{d}/{d}/{d}\n", .{
        channel_path,
        year_month / 100,
        year_month % 100,
        stats.minLength(),
        cents / 100,
        cents % 100,
        stats.maxLength(),
        stats.message_count,
        stats.total_stamp_count,
    });
}

/// posからdelimiterの直前までを符号なし10進整数として読み、posをdelimiterの次へ進める。
/// 1BRCの入力は正しいCSVであることを前提に、符号・基数・overflowの検査を省く。
inline fn parseUnsignedUntil(data: []const u8, pos: *usize, delimiter: u8) u64 {
    var value: u64 = 0;
    while (data[pos.*] != delimiter) : (pos.* += 1) {
        value = value * 10 + (data[pos.*] - '0');
    }
    pos.* += 1;
    return value;
}

/// message_lengthは99%以上が2桁か3桁なので、その2ケースを1本のrare branchと
/// branchlessな選択で処理する。1桁と4桁以上だけ汎用parserへfallbackする。
inline fn parseMessageLength(data: []const u8, pos: *usize) u16 {
    const start = pos.*;
    const comma_at_2: u2 = @intFromBool(data[start + 2] == ',');
    const comma_at_3: u2 = @intFromBool(data[start + 3] == ',');
    const comma_bits = comma_at_2 | (comma_at_3 << 1);
    if (comma_bits == 0) return @intCast(parseUnsignedUntil(data, pos, ','));

    const first_two = @as(u16, data[start] - '0') * 10 + (data[start + 1] - '0');
    const is_three: u16 = comma_bits >> 1;
    const third_ascii: u16 = data[start + 2];
    const value = first_two + is_three * (first_two * 9 + third_ascii) - is_three * '0';
    pos.* = start + 3 + is_three;
    return value;
}

/// stamp_countはほぼ常に1桁なので、改行までの汎用ループを避ける。
inline fn parseStampCount(data: []const u8, pos: *usize) u32 {
    const start = pos.*;
    if (start + 1 < data.len and data[start + 1] == '\n') {
        pos.* = start + 2;
        return data[start] - '0';
    }

    var value: u32 = 0;
    while (pos.* < data.len and data[pos.*] != '\n') : (pos.* += 1) {
        value = value * 10 + (data[pos.*] - '0');
    }
    if (pos.* < data.len) pos.* += 1;
    return value;
}

/// 月境界はすべて100秒単位なので、先頭8桁のASCII比較だけで月を確定できる。
inline fn parseMonth2027(data: []const u8, pos: *usize) u4 {
    const start = pos.*;
    const raw_prefix = @as(*align(1) const u64, @ptrCast(data.ptr + start)).*;
    const prefix: MonthBoundaryVector = @splat(@byteSwap(raw_prefix));
    const passed_0: u4 = @bitCast(prefix >= month_boundaries[0]);
    const passed_1: u4 = @bitCast(prefix >= month_boundaries[1]);
    const passed_2: u4 = @bitCast(prefix >= month_boundaries[2]);
    pos.* = start + 11;
    return @as(u4, @popCount(passed_0)) +
        @as(u4, @popCount(passed_1)) +
        @as(u4, @popCount(passed_2));
}

const ParsedChannel = struct {
    path: []const u8,
    key: PackedChannelKey,
};

/// channel pathの先頭32byteを一度に読み、comma探索とpacked key生成を同時に行う。
/// 32byte内にcommaがない場合とchunk末尾だけscalar処理へfallbackする。
inline fn parseChannel(data: []const u8, pos: *usize) ParsedChannel {
    const start = pos.*;
    const Block = @Vector(@sizeOf(PackedChannelKey), u8);

    if (data.len - start >= @sizeOf(PackedChannelKey)) {
        const block = @as(*align(1) const Block, @ptrCast(data.ptr + start)).*;
        const comma_bits: u32 = @bitCast(block == @as(Block, @splat(',')));
        if (comma_bits != 0) {
            const channel_length: u6 = @intCast(@ctz(comma_bits));
            const key_bytes = @select(
                u8,
                std.simd.iota(u8, @sizeOf(PackedChannelKey)) < @as(Block, @splat(channel_length)),
                block,
                @as(Block, @splat(0)),
            );
            pos.* = start + channel_length + 1;
            return .{
                .path = data[start .. start + channel_length],
                .key = @bitCast(key_bytes),
            };
        }
    }

    var channel_key_bytes: [@sizeOf(PackedChannelKey)]u8 = @splat(0);
    var channel_length: usize = 0;
    while (data[start + channel_length] != ',') : (channel_length += 1) {
        if (channel_length < channel_key_bytes.len) {
            channel_key_bytes[channel_length] = data[start + channel_length];
        }
    }
    pos.* = start + channel_length + 1;
    return .{
        .path = data[start .. start + channel_length],
        .key = @bitCast(channel_key_bytes),
    };
}

const ParsedFrozenChannel = struct {
    path: []const u8,
    signature: u64,
};

/// frozen後は完全keyを返さず、SIMDレジスタ上でlookup用signatureまで生成する。
/// cache miss時だけpackedChannelKeyで完全keyを再構築する。
inline fn parseFrozenChannel(data: []const u8, pos: *usize) ParsedFrozenChannel {
    const start = pos.*;
    const Block = @Vector(@sizeOf(PackedChannelKey), u8);
    const WordBlock = @Vector(@sizeOf(PackedChannelKey) / @sizeOf(u64), u64);

    if (data.len - start >= @sizeOf(PackedChannelKey)) {
        const block = @as(*align(1) const Block, @ptrCast(data.ptr + start)).*;
        const comma_bits: u32 = @bitCast(block == @as(Block, @splat(',')));
        if (comma_bits != 0) {
            const channel_length: u6 = @intCast(@ctz(comma_bits));
            const key_bytes = @select(
                u8,
                std.simd.iota(u8, @sizeOf(PackedChannelKey)) < @as(Block, @splat(channel_length)),
                block,
                @as(Block, @splat(0)),
            );
            const words: WordBlock = @bitCast(key_bytes);
            pos.* = start + channel_length + 1;
            return .{
                .path = data[start .. start + channel_length],
                .signature = mixChannelSignature(@reduce(.Xor, words), channel_length),
            };
        }

        var channel_length: usize = @sizeOf(PackedChannelKey);
        while (data[start + channel_length] != ',') : (channel_length += 1) {}
        const words: WordBlock = @bitCast(block);
        pos.* = start + channel_length + 1;
        return .{
            .path = data[start .. start + channel_length],
            .signature = mixChannelSignature(@reduce(.Xor, words), channel_length),
        };
    }

    var key_bytes: [@sizeOf(PackedChannelKey)]u8 = @splat(0);
    var channel_length: usize = 0;
    while (data[start + channel_length] != ',') : (channel_length += 1) {
        if (channel_length < key_bytes.len) key_bytes[channel_length] = data[start + channel_length];
    }
    pos.* = start + channel_length + 1;
    return .{
        .path = data[start .. start + channel_length],
        .signature = channelSignature(@bitCast(key_bytes), channel_length),
    };
}

inline fn packedChannelKey(channel_path: []const u8) PackedChannelKey {
    var key_bytes: [@sizeOf(PackedChannelKey)]u8 = @splat(0);
    const copy_length = @min(channel_path.len, key_bytes.len);
    @memcpy(key_bytes[0..copy_length], channel_path[0..copy_length]);
    return @bitCast(key_bytes);
}

const PendingStatsUpdate = struct {
    stats: *Stats,
    message_length: u16,
    stamp_count: u32,
};

const ParsedMessage = struct {
    month_index: u4,
    channel: ParsedChannel,
    message_length: u16,
    stamp_count: u32,
};

const ParsedFrozenMessage = struct {
    month_index: u4,
    channel: ParsedFrozenChannel,
    message_length: u16,
    stamp_count: u32,
};

inline fn parseMessage(data_bytes: []const u8, pos: *usize) ParsedMessage {
    const month_index = parseMonth2027(data_bytes, pos);
    const channel = parseChannel(data_bytes, pos);
    const message_length = parseMessageLength(data_bytes, pos);
    const stamp_count = parseStampCount(data_bytes, pos);
    return .{
        .month_index = month_index,
        .channel = channel,
        .message_length = message_length,
        .stamp_count = stamp_count,
    };
}

inline fn parseFrozenMessage(data_bytes: []const u8, pos: *usize) ParsedFrozenMessage {
    const month_index = parseMonth2027(data_bytes, pos);
    const channel = parseFrozenChannel(data_bytes, pos);
    const message_length = parseMessageLength(data_bytes, pos);
    const stamp_count = parseStampCount(data_bytes, pos);
    return .{
        .month_index = month_index,
        .channel = channel,
        .message_length = message_length,
        .stamp_count = stamp_count,
    };
}

inline fn applyStatsUpdate(update: *const PendingStatsUpdate) void {
    const stats = update.stats;
    const message_length = update.message_length;
    var values: StatsVector = @bitCast(stats.*);
    values += StatsVector{
        message_length,
        1,
        update.stamp_count,
        0,
    };
    var half_words: @Vector(8, u16) = @bitCast(values);
    const message_lengths: @Vector(8, u16) = @splat(message_length);
    half_words = @select(
        u16,
        @Vector(8, bool){ false, false, false, false, false, false, true, false },
        @min(half_words, message_lengths),
        half_words,
    );
    half_words = @select(
        u16,
        @Vector(8, bool){ false, false, false, false, false, false, false, true },
        @max(half_words, message_lengths),
        half_words,
    );
    stats.* = @bitCast(half_words);
}

const StatsUpdateQueue = struct {
    const capacity = 8;

    updates: [capacity]PendingStatsUpdate = undefined,
    next_index: usize = 0,
    length: usize = 0,

    inline fn push(self: *StatsUpdateQueue, stats: *Stats, message_length: u16, stamp_count: u32) void {
        @prefetch(stats, .{ .rw = .write, .locality = 3 });
        if (self.length == capacity) {
            applyStatsUpdate(&self.updates[self.next_index]);
        } else {
            self.length += 1;
        }
        self.updates[self.next_index] = .{
            .stats = stats,
            .message_length = message_length,
            .stamp_count = stamp_count,
        };
        self.next_index = (self.next_index + 1) & (capacity - 1);
    }

    fn flush(self: *StatsUpdateQueue) void {
        var index: usize = if (self.length == capacity) self.next_index else 0;
        for (0..self.length) |_| {
            applyStatsUpdate(&self.updates[index]);
            index = (index + 1) & (capacity - 1);
        }
    }
};

fn processChunk(data_bytes: []const u8, allocator: std.mem.Allocator) !ChannelMap {
    var data: ChannelMap = .{};
    var hot_stats_cache: HotStatsCache = .{};
    var pending: StatsUpdateQueue = .{};
    var pos: usize = 0;
    var frozen = false;

    while (pos < data_bytes.len) {
        const message = parseMessage(data_bytes, &pos);
        const channel_path = message.channel.path;
        const channel_key = message.channel.key;
        var became_frozen = false;
        const year_stats = hot_stats_cache.getDiscovery(channel_path, channel_key) orelse year_stats: {
            const result = try data.getOrPut(allocator, channel_path, channel_key);
            if (data.count == ChannelMap.max_channel_count and !hot_stats_cache.freeze_attempted) {
                became_frozen = hot_stats_cache.freeze(data.records[0..data.count], &data.stats[0]);
            }
            if (became_frozen) {
                hot_stats_cache.putFrozen(channelSignature(channel_key, channel_path.len), result);
            } else {
                hot_stats_cache.putDiscovery(channel_path, channel_key, result);
            }
            break :year_stats result;
        };
        pending.push(
            &year_stats.months[message.month_index],
            message.message_length,
            message.stamp_count,
        );
        if (became_frozen) {
            frozen = true;
            break;
        }
    }

    if (frozen) {
        while (pos < data_bytes.len) {
            const message = parseFrozenMessage(data_bytes, &pos);
            const channel_path = message.channel.path;
            const signature = message.channel.signature;
            const year_stats = hot_stats_cache.getFrozen(signature) orelse year_stats: {
                const channel_key = packedChannelKey(channel_path);
                const result = try data.getOrPut(allocator, channel_path, channel_key);
                hot_stats_cache.putFrozen(signature, result);
                break :year_stats result;
            };
            pending.push(
                &year_stats.months[message.month_index],
                message.message_length,
                message.stamp_count,
            );
        }
    }

    pending.flush();
    return data;
}

const WorkerResult = struct {
    arena: std.heap.ArenaAllocator,
    data: ChannelMap = .{},
    err: ?anyerror = null,
};

fn runWorker(result: *WorkerResult, data_bytes: []const u8) void {
    result.data = processChunk(data_bytes, result.arena.allocator()) catch |err| {
        result.err = err;
        return;
    };
}

fn chunkBoundaries(
    data_bytes: []const u8,
    thread_count: usize,
    allocator: std.mem.Allocator,
) ![]usize {
    const boundaries = try allocator.alloc(usize, thread_count + 1);
    boundaries[0] = 0;
    boundaries[thread_count] = data_bytes.len;

    for (1..thread_count) |i| {
        const nominal = data_bytes.len / thread_count * i;
        if (data_bytes[nominal - 1] == '\n') {
            boundaries[i] = nominal;
        } else if (std.mem.indexOfScalarPos(u8, data_bytes, nominal, '\n')) |newline| {
            boundaries[i] = newline + 1;
        } else {
            boundaries[i] = data_bytes.len;
        }
    }
    return boundaries;
}

inline fn mergeStats(dst: *Stats, src: *const Stats) void {
    var values: StatsVector = @bitCast(dst.*);
    const src_values: StatsVector = @bitCast(src.*);
    const min_length = @min(
        @as(u16, @truncate(values[3])),
        @as(u16, @truncate(src_values[3])),
    );
    const max_length = @max(
        @as(u16, @truncate(values[3] >> 16)),
        @as(u16, @truncate(src_values[3] >> 16)),
    );
    values += StatsVector{ src_values[0], src_values[1], src_values[2], 0 };
    values[3] = @as(u32, min_length) | (@as(u32, max_length) << 16);
    dst.* = @bitCast(values);
}

fn mergeYearStats(
    dst: *YearStats,
    src: *const YearStats,
) void {
    for (0..src.months.len) |month_index| {
        if (src.months[month_index].message_count != 0) {
            mergeStats(&dst.months[month_index], &src.months[month_index]);
        }
    }
}

fn mergeChannelMap(dst: *ChannelMap, allocator: std.mem.Allocator, src: *const ChannelMap) !void {
    for (src.records[0..src.count], 0..) |record, stats_index| {
        const dst_stats = try dst.getOrPut(
            allocator,
            src.channelPath(stats_index),
            record.key,
        );
        mergeYearStats(dst_stats, &src.stats[stats_index]);
    }
}

fn writeOutput(out: *Io.Writer, data: *const ChannelMap) !void {
    for (0..data.count) |stats_index| {
        const channel_path = data.channelPath(stats_index);
        try writeYearStats(out, channel_path, fixed_year, &data.stats[stats_index]);
    }
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const arena = init.arena.allocator();

    // ./program input.csv output.txt
    const args = try init.minimal.args.toSlice(arena);
    if (args.len != 3) {
        std.debug.print("usage: {s} <input.csv> <output.txt>\n", .{args[0]});
        std.process.exit(1);
    }

    const cwd = Io.Dir.cwd();

    // 入力はmmapでプロセスのアドレス空間に直接マップする。
    // readと違いバッファへのコピーが不要で、ファイル全体を1つの[]const u8として
    // 扱えるため、後の並列化でチャンク分割しやすい。
    const input_file = try cwd.openFile(io, args[1], .{});
    defer input_file.close(io);
    const input_size: usize = @intCast((try input_file.stat(io)).size);
    if (input_size == 0) return error.EmptyInput;
    const mapped = try std.posix.mmap(
        null,
        input_size,
        .{ .READ = true },
        .{ .TYPE = .PRIVATE },
        input_file.handle,
        0,
    );
    defer std.posix.munmap(mapped);
    const data_bytes: []const u8 = mapped;

    const output_file = try cwd.createFile(io, args[2], .{});
    defer output_file.close(io);
    var output_buffer: [64 * 1024]u8 = undefined;
    var output_writer: Io.File.Writer = .init(output_file, io, &output_buffer);
    const out = &output_writer.interface;

    // 1行目はヘッダ行なので読み飛ばす
    const body_start = (std.mem.indexOfScalar(u8, data_bytes, '\n') orelse return error.EmptyInput) + 1;
    const body = data_bytes[body_start..];
    const cpu_count = std.Thread.getCpuCount() catch 1;
    const max_useful_threads = @max(@as(usize, 1), body.len / (1024 * 1024));
    const thread_count = if (config.threads == 0)
        @max(@as(usize, 1), @min(@min(cpu_count, 8), max_useful_threads))
    else
        @max(@as(usize, 1), @min(config.threads, body.len));

    if (thread_count == 1) {
        const data = try processChunk(body, arena);
        try writeOutput(out, &data);
    } else {
        const boundaries = try chunkBoundaries(body, thread_count, arena);
        const results = try arena.alloc(WorkerResult, thread_count);
        for (results) |*result| {
            result.* = .{ .arena = std.heap.ArenaAllocator.init(init.gpa) };
        }
        defer for (results) |*result| result.arena.deinit();

        const threads = try arena.alloc(std.Thread, thread_count - 1);
        var spawned: usize = 0;
        errdefer {
            for (threads[0..spawned]) |thread| thread.join();
        }
        for (1..thread_count) |i| {
            threads[i - 1] = try std.Thread.spawn(
                .{ .stack_size = 1024 * 1024, .allocator = init.gpa },
                runWorker,
                .{ &results[i], body[boundaries[i]..boundaries[i + 1]] },
            );
            spawned += 1;
        }

        runWorker(&results[0], body[boundaries[0]..boundaries[1]]);
        for (threads) |thread| thread.join();
        spawned = 0;

        for (results) |result| {
            if (result.err) |err| return err;
        }

        var data = results[0].data;
        const merge_allocator = results[0].arena.allocator();
        for (results[1..]) |*result| {
            try mergeChannelMap(&data, merge_allocator, &result.data);
        }
        try writeOutput(out, &data);
    }

    try out.flush();
}
