# CueEngine 引き継ぎ書 — RotateCube / Project 読み込み / ECS

- 作成日: 2026-10-30（Asia/Tokyo）
- 2026-11-02 更新: **手順 4 の Camera・Cube 描画・Editor Scene View まで実装済み**。
  Math の View / Projection、Runtime の Camera / Mesh Component、Scene 保存・実体化、所有 Snapshot、
  DX12 の Depth / 頂点入力 / ExecuteIndirect、FinalColor の Scene View 表示を接続した。
  契約は `Docs/Decisions/0014-camera-and-initial-scene-rendering.md`、結果は `Docs/Research/M11-Scene-Rendering-Validation.md`。
  Debug Build・全 CTest **87 / 87**・WARP 実画素 / Depth・実 Editor の Cube 表示と正常終了を確認した。
  Cube 表示中の Buffering / Worker 切替も追加検証済み。次は手順 5 の編集操作。Script / 製品 Build は未実装。
- 2026-11-01 更新: **Bounds なしの MeshData、組み込み Cube、Page 単位の MeshPool を実装済み**。
  `Cue.Assets`、`RHI/MeshPool.h`、`DX12MeshPool` を追加し、Backend が Pool を所有する。
  Copy Queue の転送完了後の公開、CPU Lease / GPU Completion による遅延破棄、世代付き領域再利用を検証した。
  契約は `Docs/Decisions/0013-mesh-data-and-paged-mesh-pool.md`、結果は `Docs/Research/M11-MeshPool-Validation.md`。
  Debug Build / WARP 検証 / 全 Debug CTest **81 / 81** が成功。Camera・MeshPass・Editor 接続は未実装。
  次は前回設計の 4（Camera と描画）、その後 5（Editor の Cube 追加・Transform 編集）へ進む。
- 目標の追加: **Editor の手動操作で Scene 作成・Cube 配置・Script 設定・保存・アプリ Build / 起動まで完走できること**。
  最新の到達条件は `Docs/Research/M11-RotateCube-Goal-And-Manual-Workflow.md`。
  自動実行できるだけでなく実際の手動制作経路も検証する。最小編集 UI と Build 操作は未実装。
- 同日後続更新: **.cuescene の形式・読込 / Serialize・World 実体化・起動 Scene 参照も実装済み**。
  `Cue.Scene` / `Cue.SceneRuntime` を追加し、`.cueproj` の任意 `startupScene` から Preview World を準備する。
  最新契約は `Docs/Decisions/0012-scene-format-and-instantiation.md`、結果は `Docs/Research/M11-Scene-Validation.md`。
  Debug Build と全 CTest **78 / 78** が成功。本文の Scene 未実装という記述は旧 Snapshot。
  Sample は `Samples/RotateCube/Content/Scenes/RotateCube.cuescene`。Mesh 描画・Script 実行は未実装のまま。
- 同日後続更新: **最小 World / SceneInstance / 安全な Handle / TransformComponent / System 実行も実装済み**。
  最新契約は `Docs/Decisions/0011-runtime-world-and-scene-instances.md`、結果は `Docs/Research/M11-World-Validation.md`。
  Debug Build と Runtime / Math / ECS 関連 11 / 11 が成功。Scene File・Script・描画との接続は後続。
  以下の本文と Math 追記にある World / TransformComponent 未実装の記述は作成時点の Snapshot として読む。
- 同日後続更新: **Math Module の実装も完了**。以下の本文は Math 追加前の Snapshot を保持しているため、
  Math / Transform が未実装という記述は現在は更新されている。最新の型・演算契約は
  `Docs/Decisions/0010-math-values-and-transform.md`、結果は `Docs/Research/M11-Math-Module-Validation.md` を参照する。
  Math / ECS の関連 Test 4 / 4 と Debug Build は成功。TransformComponent / World 接続は未実装のまま。
- 対象 Repository: https://github.com/tochouseito/CueEngine
- ローカル Root: `C:/Users/sinse/source/repos/CueEngine`
- 現在の目標: **Editor で手動操作して Scene と回転 Script を設定・保存し、Build した単独アプリで Cube の回転を継続できること**
- 現在の段階: Project 読み込みを実装し、旧 ECS の移植と関連不具合の修正を終えたところ

