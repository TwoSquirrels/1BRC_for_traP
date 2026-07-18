//! コンテストへの提出。ソースと実行ファイルを multipart/form-data で POST する。
//! アクセスキーは環境変数 ONEBRC_ACCESS_KEY から読む (.env の読み込みは mise が担う)。
const std = @import("std");

const api_url = "https://1brc.trap.games/api/v1/submissions";
const max_upload = 64 * 1024 * 1024;

const Options = struct {
    source: []const u8,
    binary: []const u8,
    language: []const u8 = "c",
    dry_run: bool = false,
};

fn parseOptions(args: []const [:0]const u8) ?Options {
    var opts: Options = undefined;
    opts.language = "c";
    opts.dry_run = false;
    var seen: u8 = 0;
    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        const flag = args[i];
        if (std.mem.eql(u8, flag, "--dry-run")) {
            opts.dry_run = true;
            continue;
        }
        if (i + 1 >= args.len) return null;
        i += 1;
        const value = args[i];
        if (std.mem.eql(u8, flag, "--source")) {
            opts.source = value;
            seen |= 1;
        } else if (std.mem.eql(u8, flag, "--binary")) {
            opts.binary = value;
            seen |= 2;
        } else if (std.mem.eql(u8, flag, "--language")) {
            opts.language = value;
        } else {
            return null;
        }
    }
    if (seen != 3) return null;
    return opts;
}

fn appendPart(w: *std.Io.Writer, boundary: []const u8, name: []const u8, filename: ?[]const u8, content: []const u8) !void {
    try w.print("--{s}\r\nContent-Disposition: form-data; name=\"{s}\"", .{ boundary, name });
    if (filename) |f| try w.print("; filename=\"{s}\"\r\nContent-Type: application/octet-stream", .{f});
    try w.print("\r\n\r\n{s}\r\n", .{content});
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    const opts = parseOptions(args) orelse {
        std.debug.print("usage: {s} --source <main.c> --binary <program> [--language c] [--dry-run]\n", .{args[0]});
        return 2;
    };

    const cwd = std.Io.Dir.cwd();
    const source = try cwd.readFileAlloc(io, opts.source, alloc, .limited(max_upload));
    const binary = try cwd.readFileAlloc(io, opts.binary, alloc, .limited(max_upload));

    if (opts.dry_run) {
        std.debug.print(
            "dry-run: {s} へ提出する内容:\n  source: {s} ({d} bytes)\n  binary: {s} ({d} bytes)\n  language: {s}\n",
            .{ api_url, opts.source, source.len, opts.binary, binary.len, opts.language },
        );
        return 0;
    }

    const access_key = init.environ_map.get("ONEBRC_ACCESS_KEY") orelse "";
    if (access_key.len == 0) {
        std.debug.print("環境変数 ONEBRC_ACCESS_KEY がありません (.env は mise が読み込むので、mise 経由で実行してください)\n", .{});
        return 1;
    }

    // boundary は本文に現れなければよいので、時刻ベース + 衝突時は変化させて再生成
    var nonce: u64 = @truncate(@as(u96, @bitCast(std.Io.Timestamp.now(io, .real).toNanoseconds())));
    const boundary = while (true) : (nonce +%= 0x9e3779b97f4a7c15) {
        const candidate = try std.fmt.allocPrint(alloc, "zig1brc{x:0>16}", .{nonce});
        if (std.mem.indexOf(u8, binary, candidate) == null and std.mem.indexOf(u8, source, candidate) == null) {
            break candidate;
        }
    } else unreachable;

    var payload: std.Io.Writer.Allocating = .init(alloc);
    const w = &payload.writer;
    try appendPart(w, boundary, "executionKind", null, "native");
    try appendPart(w, boundary, "language", null, opts.language);
    try appendPart(w, boundary, "binary", std.fs.path.basename(opts.binary), binary);
    try appendPart(w, boundary, "source", std.fs.path.basename(opts.source), source);
    try w.print("--{s}--\r\n", .{boundary});

    var client: std.http.Client = .{ .allocator = alloc, .io = io };
    defer client.deinit();

    std.debug.print("{s} へ提出しています... (source {d} bytes, binary {d} bytes)\n", .{ api_url, source.len, binary.len });
    var body: std.Io.Writer.Allocating = .init(alloc);
    const result = client.fetch(.{
        .location = .{ .url = api_url },
        .method = .POST,
        .payload = payload.written(),
        .response_writer = &body.writer,
        .headers = .{
            .authorization = .{ .override = try std.fmt.allocPrint(alloc, "Bearer {s}", .{access_key}) },
            .content_type = .{ .override = try std.fmt.allocPrint(alloc, "multipart/form-data; boundary={s}", .{boundary}) },
        },
    }) catch |err| switch (err) {
        // サーバーが本文を最後まで受け取らずに切断すると応答を読めずここに来る。
        // 実測ではアクセスキー拒否 (401 相当) のときにこうなる。
        error.WriteFailed => {
            std.debug.print("送信中に接続が打ち切られました。アクセスキーが拒否された可能性が高いです。\n" ++
                "詳細な応答が必要なら contest.md 記載の curl コマンドでお確かめください。\n", .{});
            return 1;
        },
        else => return err,
    };

    const ok = result.status.class() == .success;
    std.debug.print("HTTP {d}\n{s}\n", .{ @intFromEnum(result.status), body.written() });
    if (ok) std.debug.print("提出完了です。mise run leaderboard で順位を確認できます。\n", .{});
    return if (ok) 0 else 1;
}
