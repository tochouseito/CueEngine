# CueEngine Hub

Windows x64 / C# / .NET 10 / WPF の Project 管理アプリ。
View は `Views`、画面状態は `ViewModels`、File / Process / Network の契約は `Core` に分離する。
Hub / Packaging の C# Project に限り NuGet Restore を許可する。
WPF Binding と .NET Interface の公開 Property は .NET の PascalCase を使う。

## 開発用 Build

```powershell
$env:DOTNET_CLI_HOME = Join-Path $PWD 'out/dotnet-home'
$env:NUGET_PACKAGES = Join-Path $PWD 'out/nuget/packages'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
dotnet build Tools/CueHub/CueHub.csproj -c Debug /p:RestoreLockedMode=true
```

実行 File は `Tools/CueHub/bin/Debug/net10.0-windows/win-x64/CueHub.exe`。
初回は Engines の「ローカル Engine 登録」で checkout のルートを指定する。
同じ `out/build/windows-vs2026/bin/<Configuration>` に Editor と ProjectTool の Build が必要。
開発 checkout は登録解除だけを許可し、Hub は File を削除・更新しない。

## Project

Engine を選択して新規作成、または `.cueproj` を登録する。
登録時は現在導入済み SDK の ProjectTool を読み取り用に利用するため、最初の Engine 登録が必要。
その Reader が対応する形式なら、Project の指定 Engine がまだ未導入でも一覧へ登録できる。
新規作成は C++ ProjectTool に委譲し、Empty3D / RotateCube を利用する。
Engine の指定は Project に Version として保存し、PC の絶対 Path は保存しない。
「一覧から外す」は登録解除だけであり、Project File を削除しない。

## Engine 配布物

公開済みの `engine-v<SemVer>` Release を列挙し、正式版と Preview を合計して公開日時順の最新 10 世代を表示する。
Hub Release / Draft / 必須 Asset の欠損は除外する。取得失敗時は保存済み一覧と診断を表示する。
11 世代目以前の配布物は GitHub から取得し、同じフォルダに ZIP / `engine-release.json` /
`engine-release.json.sig` を置いて「パッケージから導入」を使用する。

署名検証の信頼鍵は Hub 横の `PublisherPublicKey.pem` を既定とする。
Settings で別の発行者鍵を明示設定できるが、パッケージ内の鍵を自動登録しない。
署名は Manifest の元 Byte 列に対する ECDSA P-256 / SHA-256 / P1363 の Base64。
Hash / サイズ / Path / Reparse Point / Zip Slip / 大文字小文字の重複 / 展開上限を検証してから登録する。
使用中の Engine は Windows の共有 Lease により修復・削除を拒否する。

## Hub 自身の更新

Settings の「Hub 更新を確認・準備」で `hub-v` Release だけを調べる。
`hub-release.json` と署名、署名対象の `releases.win.json` と Full Package を検証した後、
ローカルの検証済み Feed を Velopack に渡す。適用直前にも取得済み Package の SHA-256 を確認する。
Engine Release や署名されていない Delta は利用しない。
インストーラーで導入した Hub だけが自己更新でき、直接 Build した exe では理由を表示する。
オフライン更新は Settings に署名済み Feed のフォルダを指定する。
設定は `%LOCALAPPDATA%/CueEngine/Hub`、Engine は既定で `%LOCALAPPDATA%/CueEngine/Engines`。
更新する Hub の Directory とユーザーの Project / Engine は分離する。

## 検証

```powershell
dotnet run --project Tools/CueHub.Tests/CueHub.Tests.csproj -c Debug
dotnet run --project Tools/CueHub.Tests/CueHub.Tests.csproj -c Debug -- --engine-root <checkout の絶対パス>
dotnet run --project Tools/CueHub.UiTests/CueHub.UiTests.csproj -c Debug
```

Core の検証は一時鍵と所有 Temp Root を使う。WPF 検証は STA で全 Tab を実生成し、
Binding Error と起動時 Command の抑止を確認する。
実更新の任意検証は Test exe の `--hub-update <installedRoot> <feed> <publicKey> <currentVersion> <proofFile>`。
製品 Main には検証専用の起動引数を追加しない。

インストーラー生成は `Tools/Packaging/Hub/README.md` を参照。
本番証明書 / GitHub Secret の設定、Tag Push / Release 公開はローカル検証と区別する。
