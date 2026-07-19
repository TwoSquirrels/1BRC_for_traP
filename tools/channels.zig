// 100m の生データからチャンネルの分布を実測する一回きりの調査ツール。
// working set 仮説 (1B ではアクティブチャンネルが増えて集計表が溢れる) の当否を確かめる。
// 出力: 異なるチャンネル数、名前長の分布、頻度上位が全行に占める累積シェア。
const std = @import("std");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const a = init.arena.allocator();
    const args = try init.minimal.args.toSlice(a);
    if (args.len != 2) {
        std.debug.print("usage: {s} <input.csv>\n", .{args[0]});
        return 2;
    }

    const data = try std.Io.Dir.cwd().readFileAlloc(io, args[1], a, .unlimited);

    var counts: std.StringHashMapUnmanaged(u64) = .empty;
    try counts.ensureTotalCapacity(a, 16384);

    var len_hist = [_]u64{0} ** 128;
    var rows: u64 = 0;

    var i: usize = std.mem.indexOfScalar(u8, data, '\n').? + 1;
    while (i < data.len) {
        const c1 = std.mem.indexOfScalarPos(u8, data, i, ',').?;
        const c2 = std.mem.indexOfScalarPos(u8, data, c1 + 1, ',').?;
        const name = data[c1 + 1 .. c2];
        const gop = try counts.getOrPut(a, name);
        if (!gop.found_existing) gop.value_ptr.* = 0;
        gop.value_ptr.* += 1;
        len_hist[name.len] += 1;
        rows += 1;
        i = std.mem.indexOfScalarPos(u8, data, c2 + 1, '\n').? + 1;
    }

    const Freq = struct { name: []const u8, n: u64 };
    var list = try a.alloc(Freq, counts.count());
    var it = counts.iterator();
    var k: usize = 0;
    while (it.next()) |e| : (k += 1) list[k] = .{ .name = e.key_ptr.*, .n = e.value_ptr.* };
    std.mem.sort(Freq, list, {}, struct {
        fn lt(_: void, x: Freq, y: Freq) bool {
            return x.n > y.n;
        }
    }.lt);

    const p = std.debug.print;
    p("rows            {d}\n", .{rows});
    p("distinct chans  {d}\n", .{list.len});

    const stats_bytes = list.len * 12 * 24;
    p("stats footprint {d} bytes ({d:.2} MB) 全 12 月ぶん確保\n", .{ stats_bytes, @as(f64, @floatFromInt(stats_bytes)) / 1e6 });

    const marks = [_]usize{ 16, 64, 256, 1024, 4096 };
    var cum: u64 = 0;
    var mi: usize = 0;
    for (list, 0..) |f, idx| {
        cum += f.n;
        if (mi < marks.len and idx + 1 == marks[mi]) {
            p("top {d:>5} chans {d:.2}% of rows\n", .{ marks[mi], 100.0 * @as(f64, @floatFromInt(cum)) / @as(f64, @floatFromInt(rows)) });
            mi += 1;
        }
    }
    p("--- name length histogram (行数) ---\n", .{});
    for (len_hist, 0..) |cnt, L| {
        if (cnt > 0) p("len {d:>3}: {d}\n", .{ L, cnt });
    }
    return 0;
}
