//! perf によるサンプリングプロファイル。WSL2 では HW カウンタ (cycles 等) が使えないため
//! 常に cpu-clock を指定する。時間の内訳を見る用途にはそれで十分。
//! Ubuntu の /usr/bin/perf はカーネル版数一致を要求するラッパーで、WSL2 のカーネル更新で
//! 動かなくなる (実績あり)。その場合は /usr/lib/linux-tools-*/perf の実体を探して使う。
const std = @import("std");

fn run(io: std.Io, argv: []const []const u8, stderr: std.process.SpawnOptions.StdIo) !u32 {
    var child = try std.process.spawn(io, .{ .argv = argv, .stderr = stderr });
    return switch (try child.wait(io)) {
        .exited => |code| code,
        else => 255,
    };
}

fn works(io: std.Io, path: []const u8) bool {
    // ラッパーの WARNING スパムを避けるため、動作確認の stderr は捨てる
    const code = run(io, &.{ path, "--version" }, .ignore) catch return false;
    return code == 0;
}

fn findPerf(io: std.Io, alloc: std.mem.Allocator) ?[]const u8 {
    if (works(io, "perf")) return "perf";
    const dir = std.Io.Dir.openDirAbsolute(io, "/usr/lib", .{ .iterate = true }) catch return null;
    var it = dir.iterate();
    var best: ?[]const u8 = null;
    while (it.next(io) catch null) |entry| {
        if (entry.kind != .directory or !std.mem.startsWith(u8, entry.name, "linux-tools-")) continue;
        const candidate = std.fmt.allocPrint(alloc, "/usr/lib/{s}/perf", .{entry.name}) catch return best;
        if (!works(io, candidate)) continue;
        // 複数あれば名前の大きいもの (新しい版) を選ぶ
        if (best == null or std.mem.order(u8, candidate, best.?) == .gt) best = candidate;
    }
    return best;
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len != 3) {
        std.debug.print("usage: {s} <binary> <input.csv>\n", .{args[0]});
        return 2;
    }

    const perf = findPerf(io, alloc) orelse {
        std.debug.print(
            \\動く perf が見つかりません。次でインストールできます:
            \\  sudo apt install linux-tools-common linux-tools-generic
            \\(/usr/bin/perf のラッパーが弾く場合も /usr/lib/linux-tools-*/perf を探索済みです)
            \\
        , .{});
        return 1;
    };
    std.debug.print("perf: {s}\n", .{perf});

    try std.Io.Dir.cwd().createDirPath(io, "work");
    const record = [_][]const u8{
        perf,     "record", "-e", "cpu-clock", "-F",           "999",   "-g",
        "-o",     "work/perf.data", "--", args[1], args[2], "work/perf-out.txt",
    };
    if (try run(io, &record, .inherit) != 0) return 1;

    std.debug.print("\n", .{});
    _ = try run(io, &.{ perf, "report", "--stdio", "-i", "work/perf.data", "--percent-limit", "1" }, .inherit);
    std.debug.print(
        \\
        \\対話表示: {s} report -i work/perf.data
        \\命令単位: {s} annotate -i work/perf.data
        \\
    , .{ perf, perf });
    return 0;
}
