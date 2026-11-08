# CueHub 0.1.0-preview.3

Hub の配布ファイル取得を GitHub REST API 経由へ統一した Preview。
対応する Engine SDK は既存の `0.1.0-preview.2` を利用する。

## 変更内容

- Engine のインストール・修復と Hub 自身の更新で、ブラウザー向け URL に代えて Asset API を使用する。
- 公開 Release の取得に Git / GitHub CLI / GitHub ログインを要求しない。
- Asset ID、取得先 Repository、Tag と File 名の対応を照合する。
- Manifest の署名、Package / Feed の Hash と容量、展開時の安全性検証を維持する。
- HTTP 取得失敗時に Status Code と取得先 API URL を表示する。
- 公開一覧 Cache は `engine-catalog-v2.json` を使う。旧 Cache は保全し、導入済み Engine / Project 登録は維持する。

## ローカル更新

既存 Hub の取得処理は更新されるまで従来の経路を使う。ネット経由の自己更新が失敗する場合は、
以下の 4 File を同じ Folder へ取得してから、Settings の「ローカル Feed を選択」でその Folder を指定する。

- `hub-release.json`
- `hub-release.json.sig`
- `releases.win.json`
- `CueEngine.Hub-0.1.0-preview.3-full.nupkg`

「Hub 更新を確認・準備」で版番号を確認し、更新・再起動を承認する。
更新後はローカル Feed 欄を空に戻して設定を保存し、公開一覧をオンラインで一度取得し直す。
新規導入は同じ Release の `CueEngine.Hub-win-Setup.exe` を利用する。

## 検証と制約

- Hub Core の 122 項目、WPF 画面 Test、Hub Debug Build、Repository 標準 Debug Build が成功。
- 製品と同じ API 取得処理で公開 Engine を隔離 Folder へ導入し、Hub 更新用 4 File の署名 / Hash も検証。
- 実ユーザーの Hub 更新適用、別 PC での通し検証、Authenticode 署名は未実施。
- API 経由でも GitHub 自体の障害・回数制限は発生し得る。
- 更新直後の初回公開一覧取得にはオンライン接続が必要。