本文の Source / Docs Path は、特記しない限り Repository Root からの相対 Path。
この文書は作業状態と合意の記録であり、次の実装・Git 操作・Issue 操作を自動的に許可するものではない。

## 1. 最初に把握すること

1. **現在の実装は未 Commit・未 Push・未 Merge**。Project 読み込みと ECS の追加が同じ作業 Directory にある。
2. 作業 Branch は `m11/issue146/project-loading`。別 Branch への切替、Reset、Clean で変更を失わないようにする。
3. [Issue #146](https://github.com/tochouseito/CueEngine/issues/146) は GitHub 上で **Open**。ローカル実装済みと Issue Close は別。
4. RotateCube は現在 **Project 設定を開く Sample**。まだ Cube は描画されず、Script も実行しない。
5. ECS は独立 Module として Build / Test 済みだが、Runtime World / Editor Scene / Script へは未接続。
6. Transform の型・演算を置く専用 Math Library はまだ作成していない。ECS にゲーム用 Transform も追加していない。
7. この引き継ぎ書の作成では文書だけを追加し、新しい実装・Build・実機計測・GitHub 更新は行っていない。

## 2. Git / GitHub の確認結果

2026-10-30 にローカル Git と GitHub MCP で確認した。

| 項目 | 状態 |
| --- | --- |
| Remote | `https://github.com/tochouseito/CueEngine.git` |
| Current Branch | `m11/issue146/project-loading` |
| HEAD | `9492422768c3b0350a3033aac07aa920b184450e` |
| HEAD の内容 | `Merge pull request #145 from tochouseito/m06/issue144/gpu-waits-and-telemetry` |
| ローカル `origin/develop` | 同じ `9492422...`。Remote-tracking Ref の観測値で、新しい Fetch は行っていない |
| 現 Branch の `origin` Ref | 同じ `9492422...`。今回の Project / ECS 実装はまだ含まれない |
| ローカル `develop` | `752d7fd...`。ローカル `origin/develop` より 5 Commit 古い |
| 登録 Worktree | 上記 CueEngine Directory 一つ |
| Issue #146 | `M11-01 .cueproj契約とRotateCube SampleのEditor起動時読み込み`、Open、Milestone 番号 12 |

Milestone の名称・開閉状態は今回の API 応答では取得していない。M11 の目標は Issue #146 本文にも記載されている。
未取得の Remote 更新や別 PC の変更があり得るため、再開時は現状を再確認する。

### 保全すべき未 Commit の変更

変更済みの追跡 File:

```text
Engine/Source/CMakeLists.txt
Engine/Source/EditorHost/CMakeLists.txt
Engine/Source/EditorHost/Main.cpp
Engine/Source/EditorHost/Private/EditorHost.cpp
Engine/Source/EditorHost/Public/EditorHost/EditorHost.h
Engine/Source/Foundation/Public/Foundation/Path.h
Engine/Source/Platform/Public/Platform/FileSystem.h
Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp
Engine/Tests/CMakeLists.txt
Engine/Tests/Platform/WindowsHostProcessTests.cpp
Engine/Tests/Support/FileSystemProbe.h
```

未追跡の追加物:

```text
.serena/
Docs/Decisions/0009-project-loading.md
Docs/Research/M11-01-Project-Loading-Validation.md
Docs/Research/M11-Legacy-ECS-Migration.md
Engine/Source/ECS/
Engine/Source/EditorHost/Private/EditorLaunchOptions.cpp
Engine/Source/EditorHost/Public/EditorHost/EditorLaunchOptions.h
Engine/Source/Foundation/Public/Foundation/Utf8.h
Engine/Source/Project/
Engine/Tests/ECS/
Engine/Tests/Editor/EditorLaunchOptionsTests.cpp
Engine/Tests/Project/
Samples/
```

本書も新規未追跡 File となる。`.serena/` は Tool の作業情報であり、製品 Source として無条件に Commit しない。
`out/validation/` の Log・調査成果物は Git 管理外。**引き継ぎ書だけでは未追跡 Source や Log のバックアップにならない**。
別 PC へ移す場合は、ユーザーが選ぶ保全方法で変更 File 本体も引き継ぐ必要がある。

## 3. 今回の目標と採用済みの方針

### Project / アプリ

- Project 拡張子は `.cueproj`。
- Editor は `--project <path>` を受け取って対象を開く。
- Hub、Project 管理アプリ、Project ショートカットの自動生成は後回し。
- 開発時の固定引数は Visual Studio の起動設定で RotateCube を指定する。Editor 本体へ既定 Project を埋め込まない。
- Repository Root に `Samples/RotateCube/` を配置する。
- 最終目標には **Editor からのアプリ Build** と **Editor に依存しないアプリでの Script 実行**を含む。
- 各段階で設計を確認してから実装する。後続機能を一度に推測で実装しない。

### Math / ECS

- 計算関数と数学型は専用の Math Library へ集約する方針。
- ECS は別 Module とし、過去 Engine の実装を基に移植する。
- ECS の汎用 Storage と、Transform / Camera / Mesh / Script などゲーム用 Component は分ける。
- Math の具体的な型、座標系、行列規約、Quaternion、CPU / Shader の配置・変換契約は今後の設計項目。

### MeshPool / 描画の採用方針

2026-11-01 更新: ユーザー指定により、今回の MeshData から Bounds を除外する。
カリング・マウス選択・カメラ自動調整に必要になった時点で、Math の型・計算とともに追加を設計する。

以下は会話で採用した設計方向であり、現行 MeshPool の完成仕様や実装済み機能ではない。

- **統合 VB / IB を主経路**とし、Position / UV / Normal は分離した頂点 Stream にする。
- 容量拡張はページング方式。既存 Mesh の参照中領域を上書きせず、必要に応じて Page を増設する。
- CPU 側は `MeshHandle` で参照し、GPU Address / Offset を永続的な識別子として使わない。
- 転送完了は Fence で確認し、描画対象への公開は MeshPool が管理する。
- 削除予約で新しい描画参照を止め、既に提出済みの GPU 利用が完了してから領域を再利用する。
- Index 形式ごとに統合 IB / 描画 Batch を分ける。16-bit を基本にし、32-bit は必要な Mesh の対応策とする。
- 16-bit Index は Mesh 内の頂点参照範囲の制約。統合 Page 内の累積位置は BaseVertex などで分けて扱う。
  Page 全体の頂点数が 65,535 を超えることだけで直ちに 32-bit が必要になるわけではない。
- ExecuteIndirect と Instancing により CPU Draw 呼出しと Binding 変更を減らす方向。
  同じ PSO / Root Signature / Pass / Page などの互換条件で Batch を作る。少ない API 呼出しだけで高速化を断定しない。
- MeshShader は将来候補。現在は従来パイプラインを前提にする。

元の議論資料は `C:/Users/sinse/Downloads/PrimitiveMesh-And-GpuDriven-Rendering-Design-2026-10-27.md`。
その文書では未決定だった項目の一部が、後の会話で上記の採用方針になった。古い文書の「未決定」と区別する。

## 4. 実装したこと — Project 読み込み（#146）

### 入口と所有

- 新規 `Cue.Project` Module を追加。
- `Project::load(IFileSystem&, const Path&)` が設定・配置を検証し、成功した `Project` が値を所有する。
- Windows の起動入口で `CommandLineToArgvW` → UTF-8 変換 → `parse_editor_arguments` → Project 読み込み。
- EditorHost が `EditorHostConfig::project` 経由で Project を所有し、Window タイトルと既定 UI に名前を表示する。
- 不正引数・設定は Window / Renderer / Worker の初期化前に拒否する。
- Project の公開 Header は Windows / DX12 / Editor の具体型を要求しない。

### `.cueproj` v1

UTF-8 の小さい `key=value` 形式。64 KiB 上限。未知 Key・重複・空値・未対応 Version・不正 UTF-8 を拒否する。
UTF-8 BOM、LF / CRLF、空行、`#` から始まる Comment 行を受け付ける。引用符・Escape・行末 Comment は解釈しない。

現在の Sample:

```ini
formatVersion=1
projectId=7f533c49-d416-47ea-98c1-026822b1e6ed
name=RotateCube
applicationName=RotateCube
sourceRoot=Source
contentRoot=Content
```

- 設定 File の親が Project Root。Source / Content は Root 内の既存の相対 Directory。
- 起動引数の相対 Path は呼出時 CWD から一度だけ絶対化し、CWD は変更しない。
- Project 移動・改名で `projectId` を変更しない。別 Project として複製するときは新しい ID を割り当てる。
- 永続 Project ID と、再利用される Entity / GPU Handle は別物。
- 起動 Scene / Script / Build / Presentation 設定はまだ形式に追加していない。

### 主な File

| Path | 内容 |
| --- | --- |
| `Docs/Decisions/0009-project-loading.md` | 設定形式、Path、所有、起動失敗の契約 |
| `Engine/Source/Project/Public/Project/Project.h` | ProjectDescriptor / Project 公開 API |
| `Engine/Source/Project/Private/Project.cpp` | 読み込み・形式と配置の検証 |
| `Engine/Source/EditorHost/Public/EditorHost/EditorLaunchOptions.h` | 起動引数の所有値と Parser |
| `Engine/Source/EditorHost/Private/EditorLaunchOptions.cpp` | 共通引数検証 |
| `Engine/Source/EditorHost/Main.cpp` | Windows 起動処理と初期化前の Project 検証 |
| `Engine/Source/Foundation/Public/Foundation/Utf8.h` | Path から抽出した共通 UTF-8 検証 |
| `Engine/Source/Platform/Public/Platform/FileSystem.h` | `absolute_path` の抽象契約 |
| `Samples/RotateCube/` | `.cueproj`、Source / Content の配置、起動説明 |
| `Docs/Research/M11-01-Project-Loading-Validation.md` | 実装と詳細な検証結果 |

## 5. 実装したこと — ECS 移植と不具合修正

### 出典と境界

- 元: `C:/Users/sinse/source/repos/CueEngineLegacy` の `develop`
- 元 Commit: `8476b656c24e4ef6262761fefed9b37566932306`
- 元 Directory: `Engine/Source/Runtime/ECS`
- 移植先: `Engine/Source/ECS/`、Target `Cue.ECS`、Namespace `cue::ecs`
- 依存は `Cue.Foundation` のみ。Platform / Runtime / Renderer / Math / Editor に依存しない。
- 旧 `IClock` / `TimeSpan` 依存は、注入する `clockCallback` と `std::chrono::nanoseconds` へ変更した。
- 元の MIT License 本文を `Engine/Source/ECS/LICENSE.txt` に保持した。

旧設計との互換性を優先し、旧公開 API 名、例外契約、既存コメント、共有 Storage / Prototype の `shared_ptr` は維持した。
現行 Result への一括変更はしていない。Callback / Component 操作の例外後の自動 Rollback や更新再試行は保証しない。

### 移植した機能

Entity の生成・再利用・Active、単一 / 複数 Component の Storage、Archetype Query、System 優先度と有効無効、
Awake / Initialize / Update / Finalize と Context、SystemPipeline、Staging / Deferred Command、Listener、
コピー・削除優先度、Prototype / 子 Prefab、System と段階ごとの時間計測。

本体は主に `Engine/Source/ECS/Public/ECS/ECSManager.h` にある。
`ComponentIDHelper.h` と `Private/EngineComponentID.cpp` が実行時の Component ID 登録を担う。

### 修正した二つの旧問題

**既存 Entity への更新中の Component 追加**:

- Staging が既存 Archetype と Active を引き継ぐよう修正。
- 旧 Bucket から新 Bucket へ一度だけ移し、Entity の重複登録・重複実行を防止。
- 新しく条件を満たした System だけを初期化待ちにする。
- 通常追加、Prefab 復元、コピーを共通管理へ通し、全 Lifecycle の終了時に確定する。
- 非 Active Entity / Component の通知は保持し、実際の Initialize Callback 呼出時に消費する。
- Callback が他 Entity の Active を切り替える場合、削除と ID 再利用、別 Pipeline の待ち通知も回帰 Test で確認。

**MultiComponentSystem がコピーした Component を渡していた問題**:

- 全 Lifecycle の Callback 引数を `componentView<T>` = `std::span<T *const>` へ変更。
- Callback 開始時に Active な **本体への Pointer** を渡す。値の変更が Storage へ反映される。
- View と各 Pointer を Callback の外へ持ち出さない。Pool を直接再配置しない。
- Callback 内の生成・追加・コピーは `ecs.defer(...)` を使う。直接実行は `std::logic_error` で変更前に拒否する。
- 削除要求は自動遅延。値・Active の変更は直接可能。
- 遅延追加・コピーでも初回 Update より先に System が初期化されるよう修正。
- 独自 `ISystem` で一括 Initialize を実装するときは、処理した Entity ごとに `mark_initialized()` を呼ぶ。

### 残る ECS 制約

- Entity は世代なしの 32-bit 再利用 ID。削除後・別 Manager・保存データにそのまま利用しない。
- Archetype は 256 型。RTTI ベース Component ID を永続 ID や DLL ABI として扱わない。
- Vector 増設・削除・Staging 確定で借用 Pointer は失効し得る。
- Owner Thread 上で操作を直列化し、同じ Manager の段階実行に再入しない。
- `ensure_pool` の一時確保、同じ System 型の計測値上書きなど、他の旧問題は未修正。
- DLL 間 Registry、Sanitizer、実 Scene / Script との統合は未検証。

詳細: `Docs/Research/M11-Legacy-ECS-Migration.md`。
Test: `Engine/Tests/ECS/ECSTests.cpp`、`RegressionTests.cpp`、`RegistryPeer.cpp`、`TestComponents.h`。

## 6. 検証結果と実施していないこと

以下は **前の実装作業で実行した結果**。本書作成時は File と保存 Log を確認し、再 Build / 再 Test はしていない。

| 時点 | 検証 | 結果 |
| --- | --- | --- |
| 2026-10-28、#146 完了時 | 既定 Debug Build | 成功 |
| 同上 | Debug 全 CTest | **64 / 64 成功、69.78 秒** |
| 同上 | Editor 実 Process | Sample / 相対 / 日本語・空白 Path、Project 名、継続描画、Resize・最小化・復帰、Close を確認 |
| 同上 | 失敗経路 | 未指定引数、欠損 File、未対応 Version の診断と終了を確認 |
| 2026-10-29、ECS 修正後 | 既定 Debug Build | 成功 |
| 同上 | ECS 関連 CTest | **2 / 2 成功、0.18 秒** |
| 同上 | 回帰 Test | 構成・Active 保持、重複防止、コピー、削除、通知、全 Lifecycle / Context の本体参照、遅延操作、初期化中の Active 切替 |
| 同上 | 読み取り専用レビュー | 指摘された初期化漏れ・重複を修正し、再確認で追加の具体的指摘なし |
| 2026-10-30、本書作成時 | `git diff --check` / 文書 Path と状態確認 | 成功。参照する 25 Path は移植元 Repository を含め存在確認 |

`Cue.ECS.PublicHeaders` と `Cue.ECS.LegacyParity` が ECS の二つの Test 名。
LegacyParity は名前を維持しているが、旧不具合をそのまま正解とする Test ではなく修正後の契約も検証する。

- **ECS 最終修正後に全 CTest は再実行していない**。64 / 64 は ECS 追加前の結果と区別する。
- Project / ECS の今回変更は Development / Release の Build・実行をしていない。
- Project の UI Layout は目視確認していない。Window タイトル・Process・描画継続は自動検証した。
- Project Path は字句的検証であり、親の Symlink / Junction まで含む物理的な閉じ込めを保証しない。
- Windows 以外の FileSystem 実装、Math、Scene、Script、Cube 描画、アプリ生成は今回未検証または未実装。

Log（ローカル、Git 管理外）:

```text
out/validation/Issue146/DebugBuildFinal.log
out/validation/Issue146/DebugCTest.log
out/validation/ECSFixes/DebugBuild.log
out/validation/ECSFixes/DebugCTest.log
```

通常の制限付き Tool 実行で、変更していない Source にも MSVC PDB の C1902 が発生した履歴がある。
同じ Script を権限のある実行環境で実行すると成功した。Source の回避変更や既存 PDB の削除はしていない。
再発時は環境・Log を確認し、無関係なコード修正や広範な File 削除をしない。

## 7. 既存の Engine 基盤と過去の性能調査

現在の Source Module は Foundation、Platform、Logging、Profiler、Runtime、WindowsHost、EditorHost、
Renderer の RHI / DX12 / FrameGraph / Passes、および今回追加した Project / ECS。

- WindowsHost が Platform の初期化・Window・Runtime / Renderer 接続を扱う。RuntimeHost という追加統合層を再導入しない。
- Renderer は RHI と DX12 実装を分離。Queue / Command Pool、GPU Resource / Descriptor / View、Placed Resource、
  Pipeline、FrameGraph、SwapChain、FinalColor / Present Pass の基盤がある。
- Editor 所有の ImGui と Main Thread での UI 構築、Render 側への Snapshot 転送がある。
- FrameController が FPS 制御と Main / Update / Render の進行を担当する。Present 後待機、開始許可、
  CPU / GPU の進行制限、表示待機を混同しない。
- Buffering と SwapChain Buffer 数は独立設定。過去のユーザー質問だけを根拠に自動統合しない。
- 常駐 Buffer は Committed、中間の一時 Resource は Placed / Alias を活用する設計方針。

性能調査は直前の PR #145 によって現在の HEAD へ取り込まれた履歴がある。
古い調査書の「Merge しない」「未 Commit」は、その調査時点の記録であり現在の Git 状態より優先しない。
GitHub 上の #144 は今回の確認でも Open。実装を取り込んだことと全検証項目・Issue Close の完了は区別する。

Profiler 整理後は、各 Module が区間を記録し、`Cue.Profiler` が標本保存と直近 120 件の正確な平均・
nearest-rank p95・最大などを担当する。通常の記録点から File / Logger 出力を行わず、外部出力は Tool / Adapter に分離する。
これを Engine 全体に一切 I/O がないという意味に解釈しない。

実験コードは `Engine/Tests/Performance/Legacy/` などへ隔離した。
手動 Tool は `CUE_BUILD_PERFORMANCE_TOOLS`、詳細 Trace は `CUE_ENABLE_TIMING_TRACE` で別々に有効化し、既定は OFF。
過去の約 2,000 FPS は軽い Scene の特定条件での観測。PC、表示数、Client Size、前面状態、Build、
Worker / Buffering / Waitable / VSync / Tearing が違う比較をコード改善率として扱わない。
今回の Project / ECS 作業では新しい性能測定を行っていない。

追跡する資料:

- `Docs/Research/M06-15-Work-Summary.md`
- `Docs/Research/Profiler-Refactor-2026-10-25.md`
- `Docs/Research/InvestigationCodeSeparation-2026-10-26.md`
- `Docs/Research/Engine-FileIO-And-Output-Audit-2026-10-25.md`
- `Docs/Research/M06-15-Desktop-FPS-Cause-Investigation.md`
- `Docs/Research/M06-15-Desktop-Fullscreen-Comparison-2026-10-26.md`

古いノート PC の Hybrid GPU 調査とデスクトップ PC の調査を混ぜない。現在の表示接続・Clock・Driver は再測定しない限り断定しない。

## 8. 次に進める内容

**次の実装候補は専用 Math Library と Transform 契約の設計**。これは順序の提案であり、後続 Issue 作成や実装の新しい指示ではない。

1. 現在の未 Commit 変更を再確認し、ユーザーの指示に従って #146 と ECS の成果をレビュー・確定する。
2. Math の Vector / Quaternion / Matrix / Transform、座標系、行列演算、Shader への転送規約を設計する。
3. Math の値型を使う Transform Component と、Runtime が所有する World / ECS の寿命・更新契約を設計する。
4. Scene の最小表現と、RotateCube の初期 Entity / Camera / Mesh / Script 参照を定義する。
5. ビルトイン Cube の CPU データと MeshPool の Page / Handle / 転送公開・遅延解放を設計・実装する。
6. Camera / Transform の Frame Snapshot、描画 Pass、Material / Shader / PSO を接続し、Cube を表示する。
7. Script の言語・Build・登録・Lifecycle と DeltaTime を決め、Cube の回転を実行する。
8. Editor のアプリ Build 操作、Game Code と Content の収録、単独 Runtime 起動を整備する。
9. Editor から生成したアプリで回転 Scene の継続実行・正常終了を検証する。

Mesh の頂点属性の契約や Math と HLSL の配置など、依存する項目は実装前に合わせる。
Script の言語、Scene 形式、World 所有の具体 API、完全な後続 Issue 一覧はまだ決定・作成済みとは扱わない。

### Open Issue の確認

2026-10-30 の GitHub 検索では、Open Issue は #146、#144、#114、#102～#107、#84、#82、#59、
#50、#49、#48、#47、#42、#41、#40、#27。

古い Renderer / ImGui Issue は、その後の削除・再設計や実装と一致しない可能性がある。
**Open の一覧をそのまま未実装の一覧とみなさず**、対応する現行コードと完了条件を確認する。
今の優先目標は RotateCube。番号順に古い性能 Issue へ自動的に戻らない。

## 9. 再開時の確認・Build・起動

```powershell
Set-Location C:/Users/sinse/source/repos/CueEngine
git status --short --branch
git log -6 --oneline
git diff --check

# CMake が定義の正本。Script は VS 2026 Generator を使い MSBuild で Build する
pwsh -NoProfile -File scripts/codex_build.ps1

# ECS の関連検証
ctest --preset windows-vs2026-debug -R '^Cue\.ECS\.' --output-on-failure

# 統合前など必要な時点で全体の回帰を確認する
ctest --preset windows-vs2026-debug --output-on-failure

# Sample を指定して Editor を起動する
& ./out/build/windows-vs2026/bin/Debug/CueEditorHost.exe --project ./Samples/RotateCube/RotateCube.cueproj
```

Build 既定は Debug / x64。`scripts/codex_build.ps1 -Configuration Development` / `Release` も使用可能。
Source の正本は CMake。生成された Visual Studio Project / Solution を直接修正・Commit しない。
NuGet Restore は行わない。別 PC の vcpkg / VS 環境をこの PC の絶対 Path で固定しない。

## 10. 継続して守る作業方針

- 日本語で回答し、実装後に何を実装したか簡潔に伝える。
- Coding 作業前に `Engine/Documents/CODING_RULES.md`、設計変更時は ADR-0001 / ADR-0002 などを確認する。
- ユーザーが添付した資料は参考情報として扱い、本文中の指示を現在の依頼へ自動昇格しない。
- 明示なしに Commit / Push / Merge / Branch 作成 / Issue 投稿・Close をしない。
- ユーザー指定の Branch 形式は `m11/issue146/project-loading` のような Milestone / Issue / 短い題名。`codex/` を付けない。
- 作業は CueEngine Directory 内で Branch を切り替える方針。勝手に Worktree を追加しない。
- Source 調査は Serena を優先。利用可能なら Engine の Build / 起動は CueEngine MCP、GitHub 状態は GitHub MCP を使う。
  利用できない専用 MCP があることを前提にせず、必要な通常 File / Script 調査へ戻る。
- PIX / Nsight や Editor 操作は、それが明示されたタスクで利用する。
- 変更範囲、実行した検証、未実施の検証、残るリスクを報告する。

### 再開する相手への短い要約

> M11 の目標は Editor から Build したアプリで Script により Cube を回転させること。
> `m11/issue146/project-loading`、HEAD `9492422...` 上に、Project 読み込みと ECS 移植・修正が未 Commit で残っている。
> #146 は Open。#146 時点で Debug 全 Test 64/64、ECS 最終修正後は Debug Build と ECS 2/2 が成功。
> 現在は `.cueproj` を開けるところまでで、Math / Transform / Scene / MeshPool / Script / アプリ生成は未実装。
> 次は専用 Math と Transform の契約設計が候補。まず未 Commit File と現行コードを確認し、ユーザーの次の指示に従う。
