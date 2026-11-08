# CueHub 0.1.0-preview.4

新規 Project の「使用する Engine」の候補を、Version 名だけの表示へ整理した Preview。
Engine SDK の最新版は引き続き `0.1.0-preview.2`。

## 変更内容

- Engine の候補は `0.1.0-preview.2` のように Version 名だけを表示する。
- 候補に表示していた「導入版」「ローカル開発版」とフルパスを除く。

## 更新方法

Settings の「Preview を表示・更新候補に含める」を有効にし、「Hub 更新を確認・準備」を使う。
ローカル更新では次の 4 File を同じ Folder に置き、「ローカル Feed を選択」でその Folder を指定する。

- `hub-release.json`
- `hub-release.json.sig`
- `releases.win.json`
- `CueEngine.Hub-0.1.0-preview.4-full.nupkg`

更新後はローカル Feed 欄を空に戻して設定を保存する。
新規導入は同じ Release の `CueEngine.Hub-win-Setup.exe` を使う。

## 検証と制約

Hub Build、Core の 122 項目、WPF 画面 Test、配布契約 21 項目、公開契約 59 項目と標準 Debug Build が成功。
公開後は署名 / Hash / 発行者公開鍵を検証する。
実ユーザーの Hub 更新適用、別 PC での通し検証、Authenticode 署名は未実施。
