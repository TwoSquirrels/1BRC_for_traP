# AGENTS.md

まず [README.md](README.md) を読んでください。ここには README から読み取れないエージェント向けの指示だけを書きます。

- 補助ツールは Zig で書いてください (`tools/*.zig`)。Python やシェルスクリプトを持ち込まないでください。外部コマンドへの委譲は perf のように事実上代替が無い場合に限ります。
- mise のシェルフックが効かない非対話シェルでは、`mise run <名前>` の代わりに `mise x -- zig build <名前>` を使ってください。
- 性能について報告するときは `mise run bench` の中央値を根拠にしてください。単発実行の time を根拠にしないでください。
- 提出後の判定は、提出時のレスポンスにある `statusUrl` へ `Authorization: Bearer $ONEBRC_ACCESS_KEY` 付きで GET すると確認できます (Web UI を見られないため)。`status` が `infrastructure_error` (例: "worker restarted during benchmark") ならジャッジ側の問題なので、同じ提出物をそのまま再提出してください。
