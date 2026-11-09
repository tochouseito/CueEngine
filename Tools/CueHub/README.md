# CueEngine Hub

Windows x64 / C# / .NET 10 / WPF の Project 管理アプリ。
View は `Views`、画面状態は `ViewModels`、File / Process / Network の契約は `Core` に分離する。
Hub / Packaging の C# Project に限り NuGet Restore を許可する。
WPF Binding と .NET Interface の公開 Property は .NET の PascalCase を使う。

画面左側の「プロジェクト」「インストール」「設定」で表示を切り替える。
プロジェクト画面は Hub に保存された登録一覧を表示する。右上の「追加」で既存の `.cueproj` を登録し、
「新しいプロジェクト」で一覧から専用の作成ページへ切り替える。
「使用する Engine」は作成ページの入力項目とし、インストール画面で導入したバージョンを選択する。
インストール画面は導入済み Engine をカードで表示し、検索と「すべて／正式リリース／Preview」で絞り込む。
右上の「リストに追加」からローカル Engine 登録またはパッケージ導入を行う。
「Engine をインストール」は公開版の選択ウィンドウを開き、正式版／Preview を選択して導入する。
修復・アンインストール・登録解除・保存場所の表示は、各 Engine の「管理」メニューから行う。
検索・絞り込みは新規 Project 作成用の Engine 候補を変更しない。
既存 Project を開くときは Project が指定する版を自動選択し、作成用の Engine 選択とは分離する。
作成に失敗した場合はページと入力を保持し、成功した場合は一覧へ戻って作成した Project を選択する。
「プロジェクト一覧へ」または「キャンセル」でも一覧へ戻る。

## Visual Studio から起動

Visual Studio 2026 に .NET デスクトップ開発環境と .NET 10 SDK を用意する。

Engine の Configure 後に `out/build/windows-vs2026/CueEngine.slnx` を開き、`CueHub` を右クリックして
「スタートアップ プロジェクトに設定」を選ぶ。F5 でデバッグ起動、Ctrl+F5 で通常起動できる。
Hub だけを開く場合は `Tools/CueHub/CueHub.csproj` を直接開く。
生成された Solution を手で編集する必要はない。CMake が外部 C# Project として登録する。

Hub は個別にビルドし、必要な NuGet パッケージを lock ファイルに従って復元する。
Debug は Debug、Development / Release は Release として Hub をビルドする。
UI の確認はインストーラーの再生成を経由せず、この起動経路で行う。

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
新規作成は C++ ProjectTool に委譲し、空 Project を作成する。Hub にテンプレート選択機能は設けない。
公開済み ProjectTool との互換性のため、内部 CLI 呼び出しでは必須の `--template Empty3D` を渡す。
Engine の指定は Project に Version として保存し、PC の絶対 Path は保存しない。
「一覧から外す」は登録解除だけであり、Project File を削除しない。

## Engine 配布物

Hub は GitHub Pages の `distribution/catalog-v1.json` と `.sig` を取得し、既定の発行者公開鍵で検証する。
正式版と Preview を合計して公開日時順の最新 10 世代を表示する。
一覧内の通常 HTTPS URL から GitHub Releases の配布 File を取得し、実行中の Hub は GitHub REST API を使用しない。
Git / GitHub CLI / GitHub アカウント / Token は利用者に要求しない。API を利用するのは公開 CI のみ。
GitHub の共有 IP 単位の匿名 REST 回数制限を通常導入から外すが、通信障害や配信元の制限は発生し得る。

署名付き一覧 Cache は `%LOCALAPPDATA%/CueEngine/Hub/distribution-catalog-v1.cache.json`。
一覧を開くときは署名を再検証した Cache を 30 分利用し、「一覧を更新」でオンライン取得を明示要求する。
取得失敗時は検証済み Cache と警告を表示する。未署名の旧 API Cache は移行元に使わず保全する。
導入済み Engine / Project の登録には影響しない。新方式の初回取得には新 Catalog を公開した Pages が必要。

圧縮 ZIP が 2 GiB 未満なら ZIP 1 個を使う。超過時だけ公開工程で 1 GiB の `.zip.001` 等へ自動分割する。
同じ Release / 同じ Engine バージョンとして扱い、Hub が Part を順番に取得する。
`Downloads/engine-<version>/<SHA256>/<署名された File 名>` に Cache を残し、中断時は次回の導入で途中から再開する。
Server が Range に対応しない場合は先頭から取り直す。個別 SHA-256 / Size と ZIP 全体の SHA-256 を検証してから展開する。
巨大 ZIP の再結合 File は作らず、固定 Buffer と連結読み取り Stream を使う。
取得済み Cache は再利用用に保持し、導入先の空き容量が不足する場合は導入を拒否する。
最大 512 Part、各 Part は 2 GiB 未満、ZIP / 展開容量はそれぞれ 1 TiB 以下。100 GB を実際に転送する負荷試験は別途必要。

11 世代目以前やオフライン導入は GitHub から配布物を取得し、同じフォルダに ZIP または全 Part、
`engine-release.json`、`engine-release.json.sig` を置いて「パッケージから導入」を使う。
分割版ではいずれかの Part を選ぶと署名一覧の名前から全 Part を解決する。欠損や改変があれば展開しない。

署名検証の信頼鍵は Hub 横の `PublisherPublicKey.pem` を既定とする。
Settings で別の発行者鍵を明示設定できるが、パッケージ内の鍵を自動登録しない。
署名は Manifest の元 Byte 列に対する ECDSA P-256 / SHA-256 / P1363 の Base64。
Hash / サイズ / Path / Reparse Point / Zip Slip / 大文字小文字の重複 / 展開上限を検証してから登録する。
使用中の Engine は Windows の共有 Lease により修復・削除を拒否する。

## Hub 自身の更新

Settings の「Hub 更新を確認・準備」で `hub-v` Release だけを調べる。
Hub の候補も署名付き静的 Catalog から選び、Manifest / 署名 / Feed / Full Package を通常 HTTPS URL から取得する。
Full Package は固定 Buffer で Cache へ保存し、中断した取得を再開できる。
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
公開後の取得の任意検証は Test exe の `--distribution <publicKey の絶対 Path> <新しい検証 Root の絶対 Path>`。
専用 Root に公開 Engine を実導入し、Hub Feed を検証する。既存の Hub / Engine 登録には触れず、更新適用も行わない。
製品 Main には検証専用の起動引数を追加しない。

インストーラー生成は `Tools/Packaging/Hub/README.md` を参照。
本番証明書 / GitHub Secret の設定、Tag Push / Release 公開はローカル検証と区別する。
Engine / Hub 公開後は reference-site Workflow が Catalog を再生成・署名して Pages へ配置する。
無料の既存 GitHub 配信経路を利用する設計であり、100 人同時取得の速度・帯域・可用性は保証しない。
