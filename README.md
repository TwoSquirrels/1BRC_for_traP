# りすりすの 1BRC for traP 挑戦リポジトリ

りすりすが [OPTIMIZATION CONTEST「1BRC for traP」](https://1brc.trap.games/) に挑戦するためのリポジトリです。

「1BRC for traP」とは、traQ 風 CSV を集計するプログラムの実行時間を競うコンテストです。

- [`README.md`](README.md) - 目次
- [`contest.md`](contest.md) - ルール
- [`consideration.md`](consideration.md) - 考察
- [`AGENTS.md`](AGENTS.md) - AI エージェント向けの約束事 (`CLAUDE.md` はそのインポート)
- `build.zig` - ビルド & タスクランナー定義
- `src/` - ソリューション (1 ファイル = 1 ソリューション)
- `tools/` - 検証・計測・生成ツール
- `datasets/` - 公開データセット
- `tests/` - 生成したエッジケースデータ
- `results/` - ベンチマーク履歴 (`bench.jsonl`)
- `work/` - 実行時の出力置き場

## セットアップ

Zig は [mise](https://mise.jdx.dev/) で管理しています。

```bash
mise install       # Zig の導入
mise run download  # 公開データセットの取得・展開
```

提出する場合は、アクセスキーを `.env` に置きます (mise が環境変数として読み込みます)。

```bash
echo 'ONEBRC_ACCESS_KEY=...' > .env
```

## 使い方

タスクは `mise run` で呼びます (`mise tasks` で一覧)。追加引数は `--` の後ろに渡します。

```bash
mise run verify                # 実行して出力を検証 (既定: naive を 1m で)
mise run verify -- -Ddata=10m  # データセット指定 (1m / 10m / 100m / edge)
mise run bench -- -Ddata=100m  # 本番方式の計測: キャッシュ温め → 8 CPU 固定で 3 回実行 → 毎回検証 → 中央値を results/ に記録
mise run perf                  # perf record によるサンプリングプロファイル
mise run gen                   # エッジケースの入力・期待出力を tests/ に生成
mise run release               # 提出用バイナリを zig-out/release/ に作り、提出制限を検査
mise run submit -- -Ddry       # 提出内容の確認 (dry-run)
mise run submit                # release 検査を通してから API へ提出
mise run leaderboard           # 現在の public リーダーボードを表示
mise run download              # 公開データセット (~100m) を datasets/ に取得・展開
mise run asm                   # sapphirerapids の逆アセンブリを work/asm/ に保存し、関数一覧とホット関数を表示。サイクル見積もりも可能
```

ソリューションを増やすときは `src/<名前>.c` を置いて `-Dsol=<名前>` で選択します。

```bash
mise run bench -- -Dsol=mmap -Ddata=100m
```

通常ビルドは perf で読みやすいよう ReleaseFast + デバッグ情報 + フレームポインタ保持です。
サニタイザ付きで動かしたい場合は `-Doptimize=Debug` を指定します (zig cc が UBSan を有効化)。

## メモ

- 1B データセットは展開後 26 GiB 前後になるため手元では使いません。100m までで開発し、提出で最終確認します。
- `tools/reference.zig` (参照実装) の出力は public-1m / public-10m の期待出力と完全一致を確認済みです。
- 平均値の丸めは C の `printf("%.2f")` と同じ挙動 (double の 2 進値を正確に丸め、端数 0.5 ちょうどは偶数丸め) です。Zig の `{d:.2}` はこれと結果がずれることがあるため、reference は printf 互換の丸めを自前実装しています (`writeAvg`)。
- WSL2 では perf のハードウェアカウンタ (cycles、cache-misses 等) は使えず、cpu-clock サンプリングになります。関数ごとの時間内訳を見る用途には十分です。
- 提出用バイナリは本番環境 (Sapphire Rapids) 向けの AVX-512 有効ビルドのため、非対応の手元 CPU では実行できません (`-Drelease-cpu=x86_64_v3` で差し替え可能)。手元の検証は native ビルド (verify / bench) で行い、提出物そのものの最終確認は本番の Public 計測に委ねます。
