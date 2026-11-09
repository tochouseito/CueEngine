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

- 公開 CI のビルド時だけ GitHub REST API で公開済み Release を取得する。ローカルでは API を呼ばず、ダウンロード情報が利用できない状態でページを確認できる。
- 配布 Catalog には Engine / Hub それぞれ公開日時順の最新 10 世代を含める。ダウンロード欄では検証した Hub Catalog から正式版と Preview それぞれの最新 Setup を案内する。
- 公開 CI は `GH_TOKEN` と既存の `CUE_PACKAGE_SIGNING_KEY_PEM` Secret を必要とする。`CUE_REQUIRE_RELEASE_CATALOG=1` の場合、取得・署名・検証の失敗でビルドを止め、既存の公開サイトを維持する。秘密鍵をサイトのデータや成果物へ出力しない。
- リポジトリの Settings → Pages → Source を **GitHub Actions** に設定すると、`release` ブランチの `Reference Site` Workflow から公開できる。
- CueHub の Tag Release が成功すると `reference-site.yml` を `release` ブランチで明示的に起動する。サイトだけを更新する場合は同じ Workflow を手動実行できる。
- 配布物と署名の生成は [配布手順](../Tools/Packaging/README.md) を参照する。

## 署名付き配布 Catalog

固定 URL は `https://tochouseito.github.io/CueEngine/distribution/catalog-v1.json` と隣の `catalog-v1.json.sig`。
Catalog は UTF-8 の生 Byte 列を ECDSA P-256 / SHA-256 / IEEE-P1363 で署名し、署名を Base64 と末尾改行で保存する。
上限は Catalog 4 MiB、署名 4096 Byte。署名鍵のないローカルビルドは Catalog を生成しない。

Root は `schemaVersion`（1）、`generatedAt`（UTC ISO 8601）、`engines`、`hubs` の 4 項目。
両配列の各要素は `version`、`tag`、`isPreview`、`publishedUtc`、`pageUrl`、`assets` を持つ。
`assets` は必要な File 名から通常の `https://github.com/tochouseito/CueEngine/releases/download/<Tag>/<File>` への対応であり、Tag と File 名はそれぞれ URI Escape する。API URL や Token は含めない。

Builder は各候補の Manifest と署名を Asset API で取得し、Catalog 署名鍵から導出した公開鍵で署名を検証する。
公開済み Asset の所属、完了状態、名前、サイズと Manifest の Tag / Version を照合する。
GitHub が提供する SHA-256 Digest がある場合は署名済み Hash と照合する。巨大な Package 本体の全量再取得は行わず、インストール時の Hash 検証を継続する。
Hub v1 の Setup 自体は既存 Manifest の署名対象ではないため、所属と完了状態を検証する。

- Engine v1 は Manifest / 署名 / ZIP、v2 は Manifest / 署名 / 連番 Part を含める。v2 の論理 ZIP 本体は添付しない。
- Engine v2 は最大 512 Part、各 Part は 2 GiB 未満、圧縮・展開総量はそれぞれ 1 TiB 以下。Part 名は論理 ZIP 名に `.001` から連続する 3 桁番号を付ける。
- Hub v1 は Manifest / 署名 / Full Package / Feed / Windows x64 Setup を含める。
- Manifest は 256 KiB 以下。必須 Asset の欠落、署名不正、サイズ・Digest 不一致を発見した候補は黙って除外せず、公開ビルドを失敗させる。
