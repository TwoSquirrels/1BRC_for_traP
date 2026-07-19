//! 提出ターゲット (sapphirerapids) のコード生成を静的に調べる。
//! 手元 CPU は AVX-512 を実行できないため、提出物の性能はアセンブリを読むか
//! llvm-mca のような静的解析器で見積もるしかない。
//!
//! 1. objdump で逆アセンブリ全体を work/asm/<sol>.dis に保存し、関数の命令数一覧と
//!    指定関数 (既定: worker) の本体を表示する
//! 2. zig cc -S で work/asm/<sol>.s を生成する
//! 3. ソースに LLVM-MCA-BEGIN マーカーがあり llvm-mca が入っていれば、
//!    -mcpu=sapphirerapids でサイクル・ポート占有の見積もりを表示する
const std = @import("std");

const max_output = 1 << 28;
const max_shown_instructions = 200;

fn runCapture(io: std.Io, alloc: std.mem.Allocator, argv: []const []const u8) ![]u8 {
    var child = try std.process.spawn(io, .{ .argv = argv, .stdout = .pipe });
    var buffer: [1 << 16]u8 = undefined;
    var reader = child.stdout.?.reader(io, &buffer);
    const output = try reader.interface.allocRemaining(alloc, .limited(max_output));
    return switch (try child.wait(io)) {
        .exited => |code| if (code == 0) output else error.CommandFailed,
        else => error.CommandFailed,
    };
}

fn runInherit(io: std.Io, argv: []const []const u8) !u32 {
    var child = try std.process.spawn(io, .{ .argv = argv });
    return switch (try child.wait(io)) {
        .exited => |code| code,
        else => 255,
    };
}

const Function = struct {
    name: []const u8,
    body_start: usize, // 逆アセンブリ内の行オフセット
    instructions: usize,
};

fn parseFunctions(alloc: std.mem.Allocator, disassembly: []const u8) ![]Function {
    var functions: std.ArrayList(Function) = .empty;
    var it = std.mem.splitScalar(u8, disassembly, '\n');
    var offset: usize = 0;
    while (it.next()) |line| {
        const line_start = offset;
        offset += line.len + 1;
        // 関数ヘッダは "0000000000401000 <name>:" の形
        if (!std.mem.endsWith(u8, line, ">:")) continue;
        const open = std.mem.indexOfScalar(u8, line, '<') orelse continue;
        try functions.append(alloc, .{
            .name = line[open + 1 .. line.len - 2],
            .body_start = line_start,
            .instructions = 0,
        });
    }
    // 各関数の命令数 = 次の関数ヘッダまでの空でない行数
    for (functions.items, 0..) |*f, i| {
        const body_end = if (i + 1 < functions.items.len) functions.items[i + 1].body_start else disassembly.len;
        var lines = std.mem.splitScalar(u8, disassembly[f.body_start..body_end], '\n');
        _ = lines.next(); // ヘッダ行
        while (lines.next()) |line| {
            if (std.mem.indexOfScalar(u8, line, ':') != null) f.instructions += 1;
        }
    }
    return functions.items;
}

fn instructionCountDescending(_: void, a: Function, b: Function) bool {
    return a.instructions > b.instructions;
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len < 4) {
        std.debug.print("usage: {s} <sol> <source.c> <binary> [表示する関数名...]\n", .{args[0]});
        return 2;
    }
    const sol = args[1];
    const source = args[2];
    const binary = args[3];
    const requested = if (args.len > 4) args[4..] else &[_][:0]const u8{"worker"};

    const cwd = std.Io.Dir.cwd();
    try cwd.createDirPath(io, "work/asm");

    // 1. 逆アセンブリ
    const disassembly = runCapture(io, alloc, &.{
        "objdump", "-d", "-M", "intel", "--no-show-raw-insn", binary,
    }) catch {
        std.debug.print("objdump の実行に失敗しました (binutils は入っていますか?)\n", .{});
        return 1;
    };
    const dis_path = try std.fmt.allocPrint(alloc, "work/asm/{s}.dis", .{sol});
    {
        const file = try cwd.createFile(io, dis_path, .{});
        defer file.close(io);
        var buffer: [1 << 16]u8 = undefined;
        var writer = file.writer(io, &buffer);
        try writer.interface.writeAll(disassembly);
        try writer.interface.flush();
    }

    const functions = try parseFunctions(alloc, disassembly);
    const sorted = try alloc.dupe(Function, functions);
    std.mem.sort(Function, sorted, {}, instructionCountDescending);
    std.debug.print("=== 命令数の多い関数 (全体: {s}) ===\n", .{dis_path});
    for (sorted[0..@min(12, sorted.len)]) |f| {
        std.debug.print("{d:>6}  {s}\n", .{ f.instructions, f.name });
    }

    for (requested) |want| {
        for (functions, 0..) |f, i| {
            if (std.mem.indexOf(u8, f.name, want) == null) continue;
            std.debug.print("\n=== {s} ({d} 命令) ===\n", .{ f.name, f.instructions });
            if (f.instructions > max_shown_instructions) {
                std.debug.print("(長いので省略。{s} の {s} を参照)\n", .{ dis_path, f.name });
                continue;
            }
            const body_end = if (i + 1 < functions.len) functions[i + 1].body_start else disassembly.len;
            std.debug.print("{s}\n", .{std.mem.trim(u8, disassembly[f.body_start..body_end], "\n")});
        }
    }

    // 2. アセンブリ生成 (提出ターゲットと同じ CPU 指定)
    const s_path = try std.fmt.allocPrint(alloc, "work/asm/{s}.s", .{sol});
    if (try runInherit(io, &.{
        "zig",                   "cc",            "-S",   "-O3", "-std=c23", "-target", "x86_64-linux-musl",
        "-march=sapphirerapids", "-DMCA_MARKERS", source, "-o",  s_path,
    }) != 0) {
        std.debug.print("zig cc -S に失敗しました\n", .{});
        return 1;
    }
    std.debug.print("\nアセンブリを生成しました: {s} (-march=sapphirerapids)\n", .{s_path});

    // 3. llvm-mca (マーカーがあれば)
    const s_text = try cwd.readFileAlloc(io, s_path, alloc, .limited(max_output));
    const has_marker = std.mem.indexOf(u8, s_text, "LLVM-MCA-BEGIN") != null;
    const has_mca = if (runCapture(io, alloc, &.{ "llvm-mca", "--version" })) |_| true else |_| false;
    if (has_marker and has_mca) {
        std.debug.print("\n=== llvm-mca (-mcpu=sapphirerapids) ===\n", .{});
        if (try runInherit(io, &.{ "llvm-mca", "-mcpu=sapphirerapids", s_path }) != 0) {
            std.debug.print("llvm-mca の解析に失敗しました\n", .{});
            return 1;
        }
    } else {
        if (!has_mca) {
            std.debug.print("llvm-mca が未導入です: sudo apt install llvm でサイクル見積もりが有効になります\n", .{});
        }
        if (!has_marker) {
            std.debug.print(
                \\ホットループのサイクル見積もりを取るには、対象区間を MCA_BEGIN("名前") /
                \\MCA_END("名前") マクロで囲んでください (04-parallel.c のマクロ定義を参照。
                \\-DMCA_MARKERS はこのタスクが自動で付けます)
                \\
            , .{});
        }
    }
    return 0;
}
