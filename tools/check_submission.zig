//! 提出物がコンテストのファイル制限を満たすか検査する。
//!
//! - ソース: 単一ファイル、UTF-8、NUL なし、1.0 MiB 以下
//! - 実行ファイル: x86_64 ELF、64.0 MiB 以下
const std = @import("std");

const mib = 1024 * 1024;
const elf_machine_x86_64 = 62;

fn checkSource(io: std.Io, alloc: std.mem.Allocator, path: []const u8, errors: *std.ArrayList([]const u8)) !void {
    const data = try std.Io.Dir.cwd().readFileAlloc(io, path, alloc, .unlimited);
    if (data.len > mib) {
        try errors.append(alloc, try std.fmt.allocPrint(alloc, "[source] 1.0 MiB を超過 ({d} bytes)", .{data.len}));
    }
    if (std.mem.indexOfScalar(u8, data, 0) != null) {
        try errors.append(alloc, "[source] NUL バイトが含まれる");
    }
    if (!std.unicode.utf8ValidateSlice(data)) {
        try errors.append(alloc, "[source] UTF-8 でない");
    }
}

fn checkElf(io: std.Io, alloc: std.mem.Allocator, path: []const u8, errors: *std.ArrayList([]const u8)) !void {
    const cwd = std.Io.Dir.cwd();
    const size = (try cwd.statFile(io, path, .{})).size;
    if (size > 64 * mib) {
        try errors.append(alloc, try std.fmt.allocPrint(alloc, "[binary] 64.0 MiB を超過 ({d} bytes)", .{size}));
    }

    const file = try cwd.openFile(io, path, .{});
    defer file.close(io);
    var buffer: [64]u8 = undefined;
    var reader = file.reader(io, &buffer);
    var header: [20]u8 = undefined;
    reader.interface.readSliceAll(&header) catch {
        try errors.append(alloc, "[binary] ELF ヘッダを読めない");
        return;
    };
    if (!std.mem.eql(u8, header[0..4], "\x7fELF")) {
        try errors.append(alloc, "[binary] ELF ではない");
    } else if (std.mem.readInt(u16, header[18..20], .little) != elf_machine_x86_64) {
        try errors.append(alloc, "[binary] x86_64 向け ELF ではない");
    }
}

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const alloc = init.arena.allocator();

    const args = try init.minimal.args.toSlice(alloc);
    if (args.len != 3) {
        std.debug.print("usage: {s} <source.c> <binary>\n", .{args[0]});
        return 2;
    }

    var errors: std.ArrayList([]const u8) = .empty;
    try checkSource(io, alloc, args[1], &errors);
    try checkElf(io, alloc, args[2], &errors);

    if (errors.items.len != 0) {
        std.debug.print("NG: 提出制限に違反しています\n", .{});
        for (errors.items) |e| std.debug.print("  {s}\n", .{e});
        return 1;
    }
    std.debug.print("OK: 提出制限を満たしています ({s}, {s})\n", .{ args[1], args[2] });
    return 0;
}
