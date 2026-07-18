//! perf によるサンプリングプロファイル。WSL2 では HW カウンタ (cycles 等) は使えず
//! cpu-clock フォールバックになるが、時間の内訳を見る用途にはそれで十分。
const std = @import("std");

fn run(io: std.Io, argv: []const []const u8) !u32 {
    var child = try std.process.spawn(io, .{ .argv = argv });
    return switch (try child.wait(io)) {
        .exited => |code| code,
        else => 255,
    };
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len != 3) {
        std.debug.print("usage: {s} <binary> <input.csv>\n", .{args[0]});
        return 2;
    }

    _ = run(io, &.{ "perf", "--version" }) catch {
        std.debug.print(
            \\perf が見つかりません。次でインストールできます:
            \\  sudo apt install linux-tools-common linux-tools-generic
            \\WSL2 でカーネル版が合わない場合は /usr/lib/linux-tools/*/perf を直接使うか
            \\generic 版で動きます。
            \\
        , .{});
        return 1;
    };

    try std.Io.Dir.cwd().createDirPath(io, "work");
    const record = [_][]const u8{
        "perf", "record", "-F", "999", "-g", "-o", "work/perf.data", "--", args[1], args[2], "work/perf-out.txt",
    };
    if (try run(io, &record) != 0) return 1;

    std.debug.print("\n", .{});
    _ = try run(io, &.{ "perf", "report", "--stdio", "-i", "work/perf.data", "--percent-limit", "1" });
    std.debug.print(
        \\
        \\対話表示: perf report -i work/perf.data
        \\命令単位: perf annotate -i work/perf.data
        \\
    , .{});
    return 0;
}
