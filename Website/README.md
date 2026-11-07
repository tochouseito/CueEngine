# CueEngine リファレンスサイト

`Website/` は利用者向けの Docusaurus サイト。対象は CueHub のダウンロード、Engine SDK の導入、最初のアプリ作成。
開発者向けの設計判断や調査記録はリポジトリの `Docs/` で管理する。

## ローカル確認

Node.js 24 と npm を用意し、`Website/` で実行する。

```text
npm ci
npm test
npm run build
npm run serve
```

`serve` が表示する URL の `/CueEngine/` を開く。編集時は `npm start` でプレビューできる。
サイトの依存は `package-lock.json` に固定する。C++ のビルドから独立しており、CueEngine SDK にサイトの npm 依存は同梱しない。

## ダウンロード情報と公開

- ダウンロード欄はサイトのビルド時に公開済み `hub-v…` の Release を取得し、正式版と Preview それぞれの Setup を案内する。
- GitHub に Release がない場合は公開前の状態を表示する。公開用ビルドで API 取得に失敗した場合はビルドを止める。
- リポジトリの Settings → Pages → Source を **GitHub Actions** に設定すると、`release` ブランチの `Reference Site` Workflow から公開できる。
- CueHub の Tag Release が成功すると `reference-site.yml` を `release` ブランチで明示的に起動する。サイトだけを更新する場合は同じ Workflow を手動実行できる。
- 配布物と署名の生成は [配布手順](../Tools/Packaging/README.md) を参照する。
