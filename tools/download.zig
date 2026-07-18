//! 公開データセット (1m / 10m / 100m) をダウンロードして datasets/ に展開する。
//! 1B は展開後 26 GiB 前後になるため対象外 (contest.md でも運営が 100M までを推奨)。
const std = @import("std");

const base_url = "https://1brc.trap.games/api/v1/datasets";
const sizes = [_][]const u8{ "1m", "10m", "100m" };
const kinds = [_]struct { api: []const u8, suffix: []const u8 }{
    .{ .api = "input", .suffix = "input.csv" },
    .{ .api = "expected", .suffix = "expected.txt" },
};
const tmp_path = "datasets/download.zst.tmp";
const max_attempts = 3;

fn fetchToFile(io: std.Io, client: *std.http.Client, url: []const u8, path: []const u8) !void {
    const file = try std.Io.Dir.cwd().createFile(io, path, .{});
    defer file.close(io);
    var buffer: [1 << 16]u8 = undefined;
    var writer = file.writer(io, &buffer);

    var attempt: usize = 1;
    while (true) : (attempt += 1) {
        const result = try client.fetch(.{
            .location = .{ .url = url },
            .response_writer = &writer.interface,
        });
        if (result.status == .ok) break;
        if (result.status.class() == .server_error and attempt < max_attempts) {
            std.debug.print("HTTP {d} が返りました。再試行します ({d}/{d})...\n", .{ @intFromEnum(result.status), attempt, max_attempts });
            try file.setLength(io, 0);
            continue;
        }
        std.debug.print("取得失敗: HTTP {d} ({s})\n", .{ @intFromEnum(result.status), url });
        return error.DownloadFailed;
    }
    try writer.interface.flush();
}

fn decompressToFile(io: std.Io, alloc: std.mem.Allocator, src_path: []const u8, dest_path: []const u8) !u64 {
    const cwd = std.Io.Dir.cwd();
    const src = try cwd.openFile(io, src_path, .{});
    defer src.close(io);
    const dest = try cwd.createFile(io, dest_path, .{});
    defer dest.close(io);

    var read_buffer: [1 << 16]u8 = undefined;
    var reader = src.reader(io, &read_buffer);
    const window = try alloc.alloc(u8, std.compress.zstd.default_window_len + std.compress.zstd.block_size_max);
    defer alloc.free(window);
    var decompress: std.compress.zstd.Decompress = .init(&reader.interface, window, .{});

    var write_buffer: [1 << 16]u8 = undefined;
    var writer = dest.writer(io, &write_buffer);
    const n = try decompress.reader.streamRemaining(&writer.interface);
    try writer.interface.flush();
    return n;
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const cwd = std.Io.Dir.cwd();
    try cwd.createDirPath(io, "datasets");
    var client: std.http.Client = .{ .allocator = alloc, .io = io };
    defer client.deinit();

    var failed = false;
    for (sizes) |size| {
        for (kinds) |kind| {
            const dest = try std.fmt.allocPrint(alloc, "datasets/public-{s}.{s}", .{ size, kind.suffix });
            if (cwd.statFile(io, dest, .{})) |_| {
                std.debug.print("スキップ (取得済み): {s}\n", .{dest});
                continue;
            } else |_| {}

            const url = try std.fmt.allocPrint(alloc, "{s}/public-{s}/{s}/download", .{ base_url, size, kind.api });
            std.debug.print("ダウンロード中: {s}\n", .{url});
            fetchToFile(io, &client, url, tmp_path) catch {
                failed = true;
                continue;
            };
            const bytes = try decompressToFile(io, alloc, tmp_path, dest);
            std.debug.print("展開しました: {s} ({d} bytes)\n", .{ dest, bytes });
        }
    }
    cwd.deleteFile(io, tmp_path) catch {};
    return if (failed) 1 else 0;
}
