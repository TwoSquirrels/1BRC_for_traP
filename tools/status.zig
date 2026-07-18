//! 自分の提出の判定状況を表示する。一覧 (既定) または --id で単体を表示する。
//! アクセスキーは環境変数 ONEBRC_ACCESS_KEY から読む (.env の読み込みは mise が担う)。
//! 注意: API は ?limit= を無視して全件返す (実測) ので、件数はクライアント側で絞る。
const std = @import("std");
const formatJst = @import("jst.zig").formatJst;

const api_url = "https://1brc.trap.games/api/v1/submissions";

const Public = struct {
    verdict: ?[]const u8 = null,
    scoreNs: ?[]const u8 = null,
    @"error": ?[]const u8 = null,
};

const Submission = struct {
    id: []const u8,
    sourceFilename: []const u8 = "?",
    status: []const u8 = "?",
    public: ?Public = null,
    infrastructureError: ?[]const u8 = null,
    disqualifiedReason: ?[]const u8 = null,
    uploadStartedAt: []const u8 = "",
    submissionNumber: ?u32 = null,
};

fn fetchJson(io: std.Io, alloc: std.mem.Allocator, url: []const u8, key: []const u8) ![]const u8 {
    var client: std.http.Client = .{ .allocator = alloc, .io = io };
    defer client.deinit();

    // サーバーが間欠的に 5xx を返すことがあるため数回まで再試行する
    const max_attempts = 3;
    var body: std.Io.Writer.Allocating = .init(alloc);
    var attempt: usize = 1;
    while (true) : (attempt += 1) {
        body.clearRetainingCapacity();
        const result = try client.fetch(.{
            .location = .{ .url = url },
            .response_writer = &body.writer,
            .headers = .{
                .authorization = .{ .override = try std.fmt.allocPrint(alloc, "Bearer {s}", .{key}) },
            },
        });
        if (result.status == .ok) return body.written();
        if (result.status.class() == .server_error and attempt < max_attempts) {
            std.debug.print("HTTP {d} が返りました。再試行します ({d}/{d})...\n", .{ @intFromEnum(result.status), attempt, max_attempts });
            continue;
        }
        // 失敗の本文は原因特定に必須なので全文を出す
        std.debug.print("取得失敗: HTTP {d}\n{s}\n", .{ @intFromEnum(result.status), body.written() });
        return error.FetchFailed;
    }
}

fn printSubmission(alloc: std.mem.Allocator, s: Submission) void {
    const number = s.submissionNumber orelse 0;
    const jst = formatJst(alloc, s.uploadStartedAt);
    if (s.public) |p| {
        if (p.verdict != null and p.scoreNs != null) {
            const ns = std.fmt.parseInt(u64, p.scoreNs.?, 10) catch 0;
            const secs = @as(f64, @floatFromInt(ns)) / std.time.ns_per_s;
            std.debug.print("#{d:>2} {s:<24} {s:<10} {s:<9} {d:>8.3}s ({s})\n", .{
                number, s.sourceFilename, s.status, p.verdict.?, secs, jst,
            });
            if (p.@"error") |e| std.debug.print("     error: {s}\n", .{e});
            return;
        }
    }
    std.debug.print("#{d:>2} {s:<24} {s:<10} ({s})\n", .{ number, s.sourceFilename, s.status, jst });
    if (s.infrastructureError) |e| std.debug.print("     infrastructure_error: {s} (同じ提出物をそのまま再提出)\n", .{e});
    if (s.disqualifiedReason) |e| std.debug.print("     disqualified: {s}\n", .{e});
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    var limit: usize = 5;
    var id: ?[]const u8 = null;
    var i: usize = 1;
    while (i < args.len) : (i += 2) {
        if (i + 1 >= args.len) {
            std.debug.print("usage: {s} [--limit N] [--id <uuid>]\n", .{args[0]});
            return 2;
        }
        if (std.mem.eql(u8, args[i], "--limit")) {
            limit = std.fmt.parseInt(usize, args[i + 1], 10) catch 5;
        } else if (std.mem.eql(u8, args[i], "--id")) {
            id = args[i + 1];
        } else {
            std.debug.print("usage: {s} [--limit N] [--id <uuid>]\n", .{args[0]});
            return 2;
        }
    }

    const access_key = init.environ_map.get("ONEBRC_ACCESS_KEY") orelse "";
    if (access_key.len == 0) {
        std.debug.print("環境変数 ONEBRC_ACCESS_KEY がありません (.env は mise が読み込むので、mise 経由で実行してください)\n", .{});
        return 1;
    }

    if (id) |sub_id| {
        const url = try std.mem.concat(alloc, u8, &.{ api_url, "/", sub_id });
        const body = fetchJson(io, alloc, url, access_key) catch return 1;
        const parsed = try std.json.parseFromSlice(struct { submission: Submission }, alloc, body, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        printSubmission(alloc, parsed.value.submission);
        return 0;
    }

    const body = fetchJson(io, alloc, api_url, access_key) catch return 1;
    const parsed = try std.json.parseFromSlice(struct { submissions: []Submission }, alloc, body, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();
    const list = parsed.value.submissions;
    for (list[0..@min(limit, list.len)]) |s| printSubmission(alloc, s);
    return 0;
}
