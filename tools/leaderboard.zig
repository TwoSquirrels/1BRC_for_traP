//! コンテストのリーダーボードを取得して表形式で表示する。
const std = @import("std");

const api_url = "https://1brc.trap.games/api/v1/leaderboard?board=";

const Entry = struct {
    rank: u32,
    username: []const u8,
    language: []const u8,
    scoreNs: []const u8,
    verdict: []const u8,
    submittedAt: []const u8,
};

const Leaderboard = struct {
    board: []const u8,
    ranked: []Entry,
};

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len > 2) {
        std.debug.print("usage: {s} [board (既定: public)]\n", .{args[0]});
        return 2;
    }
    const board = if (args.len == 2) args[1] else "public";

    var client: std.http.Client = .{ .allocator = alloc, .io = io };
    defer client.deinit();

    // サーバーが間欠的に 500 を返すことがあるため、5xx は数回まで再試行する
    const max_attempts = 3;
    var body: std.Io.Writer.Allocating = .init(alloc);
    var attempt: usize = 1;
    while (true) : (attempt += 1) {
        body.clearRetainingCapacity();
        const result = try client.fetch(.{
            .location = .{ .url = try std.mem.concat(alloc, u8, &.{ api_url, board }) },
            .response_writer = &body.writer,
        });
        if (result.status == .ok) break;
        if (result.status.class() == .server_error and attempt < max_attempts) {
            std.debug.print("HTTP {d} が返りました。再試行します ({d}/{d})...\n", .{ @intFromEnum(result.status), attempt, max_attempts });
            continue;
        }
        std.debug.print("取得失敗: HTTP {d}\n{s}\n", .{ @intFromEnum(result.status), body.written() });
        return 1;
    }

    const parsed = try std.json.parseFromSlice(Leaderboard, alloc, body.written(), .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    std.debug.print("=== {s} leaderboard ===\n", .{parsed.value.board});
    for (parsed.value.ranked) |entry| {
        const ns = std.fmt.parseInt(u64, entry.scoreNs, 10) catch 0;
        const secs = @as(f64, @floatFromInt(ns)) / std.time.ns_per_s;
        std.debug.print("{d:>3}. {d:>8.3}s  {s:<20} ({s}, {s})\n", .{
            entry.rank, secs, entry.username, entry.language, entry.submittedAt[0..10],
        });
    }
    return 0;
}
