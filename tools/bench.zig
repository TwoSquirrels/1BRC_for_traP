//! 本番の採点方式を再現した計測。新プロセスで複数回実行し、毎回検証して中央値を報告する。
//!
//! 本番環境はデータがページキャッシュに載った状態のため、計測前に入力を読んで温める。
//! ローカルが 8 vCPU を超える場合は taskset で本番と同じ 8 CPU に制限する。
const std = @import("std");

const contest_cpus = 8;
const default_runs = 3;
const default_log = "results/bench.jsonl";
const output_path = "work/bench-out.txt";

const Options = struct {
    bin: []const u8,
    verify: []const u8,
    input: []const u8,
    expected: []const u8,
    label: []const u8,
    // A/B 比較用のベースライン。指定時は run ごとに交互実行し、熱ドリフトを両者に均等に被せる
    bin2: ?[]const u8 = null,
    label2: ?[]const u8 = null,
    runs: usize = default_runs,
    log: []const u8 = default_log,
};

fn parseOptions(args: []const [:0]const u8) ?Options {
    // 必須フィールドは seen で全指定を強制するため undefined でよいが、
    // 省略可能なフィールドはここで初期化しないとゴミ値のままになる。
    var opts: Options = undefined;
    opts.bin2 = null;
    opts.label2 = null;
    opts.runs = default_runs;
    opts.log = default_log;
    var seen: u8 = 0;
    var i: usize = 1;
    while (i + 1 < args.len) : (i += 2) {
        const flag = args[i];
        const value = args[i + 1];
        if (std.mem.eql(u8, flag, "--bin")) {
            opts.bin = value;
            seen |= 1;
        } else if (std.mem.eql(u8, flag, "--verify")) {
            opts.verify = value;
            seen |= 2;
        } else if (std.mem.eql(u8, flag, "--input")) {
            opts.input = value;
            seen |= 4;
        } else if (std.mem.eql(u8, flag, "--expected")) {
            opts.expected = value;
            seen |= 8;
        } else if (std.mem.eql(u8, flag, "--label")) {
            opts.label = value;
            seen |= 16;
        } else if (std.mem.eql(u8, flag, "--bin2")) {
            opts.bin2 = value;
        } else if (std.mem.eql(u8, flag, "--label2")) {
            opts.label2 = value;
        } else if (std.mem.eql(u8, flag, "--runs")) {
            opts.runs = std.fmt.parseInt(usize, value, 10) catch return null;
        } else if (std.mem.eql(u8, flag, "--log")) {
            opts.log = value;
        } else {
            return null;
        }
    }
    if (seen != 31 or i != args.len or opts.runs == 0) return null;
    if ((opts.bin2 == null) != (opts.label2 == null)) return null;
    return opts;
}

fn warmPageCache(io: std.Io, path: []const u8) !void {
    const file = try std.Io.Dir.cwd().openFile(io, path, .{});
    defer file.close(io);
    var buffer: [1 << 20]u8 = undefined;
    var reader = file.reader(io, &buffer);
    _ = try reader.interface.discardRemaining();
}

fn runChild(io: std.Io, argv: []const []const u8, stderr: std.process.SpawnOptions.StdIo) !u32 {
    var child = try std.process.spawn(io, .{ .argv = argv, .stderr = stderr });
    return switch (try child.wait(io)) {
        .exited => |code| code,
        else => 255,
    };
}

/// .git/HEAD をたどって短縮コミットハッシュを返す。取れなければ null。
fn gitShortRev(io: std.Io, alloc: std.mem.Allocator) ?[]const u8 {
    const cwd = std.Io.Dir.cwd();
    const head = cwd.readFileAlloc(io, ".git/HEAD", alloc, .limited(4096)) catch return null;
    const trimmed = std.mem.trimEnd(u8, head, "\n");
    const rev = if (std.mem.startsWith(u8, trimmed, "ref: ")) blk: {
        const ref_path = std.fmt.allocPrint(alloc, ".git/{s}", .{trimmed["ref: ".len..]}) catch return null;
        const ref = cwd.readFileAlloc(io, ref_path, alloc, .limited(4096)) catch return null;
        break :blk std.mem.trimEnd(u8, ref, "\n");
    } else trimmed;
    return if (rev.len >= 8) rev[0..8] else null;
}

fn isoTimestampUtc(io: std.Io, buffer: []u8) []const u8 {
    const unix_secs = std.Io.Timestamp.now(io, .real).toSeconds();
    const secs = std.time.epoch.EpochSeconds{ .secs = @intCast(unix_secs) };
    const year_day = secs.getEpochDay().calculateYearDay();
    const month_day = year_day.calculateMonthDay();
    const day_secs = secs.getDaySeconds();
    return std.fmt.bufPrint(buffer, "{d:0>4}-{d:0>2}-{d:0>2}T{d:0>2}:{d:0>2}:{d:0>2}Z", .{
        year_day.year,              month_day.month.numeric(),     month_day.day_index + 1,
        day_secs.getHoursIntoDay(), day_secs.getMinutesIntoHour(), day_secs.getSecondsIntoMinute(),
    }) catch unreachable;
}

