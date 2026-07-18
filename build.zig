const std = @import("std");

// src/*.c の 1 ファイルが 1 ソリューション。-Dsol で選択し、-Ddata でデータセットを選ぶ。
// tools/*.zig は検証・計測などの補助ツールで、各ステップから実行される。
const default_solution = "naive";
// -O3: zig の ReleaseFast は C ソースへ -O2 を渡すため明示で上書きする (後勝ち)
const c_flags = [_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-O3" };
// perf 用にリリースビルドでもデバッグ情報とフレームポインタを残す (速度への影響はほぼゼロ)
const profile_flags = c_flags ++ [_][]const u8{ "-g", "-fno-omit-frame-pointer" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.option(std.builtin.OptimizeMode, "optimize", "最適化モード (既定: ReleaseFast)") orelse .ReleaseFast;
    const sol = b.option([]const u8, "sol", "ソリューション名 (src/<sol>.c)") orelse default_solution;
    const data = b.option([]const u8, "data", "データセット: 1m | 10m | 100m | edge") orelse "1m";
    const dry = b.option(bool, "dry", "submit を dry-run にする") orelse false;
    // 本番 (r7i.2xlarge = Sapphire Rapids, AVX-512 対応) 向け。手元の CPU では実行できない
    // 点に注意 (手元検証は native ビルド、提出物の最終確認は本番の Public 計測に委ねる)。
    const release_cpu = b.option([]const u8, "release-cpu", "提出用バイナリの CPU (既定: sapphirerapids)") orelse "sapphirerapids";

    const input_path, const expected_path = datasetPaths(b, data);
    const source_path = b.fmt("src/{s}.c", .{sol});

    const exe = addSolution(b, sol, source_path, target, optimize, &profile_flags);
    b.installArtifact(exe);

    const verify_exe = addTool(b, "verify", target);
    const bench_exe = addTool(b, "bench", target);
    const reference_exe = addTool(b, "reference", target);
    const gen_edge_exe = addTool(b, "gen_edge", target);
    const check_exe = addTool(b, "check_submission", target);
    const perf_exe = addTool(b, "perf", target);
    const submit_exe = addTool(b, "submit", target);
    const leaderboard_exe = addTool(b, "leaderboard", target);
    const download_exe = addTool(b, "download", target);

    // zig build run — work/out.txt へ出力
    const run = b.addRunArtifact(exe);
    run.addArg(input_path);
    run.addArg("work/out.txt");
    run.has_side_effects = true;
    run.step.dependOn(&makeWorkDir(b).step);
    b.step("run", "選択したソリューションを実行する").dependOn(&run.step);

    // zig build verify — 実行して期待出力と比較
    const verify = b.addRunArtifact(verify_exe);
    verify.addArgs(&.{ "work/out.txt", expected_path });
    verify.has_side_effects = true;
    verify.step.dependOn(&run.step);
    b.step("verify", "実行して出力を検証する").dependOn(&verify.step);

    // zig build bench — 本番方式で 3 回計測して中央値をログに残す
    const bench = b.addRunArtifact(bench_exe);
    bench.addArg("--bin");
    bench.addArtifactArg(exe);
    bench.addArg("--verify");
    bench.addArtifactArg(verify_exe);
    bench.addArgs(&.{ "--input", input_path, "--expected", expected_path });
    bench.addArgs(&.{ "--label", b.fmt("{s}/{s}", .{ sol, data }) });
    bench.has_side_effects = true;
    b.step("bench", "計測して結果を results/ に記録する").dependOn(&bench.step);

    // zig build perf — サンプリングプロファイル
    const perf = b.addRunArtifact(perf_exe);
    perf.addArtifactArg(exe);
    perf.addArg(input_path);
    perf.has_side_effects = true;
    b.step("perf", "perf record でプロファイルを取る").dependOn(&perf.step);

    // zig build gen — エッジケース入力を生成し、参照実装で期待出力を作る
    const gen = b.addRunArtifact(gen_edge_exe);
    gen.has_side_effects = true;
    const gen_expected = b.addRunArtifact(reference_exe);
    gen_expected.addArgs(&.{ "tests/edge.input.csv", "tests/edge.expected.txt" });
    gen_expected.has_side_effects = true;
    gen_expected.step.dependOn(&gen.step);
    b.step("gen", "エッジケースの入力と期待出力を生成する").dependOn(&gen_expected.step);

    // zig build release — 提出用バイナリ (musl 静的リンク) を作って制限を検査
    const release_query = std.Target.Query.parse(.{
        .arch_os_abi = "x86_64-linux-musl",
        .cpu_features = release_cpu,
    }) catch |err| std.debug.panic("-Drelease-cpu={s} を解釈できません: {t}", .{ release_cpu, err });
    const release_target = b.resolveTargetQuery(release_query);
    const release_exe = addSolution(b, sol, source_path, release_target, .ReleaseFast, &c_flags);
    const install_release = b.addInstallArtifact(release_exe, .{
        .dest_dir = .{ .override = .{ .custom = "release" } },
    });
    const check = b.addRunArtifact(check_exe);
    check.addArg(source_path);
    check.addArtifactArg(release_exe);
    check.step.dependOn(&install_release.step);
    b.step("release", "提出用バイナリを zig-out/release/ に作り制限を検査する").dependOn(&check.step);

    // zig build submit — release の制限検査を通過した提出物を API へ POST (-Ddry で内容確認のみ)
    const submit = b.addRunArtifact(submit_exe);
    submit.addArg("--source");
    submit.addArg(source_path);
    submit.addArg("--binary");
    submit.addArtifactArg(release_exe);
    if (dry) submit.addArg("--dry-run");
    submit.has_side_effects = true;
    submit.step.dependOn(&check.step);
    b.step("submit", "提出する (要 ONEBRC_ACCESS_KEY、-Ddry で dry-run)").dependOn(&submit.step);

    // zig build leaderboard — 現在の順位表を表示
    const leaderboard = b.addRunArtifact(leaderboard_exe);
    leaderboard.has_side_effects = true;
    b.step("leaderboard", "リーダーボードを表示する").dependOn(&leaderboard.step);

    // zig build download — 公開データセット (1m/10m/100m) を datasets/ に取得・展開
    const download = b.addRunArtifact(download_exe);
    download.has_side_effects = true;
    b.step("download", "公開データセットをダウンロードして展開する").dependOn(&download.step);
}

fn addSolution(
    b: *std.Build,
    name: []const u8,
    source_path: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    flags: []const []const u8,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addCSourceFiles(.{ .files = &.{source_path}, .flags = flags });
    return b.addExecutable(.{ .name = name, .root_module = mod });
}

fn addTool(b: *std.Build, comptime name: []const u8, target: std.Build.ResolvedTarget) *std.Build.Step.Compile {
    const mod = b.createModule(.{
        .root_source_file = b.path("tools/" ++ name ++ ".zig"),
        .target = target,
        .optimize = .ReleaseSafe,
    });
    return b.addExecutable(.{ .name = name, .root_module = mod });
}

fn datasetPaths(b: *std.Build, data: []const u8) struct { []const u8, []const u8 } {
    if (std.mem.eql(u8, data, "edge")) {
        return .{ "tests/edge.input.csv", "tests/edge.expected.txt" };
    }
    return .{
        b.fmt("datasets/public-{s}.input.csv", .{data}),
        b.fmt("datasets/public-{s}.expected.txt", .{data}),
    };
}

fn makeWorkDir(b: *std.Build) *std.Build.Step.Run {
    return b.addSystemCommand(&.{ "mkdir", "-p", "work" });
}
