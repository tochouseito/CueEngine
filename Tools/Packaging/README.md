# Engine / Hub の配布

Version の正本は `Engine/Version.json` と `Tools/CueHub/Version.json`。
Hub は .NET 10 / WPF、配布 Engine は Development / x64 とする。
初期 SDK はゲームを Source から Build する方式であり、利用者の Visual Studio / Windows SDK / CMake が必要。
Toolchain 本体、Microsoft Debug CRT、PIX Tool は同梱しない。

## ローカル生成

秘密鍵は Repository 外で管理する ECDSA P-256 PEM を使う。
署名対象は Manifest の元 Byte 列、署名形式は SHA-256 / P1363 とする。
公開鍵は Hub に同梱し、自己申告されたダウンロード先の鍵を信頼しない。

```powershell
pwsh -NoProfile -File Tools/Packaging/Engine/New-EnginePackage.ps1 `
    -SigningKeyFile C:/Private/publisher-private.pem `
    -OutputDirectory C:/Output/CueEngine-0.1.0-preview.1

pwsh -NoProfile -File Tools/Packaging/Hub/New-HubPackage.ps1 `
    -SigningKeyFile C:/Private/publisher-private.pem `
    -TrustedPublisherKeyFile C:/Private/publisher-public.pem `
    -OutputDirectory C:/Output/CueHub-0.1.0-preview.1
```

出力先は未作成の Directory に限定する。Engine は公開時に clean checkout を要求する。
作業中のローカル試験だけ `-AllowDirtyLocal` を指定でき、その Build ID は `-dirty` を含む。
`-SkipBuild` は既に作成した専用配布 Tree の再梱包用で、通常の Debug Tree は使えない。
Hub の `-VersionOverride` はローカル更新試験用であり、公開 Version の正本を変更する手段ではない。

Engine 出力は署名付き Manifest と ZIP、Hub は `Release/` に Setup / Full Package / Feed / 署名付き Manifest を持つ。
License / Third-party Notice は実際の Payload に対応するものを収録する。
詳細は [配布契約](../../Docs/Decisions/0021-hub-and-engine-distribution.md) と
[Hub パッケージ生成](Hub/README.md) を参照する。

## Tag から公開

`.github/workflows/tagged-release.yml` は `engine-v<version>` / `hub-v<version>` の Push を受ける。
定義 Version との一致、Build / Test、署名・Hash を検証し、Draft に Asset を添付してから公開する。
Preview は GitHub Pre-release にし、既存 Tag の Release の上書きを拒否する。

実行前に GitHub Actions Secret `CUE_PACKAGE_SIGNING_KEY_PEM` を設定する。
公開鍵はその鍵から生成し、Hub の信頼設定に同じ鍵を使う。鍵のローテーションは既存 Hub の信頼更新と合わせて計画する。
`GITHUB_TOKEN` の Release Job だけ `contents: write` を持つ。秘密鍵は一時 File として作成し、終了時に除去する。
Tag の作成・Push・実 Release 公開は明示的なユーザー操作として扱う。

本番 Authenticode 署名と配布サイトは後続 M12 の対象。Manifest 署名と Windows 実行 File の証明書は別の仕組み。

## 検証

```powershell
pwsh -NoProfile -File Tools/Packaging/Scripts/PackageCommonTests.ps1
pwsh -NoProfile -File Tools/Packaging/Scripts/TaggedReleaseTests.ps1
dotnet restore Tools/CueHub.Tests/CueHub.Tests.csproj --locked-mode
dotnet run --project Tools/CueHub.Tests/CueHub.Tests.csproj -c Release --no-restore -- --engine-root C:/Source/CueEngine
dotnet restore Tools/CueHub.UiTests/CueHub.UiTests.csproj --locked-mode
dotnet run --project Tools/CueHub.UiTests/CueHub.UiTests.csproj -c Release --no-restore
```

Tag 公開 Test は GitHub を模擬し、ネットワークへ公開・投稿しない。
実 ZIP の導入、Font / License の同梱、空 Project 作成、CLI 検証、利用中 Lease の確認は
`Engine/Validation/CueEnginePackageProbe.csproj` を使う。

```powershell
dotnet run --project Tools/Packaging/Engine/Validation/CueEnginePackageProbe.csproj -c Release -- `
    C:/Output/CueEngine-0.1.0-preview.1 C:/Private/publisher-public.pem C:/Validation/NewEngineRoot
```

第3引数は存在しない検証先を指定する。`--open-editor` を追加すると製品の ProjectService から Editor を開く。
個々の実測結果と未実施項目は [検証記録](../../Docs/Research/Hub-Distribution-Validation.md) を参照する。
