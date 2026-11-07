# Hub の配布物生成

`New-HubPackage.ps1` は Windows x64 self-contained の Publish を実行し、固定版 Velopack で
インストーラーと Full Package を生成する。NuGet Restore は Hub / Packaging の C# Tools だけで行う。

```powershell
pwsh -NoProfile -File Tools/Packaging/Hub/New-HubPackage.ps1 `
    -OutputDirectory C:/Output/CueHub-0.1.0-preview.1 `
    -SigningKeyFile C:/Private/publisher-private.pem `
    -TrustedPublisherKeyFile C:/Private/publisher-public.pem
```

公開鍵は `PublisherPublicKey.pem` として Hub に同梱する。秘密鍵は出力にコピーしない。
`hub-release.json` の元 Byte 列へ ECDSA P-256 / SHA-256 / P1363 の署名を付ける。
Hub は署名した Feed と Full Package の SHA-256 を検証してから Velopack を呼ぶ。
Hub を公開する Release の Tag は `hub-v<Version>`。Engine Release と同じ一覧から取得しても
Hub Tag / Asset だけを選択する。

初期実装は Full Package 一つの Feed を配布する。署名対象外の Delta は自動適用しない。
利用者の設定はインストール Directory 外の LocalAppData に保存し、Updater が差し替える場所と分離する。
`VersionOverride` はローカルの更新試験専用。CI の Tag と正本 Version の一致検証を代替しない。

## License

Velopack 1.2.161: MIT、上流 Commit `92d6a1c91716729d449034df5c50307dcce39493`。
`Core/packages.lock.json` の net10.0-windows Target は Velopack だけを参照し、推移 Package はない。
Self-contained の Runtime は実際に解決された .NETCore / WindowsDesktop Runtime Pack の
`LICENSE.TXT` と `THIRD-PARTY-NOTICES.TXT` を出力へ収録する。
Windows Runtime を一律 MIT と表示しない。
`Licenses/DotNet-Library-License.htm`、`Windows-SDK-License.html`、
`DotNet-Windows-License-Information.md` は 2026-11-04 に Microsoft の公式配布ページから取得した全文を保持する。
Velopack Vendor Notice は Package Builder 付属の一覧であり、その全 Tool を Hub に同梱する意味ではない。
Builder の WiX / zstd / Linux 用 Tool 自体を Hub Payload へコピーしない。

本番の Authenticode 署名証明書・配布サイトは M12 で扱う。Manifest 署名は Publisher 公開鍵に
対する真正性確認であり、Windows の実行 File 証明書や SmartScreen の表示を代替しない。
