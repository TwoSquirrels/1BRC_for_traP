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

/// "2026-07-18T18:49:12.345Z" のような ISO 8601 UTC を「7/19 03:49」の JST 表示にする。
/// Web UI の表示に合わせる。パースできなければそのまま返す。
fn formatJst(alloc: std.mem.Allocator, iso: []const u8) []const u8 {
    if (iso.len < 16 or iso[4] != '-' or iso[7] != '-' or iso[10] != 'T') return iso;
    const parse = std.fmt.parseInt;
    const year = parse(u16, iso[0..4], 10) catch return iso;
    const month = parse(u8, iso[5..7], 10) catch return iso;
    const day = parse(u8, iso[8..10], 10) catch return iso;
    const hour = parse(u8, iso[11..13], 10) catch return iso;
    const minute = parse(u8, iso[14..16], 10) catch return iso;

    // JST = UTC+9。日付繰り上がりは月末日数を見て処理する (コンテスト期間中に年跨ぎはない)
    var jst_hour = hour + 9;
    var jst_day = day;
    var jst_month = month;
    if (jst_hour >= 24) {
        jst_hour -= 24;
        jst_day += 1;
        const leap = (year % 4 == 0 and year % 100 != 0) or year % 400 == 0;
        const month_days = [12]u8{ 31, if (leap) 29 else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (jst_day > month_days[jst_month - 1]) {
            jst_day = 1;
            jst_month = if (jst_month == 12) 1 else jst_month + 1;
        }
    }
    return std.fmt.allocPrint(alloc, "{d}/{d:0>2} {d:0>2}:{d:0>2}", .{ jst_month, jst_day, jst_hour, minute }) catch iso;
}

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
            entry.rank, secs, entry.username, entry.language, formatJst(alloc, entry.submittedAt),
        });
    }
    return 0;
}