/// JSON Lines 形式でログに 1 行追記する。
fn appendLog(io: std.Io, alloc: std.mem.Allocator, log_path: []const u8, label: []const u8, times: []const f64, median: f64, input_bytes: u64) !void {
    const cwd = std.Io.Dir.cwd();
    if (std.fs.path.dirname(log_path)) |dir| try cwd.createDirPath(io, dir);
    const old = cwd.readFileAlloc(io, log_path, alloc, .limited(1 << 26)) catch "";

    var line: std.Io.Writer.Allocating = .init(alloc);
    const w = &line.writer;
    var time_buffer: [32]u8 = undefined;
    try w.print("{{\"time\": \"{s}\", \"label\": \"{s}\", \"times\": [", .{ isoTimestampUtc(io, &time_buffer), label });
    for (times, 0..) |t, i| {
        try w.print("{s}{d:.4}", .{ if (i == 0) "" else ", ", t });
    }
    try w.print("], \"median\": {d:.4}, \"input_bytes\": {d}, \"git\": ", .{ median, input_bytes });
    if (gitShortRev(io, alloc)) |rev| try w.print("\"{s}\"}}\n", .{rev}) else try w.print("null}}\n", .{});

    const file = try cwd.createFile(io, log_path, .{});
    defer file.close(io);
    var buffer: [1 << 12]u8 = undefined;
    var writer = file.writer(io, &buffer);
    try writer.interface.writeAll(old);
    try writer.interface.writeAll(line.written());
    try writer.interface.flush();
}

fn medianOf(alloc: std.mem.Allocator, times: []const f64) !f64 {
    const sorted = try alloc.dupe(f64, times);
    std.mem.sort(f64, sorted, {}, std.sort.asc(f64));
    return if (sorted.len % 2 == 1)
        sorted[sorted.len / 2]
    else
        (sorted[sorted.len / 2 - 1] + sorted[sorted.len / 2]) / 2;
}

/// 1 回実行して検証し、経過秒を返す。失敗はエラーで報告する
fn timedRun(io: std.Io, argv: []const []const u8, verify: []const u8, expected: []const u8) !f64 {
    std.Io.Dir.cwd().deleteFile(io, output_path) catch {};
    const start = std.Io.Timestamp.now(io, .awake);
    const code = try runChild(io, argv, .inherit);
    const ns = start.durationTo(std.Io.Timestamp.now(io, .awake)).toNanoseconds();
    if (code != 0) {
        std.debug.print("終了コード {d} で失敗\n", .{code});
        return error.RunFailed;
    }
    // 検証結果の詳細は不正解時に verify を直接叩けば見られるので、ここでは黙らせる
    if (try runChild(io, &.{ verify, output_path, expected }, .ignore) != 0) {
        std.debug.print("出力が不正解\n", .{});
        return error.WrongAnswer;
    }
    return @as(f64, @floatFromInt(ns)) / std.time.ns_per_s;
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    const opts = parseOptions(args) orelse {
        std.debug.print(
            "usage: {s} --bin <exe> --verify <exe> --input <csv> --expected <txt> --label <name> [--bin2 <exe> --label2 <name>] [--runs N] [--log path]\n",
            .{args[0]},
        );
        return 2;
    };

    const cwd = std.Io.Dir.cwd();
    try cwd.createDirPath(io, "work");

    var prefix: std.ArrayList([]const u8) = .empty;
    const cpu_count = std.Thread.getCpuCount() catch 1;
    if (cpu_count > contest_cpus) {
        const cpu_list = try std.fmt.allocPrint(alloc, "0-{d}", .{contest_cpus - 1});
        if (std.Io.Dir.accessAbsolute(io, "/usr/bin/taskset", .{})) |_| {
            try prefix.appendSlice(alloc, &.{ "taskset", "-c", cpu_list });
            std.debug.print("CPU を本番相当の {d} 個に制限: taskset -c {s}\n", .{ contest_cpus, cpu_list });
        } else |_| {}
    }
    var argv_a = try prefix.clone(alloc);
    try argv_a.appendSlice(alloc, &.{ opts.bin, opts.input, output_path });
    var argv_b = try prefix.clone(alloc);
    if (opts.bin2) |bin2| try argv_b.appendSlice(alloc, &.{ bin2, opts.input, output_path });

    std.debug.print("ページキャッシュを温めています...\n", .{});
    try warmPageCache(io, opts.input);

    const times_a = try alloc.alloc(f64, opts.runs);
    const times_b = try alloc.alloc(f64, opts.runs);
    for (0..opts.runs) |run| {
        times_a[run] = timedRun(io, argv_a.items, opts.verify, opts.expected) catch return 1;
        std.debug.print("run {d} {s}: {d:.3}s (正解)\n", .{ run + 1, opts.label, times_a[run] });
        if (opts.bin2 != null) {
            times_b[run] = timedRun(io, argv_b.items, opts.verify, opts.expected) catch return 1;
            std.debug.print("run {d} {s}: {d:.3}s (正解)\n", .{ run + 1, opts.label2.?, times_b[run] });
        }
    }

    const input_bytes = (try cwd.statFile(io, opts.input, .{})).size;
    const median_a = try medianOf(alloc, times_a);
    std.debug.print("中央値: {d:.3}s ({d:.2} GB/s) [{s}]\n", .{ median_a, @as(f64, @floatFromInt(input_bytes)) / median_a / 1e9, opts.label });
    try appendLog(io, alloc, opts.log, opts.label, times_a, median_a, input_bytes);

    if (opts.bin2 != null) {
        const median_b = try medianOf(alloc, times_b);
        std.debug.print("中央値: {d:.3}s ({d:.2} GB/s) [{s}]\n", .{ median_b, @as(f64, @floatFromInt(input_bytes)) / median_b / 1e9, opts.label2.? });
        try appendLog(io, alloc, opts.log, opts.label2.?, times_b, median_b, input_bytes);
        const delta = (median_a / median_b - 1) * 100;
        std.debug.print("A/B: {s} は {s} 比 {s}{d:.1}%\n", .{ opts.label, opts.label2.?, if (delta >= 0) "+" else "", delta });
    }
    std.debug.print("結果を {s} に追記しました\n", .{opts.log});
    return 0;
}
