# 1BRC for traP 考察

## ルールを読んで

- ランタイムは、普通に考えると Native がよさそう。
- 言語は、メモリレベルで黒魔術が書けて限界高速化ができる C がいいのかな。
- 愚直にやるなら、適当なハッシュテーブルを持って集計なので、まずはこれを書くのがよさそう。
- 並列化がかなり利きそう。
- チャンネル名は一様分布の文字列ではなくかなり偏りがあるので、これを活かした最適化ができそう。
- 入出力の高速化は、Library Checker の問題「A+B」の解説記事が役に立ちそう。
  - [C++で挑む入出力限界高速化 | 東京科学大学デジタル創作同好会traP](https://trap.jp/post/2887/)
  - [A+Bから始める異常高速化](https://zenn.dev/mizar/articles/fc87d667153080)
- キャッシュメモリを意識した高速化は、tatyam さんの特別講義が役に立ちそう。
  - [定数倍高速化の技術 - Speaker Deck](https://speakerdeck.com/tatyam_prime/ding-shu-bei-gao-su-hua-noji-shu)
- まず、手元での計測周りのツールを整備するところから始めるのがよさそう？
  - Claude Fable 5 に mise + Zig で整備してもらった。
