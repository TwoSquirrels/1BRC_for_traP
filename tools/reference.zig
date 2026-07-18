//! 遅いが自明に正しい参照実装。エッジケースの期待出力生成と仕様確認に使う。
const std = @import("std");

const Stats = struct {
    min: i64,
    max: i64,
    total: i64,
    count: i64,
    stamps: i64,
};

/// C の printf("%.2f") と同じ結果になるよう、double の正確な 2 進値を
/// 小数第 2 位へ丸めて (端数 0.5 ちょうどは偶数丸め) 出力する。
/// Zig の {d:.2} は十進の最短表現経由で丸めるため結果がずれることがある
/// (例: 170.975 の 2 進値は 170.9749... なので printf は 170.97、{d:.2} は 170.98)。
fn writeAvg(out: *std.Io.Writer, total: i64, count: i64) !void {
    const avg = @as(f64, @floatFromInt(total)) / @as(f64, @floatFromInt(count));
    const bits: u64 = @bitCast(avg);
    std.debug.assert(bits >> 63 == 0); // メッセージ長は非負
    const biased_exp: i32 = @intCast((bits >> 52) & 0x7ff);
    const fraction: u64 = bits & ((@as(u64, 1) << 52) - 1);
    const mantissa: u128 = if (biased_exp == 0) fraction else fraction | (@as(u64, 1) << 52);
    const exp: i32 = if (biased_exp == 0) -1074 else biased_exp - 1075;

    // avg * 100 * 2^exp を丸めて 1/100 単位の整数にする
    const scaled: u128 = mantissa * 100;
    var hundredths: u128 = undefined;
    if (exp >= 0) {
        hundredths = scaled << @intCast(exp);
    } else if (-exp > 126) {
        hundredths = 0; // 集計上ありえない極小値だが 0.00 に丸めておく
    } else {
        const shift: u7 = @intCast(-exp);
        const divisor = @as(u128, 1) << shift;
        const quotient = scaled >> shift;
        const remainder = scaled & (divisor - 1);
        hundredths = quotient;
        if (remainder * 2 > divisor or (remainder * 2 == divisor and quotient % 2 == 1)) {
            hundredths += 1;
        }
    }
    try out.print("{d}.{d:0>2}", .{ hundredths / 100, hundredths % 100 });
}

/// Howard Hinnant の civil_from_days による Unix 秒 → UTC 年月変換
fn yearMonthOf(ts: i64) struct { year: u16, month: u8 } {
    const z = @divFloor(ts, 86400) + 719468;
    const era = @divFloor(z, 146097);
    const doe: u64 = @intCast(z - era * 146097);
    const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const mp = (5 * doy + 2) / 153;
    const month: u8 = @intCast(if (mp < 10) mp + 3 else mp - 9);
    const year = @as(i64, @intCast(yoe)) + era * 400 + @intFromBool(month <= 2);
    return .{ .year = @intCast(year), .month = month };
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len != 3) {
        std.debug.print("usage: {s} <input.csv> <output.txt>\n", .{args[0]});
        return 2;
    }

    const data = try std.Io.Dir.cwd().readFileAlloc(io, args[1], alloc, .unlimited);
    var map: std.StringArrayHashMapUnmanaged(Stats) = .empty;

    var it = std.mem.splitScalar(u8, data, '\n');
    _ = it.next(); // ヘッダ行
    while (it.next()) |raw| {
        const line = std.mem.trimEnd(u8, raw, "\r");
        if (line.len == 0) continue;
        var fields = std.mem.splitScalar(u8, line, ',');
        const ts = try std.fmt.parseInt(i64, fields.next().?, 10);
        const channel = fields.next().?;
        const length = try std.fmt.parseInt(i64, fields.next().?, 10);
        const stamps = try std.fmt.parseInt(i64, fields.next().?, 10);

        const ym = yearMonthOf(ts);
        const key = try std.fmt.allocPrint(alloc, "{s},{d:0>4}-{d:0>2}", .{ channel, ym.year, ym.month });
        const gop = try map.getOrPut(alloc, key);
        if (!gop.found_existing) {
            gop.value_ptr.* = .{ .min = length, .max = length, .total = length, .count = 1, .stamps = stamps };
        } else {
            const s = gop.value_ptr;
            s.min = @min(s.min, length);
            s.max = @max(s.max, length);
            s.total += length;
            s.count += 1;
            s.stamps += stamps;
        }
    }

    const out_file = try std.Io.Dir.cwd().createFile(io, args[2], .{});
    defer out_file.close(io);
    var buffer: [1 << 16]u8 = undefined;
    var writer = out_file.writer(io, &buffer);
    const out = &writer.interface;
    for (map.keys(), map.values()) |key, s| {
        try out.print("{s}={d}/", .{ key, s.min });
        try writeAvg(out, s.total, s.count);
        try out.print("/{d}/{d}/{d}\n", .{ s.max, s.count, s.stamps });
    }
    try out.flush();
    return 0;
}
