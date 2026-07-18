//! ISO 8601 UTC 文字列を Web UI と同じ JST 表示に変換する共通ヘルパ。
const std = @import("std");

/// "2026-07-18T18:49:12.345Z" のような ISO 8601 UTC を「7/19 03:49」の JST 表示にする。
/// パースできなければそのまま返す。
pub fn formatJst(alloc: std.mem.Allocator, iso: []const u8) []const u8 {
    if (iso.len < 16 or iso[4] != '-' or iso[7] != '-' or iso[10] != 'T') return iso;
    const parse = std.fmt.parseInt;
    const year = parse(u16, iso[0..4], 10) catch return iso;
    const month = parse(u8, iso[5..7], 10) catch return iso;
    const day = parse(u8, iso[8..10], 10) catch return iso;
    const hour = parse(u8, iso[11..13], 10) catch return iso;
    const minute = parse(u8, iso[14..16], 10) catch return iso;

    // JST = UTC+9。日付繰り上がりは月末日数を見て処理する (コンテスト期間中に年跨ぎはない)
    var jst_hour = hour + 9;
    var jst_day = day;
    var jst_month = month;
    if (jst_hour >= 24) {
        jst_hour -= 24;
        jst_day += 1;
        const leap = (year % 4 == 0 and year % 100 != 0) or year % 400 == 0;
        const month_days = [12]u8{ 31, if (leap) 29 else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (jst_day > month_days[jst_month - 1]) {
            jst_day = 1;
            jst_month = if (jst_month == 12) 1 else jst_month + 1;
        }
    }
    return std.fmt.allocPrint(alloc, "{d}/{d:0>2} {d:0>2}:{d:0>2}", .{ jst_month, jst_day, jst_hour, minute }) catch iso;
}
