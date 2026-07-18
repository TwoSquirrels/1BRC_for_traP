//! コンテストの検証仕様 (行順・LF/CRLF・空行を無視、他は完全一致) で出力を比較する。
const std = @import("std");

const max_file_size = 1 << 30;
const max_shown_diffs = 10;

fn lineLessThan(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.order(u8, a, b) == .lt;
}

fn normalizedLines(io: std.Io, alloc: std.mem.Allocator, path: []const u8) ![][]const u8 {
    const data = std.Io.Dir.cwd().readFileAlloc(io, path, alloc, .limited(max_file_size)) catch |err| {
        std.debug.print("{s} を読めません: {t}\n", .{ path, err });
        return err;
    };
    var lines: std.ArrayList([]const u8) = .empty;
    var it = std.mem.splitScalar(u8, data, '\n');
    while (it.next()) |raw| {
        const line = std.mem.trimEnd(u8, raw, "\r");
        if (line.len != 0) try lines.append(alloc, line);
    }
    std.mem.sort([]const u8, lines.items, {}, lineLessThan);
    return lines.items;
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len != 3) {
        std.debug.print("usage: {s} <actual> <expected>\n", .{args[0]});
        return 2;
    }
    const actual = try normalizedLines(io, alloc, args[1]);
    const expected = try normalizedLines(io, alloc, args[2]);

    var equal = actual.len == expected.len;
    if (equal) for (actual, expected) |a, e| {
        if (!std.mem.eql(u8, a, e)) {
            equal = false;
            break;
        }
    };
    if (equal) {
        std.debug.print("OK: 出力は期待と一致しました\n", .{});
        return 0;
    }

    std.debug.print("NG: 出力が期待と一致しません\n", .{});
    std.debug.print("  行数: actual={d} expected={d}\n", .{ actual.len, expected.len });
    var i: usize = 0;
    var j: usize = 0;
    var shown: usize = 0;
    while ((i < actual.len or j < expected.len) and shown < max_shown_diffs) {
        if (j >= expected.len or (i < actual.len and lineLessThan({}, actual[i], expected[j]))) {
            std.debug.print("  actual のみ: {s}\n", .{actual[i]});
            i += 1;
            shown += 1;
        } else if (i >= actual.len or lineLessThan({}, expected[j], actual[i])) {
            std.debug.print("  expected のみ: {s}\n", .{expected[j]});
            j += 1;
            shown += 1;
        } else {
            i += 1;
            j += 1;
        }
    }
    return 1;
}
