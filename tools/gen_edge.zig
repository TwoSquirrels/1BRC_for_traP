//! 公開データでは踏みにくいエッジケースの入力を tests/ に生成する。
//! 期待出力は build.zig が reference を続けて実行して生成する。
const std = @import("std");

const Row = struct { ts: i64, channel: []const u8, length: i64, stamps: i64 };

// 入力保証 (contest.md 02 章) の範囲内でエッジを突く:
//   時刻は 2027 年内 (1798761600..1830297599)、message_length >= 1、
//   channel_path は [0-9A-Za-z_-]{1,20} のセグメント 1〜5 個を / で連結
// 月境界: 2027-01-31T23:59:59Z = 1801439999, 2027-02-01T00:00:00Z = 1801440000
const segment20 = "aZ09_-aZ09_-aZ09_-aZ"; // 使える全文字種を含む 20 文字
const max_channel = segment20 ++ ("/" ++ segment20) ** 4; // 合法な最大長 104 文字
const rows = [_]Row{
    .{ .ts = 1801439999, .channel = "edge/month-boundary", .length = 10, .stamps = 1 },
    .{ .ts = 1801440000, .channel = "edge/month-boundary", .length = 20, .stamps = 2 },
    .{ .ts = 1798761600, .channel = "edge/year-start", .length = 5, .stamps = 0 }, // 2027-01-01T00:00:00
    .{ .ts = 1830297599, .channel = "edge/year-end", .length = 5, .stamps = 0 }, // 2027-12-31T23:59:59
    .{ .ts = 1798761601, .channel = "a", .length = 1, .stamps = 1 }, // 最短チャンネル名・最小メッセージ長
    .{ .ts = 1798761602, .channel = max_channel, .length = 12345, .stamps = 99 },
    // 8 桁以上の数値 (SWAR 一括パースの範囲外)。月合計が u32 に収まる保証の範囲で最大級の桁数
    .{ .ts = 1798761603, .channel = "edge/big-numbers", .length = 4_000_000_000, .stamps = 294_967_295 },
    .{ .ts = 1798761603, .channel = "edge/big-numbers", .length = 12345678, .stamps = 10000000 },
    // 数値 2 フィールドが 8 バイト内に収まりつつ stamp_count が 3 桁以上 (低乗算経路の範囲外)
    .{ .ts = 1798761604, .channel = "edge/mid-numbers", .length = 9, .stamps = 999 },
    // 2 フィールド合計は 8 バイト超だが各フィールドは 7 桁以下 (個別 SWAR 一括パースの経路)
    .{ .ts = 1798761605, .channel = "edge/mid-numbers", .length = 123456, .stamps = 1234 },
    // SIMD 名前スキャンの一括幅の境界。AVX2 (手元) は 32 文字から、AVX-512 (提出) は
    // 64 文字からフォールバックの 8 B ループに落ちる
    .{ .ts = 1798761606, .channel = simd_boundary(31), .length = 1, .stamps = 0 },
    .{ .ts = 1798761606, .channel = simd_boundary(32), .length = 1, .stamps = 0 },
    .{ .ts = 1798761606, .channel = simd_boundary(63), .length = 1, .stamps = 0 },
    .{ .ts = 1798761606, .channel = simd_boundary(64), .length = 1, .stamps = 0 },
    .{ .ts = 1798761600, .channel = "edge/dup", .length = 50, .stamps = 5 },
    .{ .ts = 1798761600, .channel = "edge/dup", .length = 50, .stamps = 5 }, // 完全に同一の行
    .{ .ts = 1798761600, .channel = "edge/avg-round", .length = 1, .stamps = 0 }, // 平均 1.5 → 丸め確認
    .{ .ts = 1798761600, .channel = "edge/avg-round", .length = 2, .stamps = 0 },
} ++ tieRows("edge/tie-even", 9) // 平均 9/8 = 1.125 (2 進で正確) → 偶数丸めで 1.12
    ++ tieRows("edge/tie-odd", 11); // 平均 11/8 = 1.375 → 偶数丸めで 1.38

/// 指定した合計長のチャンネル名を、制約 (セグメント 1〜20 文字) の範囲で組み立てる
fn simd_boundary(comptime total: usize) []const u8 {
    comptime {
        var name: []const u8 = "b" ** 20;
        while (total - name.len >= 21) name = name ++ "/" ++ "b" ** 20;
        const rem = total - name.len;
        if (rem == 1) {
            // 残り 1 文字は「スラッシュ + 空セグメント」になってしまうので、
            // 末尾セグメントを 1 文字削ってから 1 文字セグメントを足す (差し引き +1)
            name = name[0 .. name.len - 1] ++ "/c";
        } else if (rem >= 2) {
            name = name ++ "/" ++ "c" ** (rem - 1);
        }
        std.debug.assert(name.len == total);
        return name;
    }
}

/// 合計 total を 8 件で割ると 2 進で正確に .xx5 になる行を作る (printf の偶数丸めを踏む)
fn tieRows(comptime channel: []const u8, comptime total: i64) [8]Row {
    var result: [8]Row = undefined;
    for (&result, 0..) |*row, i| {
        row.* = .{ .ts = 1798761600, .channel = channel, .length = if (i == 0) total - 7 else 1, .stamps = 0 };
    }
    return result;
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const cwd = std.Io.Dir.cwd();
    try cwd.createDirPath(io, "tests");
    const file = try cwd.createFile(io, "tests/edge.input.csv", .{});
    defer file.close(io);
    var buffer: [1 << 12]u8 = undefined;
    var writer = file.writer(io, &buffer);
    const out = &writer.interface;
    try out.print("unix_timestamp,channel_path,message_length,stamp_count\n", .{});
    for (rows) |row| {
        try out.print("{d},{s},{d},{d}\n", .{ row.ts, row.channel, row.length, row.stamps });
    }
    try out.flush();
    std.debug.print("生成しました: tests/edge.input.csv ({d} 行)\n", .{rows.len});
}
