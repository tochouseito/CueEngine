# Hub 初期実装の検証

日付: 2026-11-04。Tag / Push / GitHub Release 公開は実施していない。
最終パッケージと統合検証の完了日は 2026-11-05。

## 自動検証

- WPF Debug Build: 成功、警告 0 / Error 0
- Core Console: 52 checks 成功。実 C++ ProjectTool の Empty3D / RotateCube 作成・検証・既存拒否、未導入版の登録と別版の起動前拒否を含む
- Core 単独: 41 checks 成功。`out/validation/HubCoreOnlyTests.log`
- WPF STA: Window を実生成し、3 Tab の切替、Binding Error、初期 Command 抑止、未導入版の起動抑止と版選択を確認して成功
- 記録: `out/validation/HubCoreTests.log`、`out/validation/HubWpfUiTests.log`

最終 Debug Build 後の実 CLI で、未導入 Engine 版を指定した Project の登録は許可し、
開く時だけ版照合する契約を確認した。

Core 検証は署名の改竄・異なる発行者・Hash・サイズ、不正 Zip の Traversal / 重複 / 予約名、
取消、修復時の旧版保持、同 Version の別 Build 拒否、共有 Lease と削除排他、管理外削除拒否、
複数 Engine 共存、GitHub のページング・製品混在・Draft・欠損・合計 10 件・Offline Cache を含む。
Hub 更新は署名・Feed / Full Package の改竄、製品混同、Path Traversal、未署名 Delta を拒否する。

## 実インストーラーと自己更新

親の検証用 Root は `out/validation/HubDistributionCurrent.txt` に記録する。
共通の一時 ECDSA P-256 発行者鍵で HubPackage1 / 2 / 3 / 4 を生成した。
初版 Setup を `--silent --installto <TestRoot>/HubInstalled` で専用配置へ導入し、
`HubInstall.log` で正常終了を確認した。事前に既存 `CueEngine.Hub` の導入がないことを確認した。

署名済み Local Feed を製品の HubUpdateService に渡し、実 Update.exe で
`0.1.0-preview.3` → `0.1.0-preview.4` の差替え・再起動に成功した。
Test Driver は製品と同じ WindowsVelopackLocator を使用し、実 PID の終了を待たせる。
WPF Main には検証用の起動引数を追加していない。

- `<TestRoot>/HubUpdateFixedProof.json`: 適用前の署名検証済み Version
- `<TestRoot>/HubInstalled/HubUpdater.log`: Package applied successfully、再起動実行 File
- `CueHub.dll` の ProductVersion: `0.1.0-preview.4+9492422768c3b0350a3033aac07aa920b184450e`
- 再起動直後の検証 PID: `46564`、Main Window Handle: `3542690`

この Preview 番号の差分はローカル更新試験用 VersionOverride。Source 正本は `0.1.0-preview.1`。
一時署名鍵は配布用鍵として登録・公開しない。Authenticode 署名は未実施。

## 最終配布物

最新 Source と同じ `0.1.0-preview.1` を `<TestRoot>/HubPackageValidated` に生成した。
初期の HubPackage1 は UI 修正前のため最終成果物として利用しない。
Self-contained Publish と Velopack Setup / Full Package / Portable の生成に成功した。
生成後の署名、Full Package と Feed の SHA-256 / サイズ、同梱の信頼公開鍵一致を再検証した。
記録は `out/validation/HubPackageValidatedBuild.log`。

## 修正した起動不具合

初期の実画面確認で ProgressBar.Value が既定 TwoWay Binding として解釈され、
読み取り専用 ViewModel.Progress への Binding により起動が失敗した。
Value を OneWay とし、Core だけでは拾えない問題を検出する WPF STA 回帰 Test を追加した。
Window Title だけでは同名の起動失敗 MessageBox を成功と誤判定するため、
実画面の Projects / Engines / Settings を確認する。
最終の手動実画面確認は Computer Use の承認待ちが時間切れとなったため未実施。
WPF STA の実 Window / 全 Tab 生成 Test と、上記の実 Update.exe 適用・再起動記録を根拠とする。

## 未実施

GitHub 実 Release の公開・オンライン更新、別 PC のインストール、本番 Authenticode 証明書、
OS 再起動 / 電源断を挟んだ復旧は未検証。署名 Secret がない公開は成功扱いしない。
