# ADR-0021: Runtime Application, Service Lifetime, and Play Session Contract

- Status: Accepted
- Date: 2026-09-08
- Decision Owners: CueEngine Project

## Context

M14では、保存済みまたは編集中のSceneから独立したRuntime Worldを開始し、Inputと時間を渡して単一Threadで更新し、
安全に停止した後、同じEditor Processで再びPlayできる最小Runtime Applicationを構築する。

ADR-0016は`RuntimeWorld`と`WorldIdentitySource`、Owner Thread、Structural Safe Pointを決定した。
ADR-0017は`SceneDocument -> SceneSnapshot -> RuntimeWorld + SceneInstance`の一方向実体化と、
`SceneInstance`を`RuntimeWorld`より先に終了する所有順を決定した。ADR-0018は`EditorDocument`を
Authoring状態の正本とし、Runtime Objectを所有しないことを決定した。

現在の`Cue.RuntimeHost`はWindow、D3D12、固定色Clear／Presentを直接組み合わせたExecutableであり、
Input、Clock、Game System、Scene Sessionを共有可能なApplication境界として所有していない。
Editor PlayとStandaloneへ個別のLoopとGlobal Serviceを追加すると、同じProcess内の複数World、開始途中失敗、
停止途中失敗、Editor状態との分離を一つの契約で検証できない。

本ADRはRuntime Application、Process／Project／Session／World Lifetime、Composition Root、Play Session状態、
開始／停止／失敗Rollback、Editorとの依存方向、Headless Test境界を決定する。
Parallel ECS、Job System、Scripting、Game Rendering、Sound、Effect、Physics、Asset Pipeline、Runtime Packagingは決定しない。

## Prior and Legacy Reference

### Current Rebuild Contracts

- `WorldIdentitySource`はProcessで一意所有し、全Worldへ再利用しない`WorldId`を発行するだけとする
- `RuntimeWorld`は生成ThreadをOwnerとし、`request_stop`後の`tick`にあるSafe Pointで終了する
- `SceneSnapshot`はSource Dataを自己所有し、Mutableな`SceneDocument`への参照を持たない
- `SceneInstance`はmove-only所有Handleであり、生存Entityを保持したまま破棄できない
- `EditorDocument`は`RuntimeWorld`、`SceneInstance`、Runtime `EntityHandle`を所有しない
- Runtime Moduleは`Cue.EditorCore`へ依存しない

本ADRはこれらを置換せず、Application Ownerと失敗時の結合順を追加する。

### Legacy CueEngine

旧CueEngineはEditor WorldとPlay Worldを分け、Play開始時にEditor側Worldを複製して実行対象を切り替える短い導線を持っていた。
しかし、Engine全体がEditor／Play Worldと現在World Pointerを所有し、Editor UIから開始／停止へ到達していたため、
複製後の初期化失敗でどのWorldが正本か、どの段階までRollbackするか、Pointerがいつ失効するかを局所的に判断できなかった。

Worldを分ける意図は取り入れるが、Mutable WorldのClone、GlobalなCurrent World、UIからの直接切替は採用しない。
旧Source Code、型、関数、状態遷移はコピー、移植、改名、部分抽出しない。

### TheatriaEngine

TheatriaEngineは上位Engine ObjectがPlatform、Graphics、World、Editor等をまとめて所有し、Editor Toolbarから
Game Run／Stopへ短く到達できる構造を持つ。小規模な制作Loopには理解しやすい一方、上位Objectの責務が増えやすく、
Editor操作、Scene Reload、Runtime Service、World Lifetimeの失敗境界が同じOwnerへ集中する。

明示的な最上位Ownerと短いPlay導線は参考にするが、万能Engine Object、EditorからのRuntime直接操作、
暗黙Scene Reloadは採用しない。Source Codeは比較のためだけに読み取り、CueEngineへ取り込まない。

## Current Requirements

- Editor PlayとStandaloneが同じFirst-party Runtime Sessionを使用する
- RuntimeからEditor、ImGui、Window、D3D12への依存を作らない
- Mutable `EditorDocument`または`SceneDocument`をRuntimeへ渡さない
- Process、Project、Session、Worldの所有者と破棄順を明示する
- 同一Processで複数Sessionを構築してもMutable状態を共有しない
- Global Current World、Global Singleton、無制限Service Locatorを導入しない
- Service依存をConstructorまたはFactory入力で明示する
- Start途中失敗で公開済みRuntime状態と部分Entityを残さない
- Stop途中失敗で未解放所有物を失わず、再Cleanup可能にする
- Owner ThreadとStructural Safe Pointを既存契約のまま維持する
- Window、ImGui、D3D12なしで主要状態遷移をHeadless Testできる
- M14でECS Storage／Query、Renderer、Sound、Effect、Physicsを変更しない

## Reference Engine and SOL-AVES Comparison

| Reference | 参考にする点 | CueEngineでそのまま採用しない点 |
| --- | --- | --- |
| Unreal Engine | `UGameInstance`と`UWorld`の異なるLifetime、PIEごとのGame Instance、Editor内に複数Worldを持つ考え方、SubsystemのScope | `UObject`／GC、暗黙Subsystem生成、任意箇所からのSubsystem Lookup、Actor／World Object Modelを移植しない |
| Unity | Player Loopを順序付きSystem更新として捉え、入力、更新、描画のPhaseを明示する考え方 | StaticなPlayer Loop置換、`MonoBehaviour` Callback順、Domain Reload設定をCueEngineの所有契約にしない |
| Godot | `MainLoop`のInitialize／Process／Finalizeと、`SceneTree`がScene Lifetimeを管理する理解しやすい入口 | Scene TreeとApplication Loopを一つの万能Object Modelにせず、Node継承をRuntime ECSへ強制しない |
| SOL-AVES | Singletonを避け、Constructor InjectionとGlobal／World Scopeを区別し、Editor WorldとPlay Worldを別Containerにする考え方 | 汎用Service Container、型による任意Lookup、自動Object Graph、Job System、Updater Graph、ECS変更をM14へ導入しない |
| Legacy CueEngine | Editor WorldとPlay Worldを分ける意図 | Mutable World Clone、Global Current World、UI直結の切替を採用しない |
| TheatriaEngine | 最上位Ownerと短いPlay／Stop導線 | 万能Engine Owner、RuntimeとEditorのService混在、暗黙Reloadを採用しない |

References:

- [Unreal Engine `UGameInstance`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UGameInstance)
- [Unreal Engine `UWorld`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UWorld)
- [Unreal Engine Programming Subsystems](https://dev.epicgames.com/documentation/en-us/unreal-engine/programming-subsystems-in-unreal-engine)
- [Unity `PlayerLoop`](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/LowLevel.PlayerLoop.html)
- [Unity Managing update and execution order](https://docs.unity3d.com/6000.0/Documentation/Manual/managing-update-order.html)
- [Godot `MainLoop`](https://docs.godotengine.org/en/stable/classes/class_mainloop.html)
- [Godot `SceneTree`](https://docs.godotengine.org/en/stable/classes/class_scenetree.html)
- [CEDiL: 「SOL-AVES」の高性能なランタイムを構成するアーキテクチャ](https://cedil.cesa.or.jp/cedil_sessions/view/3297)

SOL-AVES提供資料のService Container範囲は、Singletonが依存と寿命を隠す問題、Constructor Injection、
Application相当ScopeとWorld Scopeの分離、Editor／Play／StandaloneごとのWorld Service分離を示している。
CueEngineはScoped Lifetimeと明示注入の原則だけを取り入れ、資料のClass、API、Container実装、Data Layoutは使用しない。
並列Main Loop、Job System、Updater Graph、ECSの内容はM14の対象外とする。

## Decision

### Runtime Module Boundary

共有可能なApplication層をFirst-party Target `Cue.Runtime`として追加する。
`Cue.Runtime`は`Cue.Foundation`、`Cue.GameCore`、`Cue.Scene`だけへ依存し、
Platform Event、Clock、Input等の必要値はPlatform非依存Contractまたは注入Adapterを通して受け取る。

依存方向は次のとおりとする。

```text
Cue.RuntimeHost --------------------> Cue.Runtime
       |----------------------------> Cue.Platform.Windows
       |----------------------------> Cue.RHI / Cue.RHI.D3D12
       `----------------------------> existing Clear / Present composition

Cue.Editor.Tool --------------------> Cue.EditorCore
       |----------------------------> Cue.Runtime
       `----------------------------> ToolHost / Platform / ImGui composition

Cue.Editor.ImGui -------------------> Cue.EditorCore
Cue.EditorCore ---------------------> Cue.Runtime
Cue.Runtime ------------------------> Cue.GameCore / Cue.Scene / Cue.Foundation
```

`Cue.Runtime`と`Cue.GameCore`は`Cue.EditorCore`、ImGui、Platform Windows、RHI、D3D12へ依存しない。
`Cue.RuntimeHost`と`Cue.Editor.Tool`は別のComposition Rootだが、同じ`RuntimeApplicationSession`を構成する。
Window Message Pump、Native Handle、D3D12 Device、Swap Chain、PresentはHostが所有し、Runtime Sessionへ入れない。

`Cue.EditorCore`から`Cue.Runtime`への依存はPlay Session Controllerだけに限定する。
既存Editor Document、Command、Save、Files機能がRuntime型を正本として使用してはならない。
これはADR-0018がM12向けに限定した`Cue.EditorCore`依存集合をM14のPlay機能だけ拡張するAmendmentであり、
Editor Command、Persistence、Project Hubの依存辺は変更しない。

### Lifetime Scopes

Lifetime Scopeを次の順序で定義する。

```text
Process Scope
  `-- Project Scope [0..N]
        `-- Runtime Application Session [0..N]
              `-- Runtime World + Scene Instance [0..1]
                    `-- Frame Values [transient]
```

親Scopeは子Scopeより長く生存する。子から親への参照は非所有かつ不変Contractとし、親から子をGlobal Registryで検索しない。

| Scope | 所有するもの | 所有しないもの |
| --- | --- | --- |
| Process | `WorldIdentitySource`、Sealed `SchemaRegistry`、`AssertContext`とEmergency Handler、Process共通の不変Factory定義 | Current World、EditorDocument、Session-local Input／Clock／System状態 |
| Project | 検証済みProject Identity、Runtime設定Snapshot、Startup Scene選択、Projectに適用する不変System Factory集合 | Window、RuntimeWorld、SceneInstance、Editor Selection、Mutable Authoring Data |
| Session | Input状態、Game Clock、Sealed System Registry、Runtime Scene Session、Session状態と診断 | EditorDocument、SceneDocument、ImGui、D3D12、別SessionのWorld |
| World | 一つの`RuntimeWorld`と最大一つの`SceneInstance` | Project File、Editor状態、別WorldのEntity |
| Frame | `FrameInputSnapshot`、`UpdateContext`、System Update結果 | 長期所有Object、Native Event Pointer |

M14では一つのRuntime Application Sessionが一つのWorld Scopeだけを所有する。
`WorldIdentitySource`とSealed `SchemaRegistry`は同一Process内のRuntimeHost、Editor Play Session、Headless Testで共有できるが、
Current World Pointer、Session Registry、Callback、任意Serviceを保持しない。

Project Scopeの共有は不変Dataに限る。Mutable Input、Clock、System State、SceneInstance、Command Buffer、Error Queueを
二つのSessionで共有しない。同じProjectから二つのSessionを構築した場合も、別の`WorldId`とSession Generationを持つ。

### Explicit Dependency Injection

Runtime Objectの依存はConstructor、Factory、または明示的な作成Requestへ列挙する。
初期実装は小さい型付きDependency Aggregateを使用してよいが、次を満たす。

- 各参照のOwner、必要Lifetime、Const性を公開APIへ記述する
- 必須依存の欠落を部分構築後まで遅延させない
- Session-local所有物をFactory内部のStatic変数へ置かない
- Interfaceと実装のBindingはComposition Rootで一度だけ行う
- Headless Test Doubleを同じ入力位置へ注入できる

`get<T>()`、文字列Key、Type ID等で任意Objectを取得するService Locatorを公開しない。
Runtime中のService追加／削除、親Containerへの暗黙Fallback、自動Constructor Graph、Reflectionによる生成はM14へ導入しない。
Systemが必要とする依存はSystem生成時またはStart Requestへ明示し、Update中にAmbient Contextから検索しない。

SOL-AVESのGlobal／World ContainerはLifetime比較として参考にするが、CueEngineではContainer APIではなく、
Composition Rootが所有する型付きObject Graphとして実装する。

### Runtime Application Session Ownership

`RuntimeApplicationSession`は一回のGame実行を表すcopy不可のOwnerとする。
同じObjectを停止後に再初期化せず、再Playは新しいSessionを作成する。

Sessionは少なくとも次を一意所有する。

- Session Generation
- Session-local Input State
- Game ClockとFrame Index
- 登録完了後にSealされたRuntime System Registry
- `RuntimeSceneSession`
- 最初の失敗と順序付きCleanup診断

`RuntimeApplicationSession`は`RuntimeSceneSession`を一意所有し、`RuntimeSceneSession`が一つの
`RuntimeWorld`と最大一つの`SceneInstance`を一意所有する。これらをSibling Ownerへ分割しない。

Sessionは`EditorDocument`、`SceneDocument`、Window、Native Handle、D3D12 Object、Renderer、
Project Filesystem、Process Logger全体、別SessionのLog Sinkを所有しない。
開始成功後はSnapshot自体を保持する必要はなく、`SceneInstance`もSnapshotへの参照を持たない。

### Editor Play Boundary

`EditorPlaySessionController`は`Cue.EditorCore`に置き、Editor側の唯一のPlay Ownerとする。
PresentationはPlay／Stop IntentをControllerへ渡し、`RuntimeWorld`、`SceneInstance`、System Registryを直接操作しない。
EditorのAuthoring ContextはRuntime Worldではなく、`EditorDocument`と`SceneDocument`のまま維持する。
Playごとに作るWorldだけをPlay Worldとし、Editor Worldという別のMutable RuntimeWorldを常駐させない。

Play開始時は次を行う。

1. 対象`EditorDocument`とScene Identityが現在のProject Sessionに属することを検証する
2. 現在のMemory上の`SceneDocument`を完全検証し、不変`SceneSnapshot`を作成する
3. Snapshotと明示Dependenciesを`Cue.Runtime`のStart Requestへ値またはmove-only所有として渡す
4. Runtime側がRunningへ到達した後だけ、ControllerのActive Sessionとして公開する

DirtyなDocumentは暗黙Saveせず、検証済みの現在Memory状態からSnapshotを作る。
Playは保存成功を意味せず、`savedStateId`、Dirty、Undo／Redo、Selectionを変更しない。
未保存SceneもSnapshot生成条件を満たせばPlayできるが、M14 Completion Gateの標準手順は保存済みSceneを使用する。

Snapshot作成後にEditorがDocumentを変更、Reload、Closeしても実行中Sessionへ影響しない。
Runtime変更をSceneDocumentへ暗黙反映せず、Play Changes Applyは別Research Issueまで禁止する。
ControllerはStable Session IDと状態SnapshotだけをPresentationへ公開し、Runtime PointerまたはEntity Handleを出さない。

### Session State Machine

Runtime Sessionは次の状態を持つ。

```text
Constructed
    |
    v
Starting --------------------------.
    | success                      | start failure
    v                              v
Running                       RollingBack
    | stop / close / update error  | cleanup success
    v                              v
StopRequested                   Stopped
    |
    | frame safe point
    v
Stopping --------------------------> Stopped
    |
    | cleanup failure
    v
CleanupFailed --retry cleanup-----> Stopped
```

`RollingBack`のCleanupが完了できない場合も`CleanupFailed`へ移る。

- `start`は`Constructed`から一度だけ実行できる
- `Starting`はM14ではOwner Thread上の同期処理であり、再入可能なPlay／Stopを受理しない
- `Running`だけがFrame Updateを受理する
- Stop Requestは`Running`または既存`StopRequested`で冪等に受理する
- Update ErrorまたはWindow CloseはStop理由を記録して`StopRequested`へ移る
- Start失敗後のRollbackが完了すれば`Stopped`となり、新しいSessionを作って再試行できる
- Cleanup途中失敗は`CleanupFailed`となり、生存所有物を保持して同じOwner Threadから再Cleanupできる
- `Stopped`または完全に未開始の`Constructed`だけを通常破棄できる
- live `SceneInstance`または`RuntimeWorld`を保持したDestructorは暗黙Cleanupを推測実行せずProgrammer Errorとする

`CleanupFailed`を成功またはStoppedとして表示しない。予期した開始／更新／停止失敗は`Result`と状態で扱い、
Out-of-memory、Owner Thread違反、破棄契約違反等のFatalと混同しない。

### Start Order and Rollback

開始順は次のとおりとする。

1. Start Request、Project Identity、Snapshot Identity、Dependency Lifetimeを副作用なしで検証する
2. Session-local Input、Clock、System定義を構築し、System RegistryをSealする
3. `RuntimeWorld`を作成して`initialize`する
4. `SceneSnapshot`を検証して`RuntimeWorld`へ実体化し、`SceneInstance`を取得する
5. Seal済み順序でRuntime SystemをStartする
6. ClockとInputのFrame境界をResetし、Sessionを`Running`として公開する

個々のSystemの`start`はStrong Failureを保証する。Errorを返したSystemは`Started`へ遷移せず、
購読、Callback、外部登録、Resource、World Mutation等のCleanupを必要とする副作用を残さない。
SystemはFallibleな準備を非公開Candidateへ行い、必要なRAII Tokenを含めて完全に準備できた場合だけ状態をCommitする。
この保証を満たせないSystemはM14のRegistryへ登録できない。

失敗時は開始済み要素だけを次の逆順でRollbackする。

1. Start済みSystemを逆順にStopする
2. `SceneInstance::end`で所有Entityを終了する
3. `RuntimeWorld::request_stop`後に最終`tick`を行い、`shutdown`を確認する
4. System Registry、Clock、InputのSession-local所有物を解放する

Snapshot検証またはInstantiation Plan作成中はWorldを変更しない。
Scene実体化失敗はADR-0017に従い、そのOperation由来の生存Entityを残さない。
RollbackはPrimary Start Errorを保持し、Cleanup Errorを順序付きSecondary Diagnosticとして合成する。
失敗を返したSystem自身は上記Strong FailureによりCleanup対象ではなく、それ以前にStart成功したSystemだけをStopする。
Cleanupが一つでも未完了なら生存Ownerと再Cleanupに必要な依存閉包を失わず`CleanupFailed`へ移る。

### Frame and Stop Boundary

HostとRuntime Sessionの責務を次のFrame境界で分ける。

```text
Host:    Pump platform events -> route close/stop -> build portable input frame -> sample monotonic clock
Runtime: if Running, consume input/time -> update sealed systems -> flush commands at safe point
Host:    observe update result -> existing clear/present when allowed
```

詳細なKey変換、Delta Clamp、Update Phase、Present順は#209、#210、#211、#213で確定するが、次をInvariantとする。

- Platform Event PointerまたはWindows Virtual-Key値をSessionへ渡さない
- ClockはWall Clock変更で逆行しないMonotonic Adapterを注入する
- System Update順はRegistry Seal後に不変である
- 一つのFrameでSystem Update後にStructural Commandを一回の明示Safe Pointへ集約する
- Stop要求がFrame中に発生した場合、現在Callbackを途中破棄せず次の安全な境界で停止へ移る
- Frame開始前にClose／Stopが確定した場合は新しいSystem Updateを開始しない
- StopRequested後に新しいFrame Updateを開始しない
- PresentはHostの任意Stepであり、Headless Sessionの成功条件にしない

Runtime Session、`RuntimeWorld`、System Registry、SceneInstance、Clock、Input StateのMutationは同じOwner Threadに限定する。
別ThreadからのStopは直接状態を変更せず、Host所有のRequest配送境界でOwner Threadへ渡す。
M14は汎用Thread-safe QueueまたはParallel Systemを導入しない。

### Normal Stop and Cleanup Failure

通常停止はFrame Safe Pointで次の順序を使用する。

1. 新規Frame受理を停止する
2. 現在FrameのSystem UpdateとStructural Flushが未完了なら完了させる
3. Start済みRuntime Systemを逆順にStopする
4. System Stopが生成したStructural Commandがある場合はScene終了前の最後のSafe Pointで適用する
5. `SceneInstance::end`を実行し、そのInstanceが所有する生存Entityを終了する
6. `RuntimeWorld::request_stop`を呼び、最後の`tick`でWorldをShutdownする
7. 終了を確認でき、未終了Ownerから参照されないSession-local所有物だけを解放する
8. Pointer、Span、View、Generationを無効化し`Stopped`へ移る

`SceneInstance`終了前に`RuntimeWorld`をShutdownしない。
System StopまたはSceneInstance Endが一部失敗しても、依存関係から独立している後続Cleanupだけを継続して全Errorを収集する。
失敗したOwnerより下位の依存を先に破棄しない。終了済みであっても、未終了Ownerが再Cleanupで参照し得るObjectは保持する。

`CleanupFailed`では次の依存閉包を最低限保持する。

- Stop未完了Systemがある場合は、そのSystem、System Registry、Clock、Input State、Runtime Scene Session、
  SceneInstance、RuntimeWorld、および非所有参照先を保持する
- 生存Entityを持つSceneInstanceがある場合は、そのSceneInstanceとRuntimeWorldを保持する
- RuntimeWorldのShutdownが未完了の場合は、そのRuntimeWorldと必要なProcess Scope参照を保持する
- 終了を確認できたOwnerでも、上記未終了Ownerの再Cleanupに必要なら解放しない

すべての未終了Ownerが成功状態へ到達し、依存閉包が不要になった後だけ通常の逆順解放を再開する。
再Cleanupは終了済みStepを再実行せず、未終了Ownerとその依存閉包だけを対象にする。

RuntimeWorldのShutdown完了後にRuntime Entity Handle、World Pointer、Component View、Command Buffer Pointerを
Controller、UI、Log Entryへ残さない。診断にはStable Session ID、Scene Asset ID、System ID、Error Categoryを値として保存する。

### Multiple Session Isolation

- 各Sessionは別の`RuntimeWorld`、`WorldId`、SceneInstance、Clock、Input State、System Instanceを所有する
- 一方のStart／Update／Stop失敗で他方の状態を変更しない
- Process共通Objectは不変または限定されたID発行だけを行う
- `current_session`、`current_world`、`active_scene_instance`等のGlobal変数を持たない
- System Callbackへ対象Session／Worldを明示Contextで渡す
- 別Worldの`EntityHandle`は既存`WorldId`検査で拒否する

M14のEditor UIはActive Play Sessionを一つだけ表示してよい。これはPresentation制限であり、
CoreのGlobal SingletonまたはProcess内一Session制約として実装しない。

### Standalone and Editor Composition

Standalone `Cue.RuntimeHost`はProcess ScopeとProject Scopeを構築し、起動SceneのSnapshotを用意して
Runtime Application Sessionを所有する。M14では固定またはTest用Scene入力を許可し、
Package発見とRuntime Data PublishはM16で決定する。

Editorでは`Cue.Editor.Tool`がProcess Scopeを所有し、`ProjectWorkspaceSession`と
`EditorPlaySessionController`をProject Scope内へ構成する。ControllerがRuntime Application Sessionを一意所有し、
Editor Window Close前にStopを完了する。

StandaloneとEditor Playは同じRuntime Session APIとStart／Stop順を使用するが、外側のLoop Driverは異なる。
StandaloneはWindows Message Loop、Editor PlayはEditor Host Frameから一回ずつRuntime Frameを駆動する。
Runtime Session内部にWindow Message LoopまたはEditor Main Loopを埋め込まない。

### Error and Diagnostic Contract

公開Lifecycle操作は`Result`を返し、Invalid State、Identity不一致、Snapshot／Instantiation失敗、
System Seal／Start／Update／Stop失敗、SceneInstance End失敗、Cleanup未完了、Resource Limit超過を区別する。
Primary ErrorとCleanup Errorの合成はADR-0010の順序付きSecondary Diagnostic規則に従う。
日本語UI文言をRuntime Errorの正本にせず、安定CategoryとContextからPresentationが生成する。

Session LogはStable Session IDを持つ。Log Sinkの購読Tokenは明示Ownerが保持し、Session終了時にCallbackを無効化してから
UIまたはSessionを破棄する。Process Logger自体をSessionが所有せず、停止後のCallbackが破棄済みControllerへ到達しない。

### Headless Test Contract

次をWindow、ImGui、D3D12、Renderer、Native Inputなしで検証可能にする。

- 同じProcess Scopeから二つのSessionを作成し、異なる`WorldId`と独立状態を持つ
- Snapshot作成後のEditorDocument変更または破棄がRuntimeへ影響しない
- Start各StepへのFailure Injectionで、成功済み要素だけが逆順Cleanupされる
- 各SystemのStart失敗で購読、Resource、World Mutation等の副作用が残らない
- System Start途中失敗で開始済みSystemだけが一度ずつ逆順Stopされる
- Scene実体化失敗でOperation由来の生存Entityを残さない
- SceneInstance End部分失敗で生存所有集合とWorldを保持し、再Cleanupできる
- System Stop失敗でSystem、Registry、Clock、Input、Scene Session、Worldの依存閉包を保持して再Cleanupできる
- Stop要求がSafe Pointで適用され、Stop後にFrame Updateを拒否する
- Update ErrorをFatalとせず、診断を保持して停止へ移る
- 10回以上のPlay／StopでSession、World、SceneInstance、System Instanceが残留しない
- 一方のSession失敗が別Sessionへ影響しない
- Stop後にEditor側へRuntime PointerまたはEntity Handleを残さない
- Wrong Thread、live所有Destructor等のProgrammer Errorを子Process Testで検出する
- Public Header単体CompileとTarget依存検査でRuntimeからEditor／Platform Windows／RHIへの逆依存を拒否する

Windows Process TestはWindow Close、Portable Input変換、Runtime Loop Exit Code、逆順Shutdownを追加で検証する。
手動TestはEditorを終了せず10回以上Play／Stopし、開始失敗後も再Playできることを確認する。

## Consequences

### Positive

- Editor PlayとStandaloneが同じRuntime Lifecycleを使用できる
- Mutable Authoring DataとRuntime Entityの寿命が混在しない
- Start／Stop途中失敗でも未解放Ownerを失わず再Cleanupできる
- 同一Processの複数WorldをGlobal Current Worldなしで分離できる
- Platform、Renderer、EditorなしでLifecycleを再現できる

### Trade-offs

- 明示DependencyとScope Ownerの型が増え、万能Engine Objectより初期構成が長くなる
- `CleanupFailed`を保持するため、UIとHostに再Cleanup／終了診断経路が必要になる
- Stopは即時Object破棄ではなくFrame Safe Pointと逆順Cleanupを通る
- EditorとStandaloneは外側のLoop Driverを別々に持つ

### Mitigations

- Dependency Aggregateを用途単位の小さい型へ分け、任意Service Lookupへ拡張しない
- Lifecycle遷移とRollback順をFailure Injection Testで固定する
- Presentationへ状態SnapshotとSemantic Intentだけを公開する
- M14 GateでECS、Renderer、Sound、Effect、Physicsが変更されていないことを差分確認する

## Rejected Alternatives

### Global Current WorldまたはRuntime Singletonを導入する

Editor Play、Standalone、Test Worldを同じProcessで分離できず、停止後Pointerと再Play時Identityを安全に無効化できないため採用しない。

### 汎用Service ContainerをM14で実装する

型Lookupと自動Object Graphは依存をRuntime時まで隠し、Scope逸脱と循環依存の診断範囲を拡大する。
M14に必要なService数は明示Compositionで管理できるため採用しない。

### Editor WorldをCloneしてPlay Worldにする

EditorDocument状態、Undo履歴、Runtime Pointerを複製する境界が曖昧になり、既存SceneSnapshot契約より安全に扱えないため採用しない。

### RuntimeWorldがEditorDocumentまたはSceneDocumentを参照する

Mutable Authoring Dataの寿命とOwner ThreadがRuntime Updateへ漏れ、Editor変更で実行結果が非決定的になるため採用しない。

### Runtime Application SessionがWindowとRendererを所有する

Headless TestとEditor組込みを阻害し、Windows／D3D12をRuntime Coreの公開依存へするため採用しない。

### DestructorだけにCleanupを任せる

SceneInstance終了にはWorldが必要であり、部分失敗の診断と再試行を返せない。破棄順違反を隠すため採用しない。

### Stop要求で実行中Frameを即時破棄する

System Callback、Query、Structural Commandの途中で参照を無効化し、ADR-0016のSafe Pointを破るため採用しない。

### M14でParallel Update／Job Systemを導入する

Owner Thread、Access Graph、同期、Failure Propagationの追加Researchが必要であり、最小Play Loop完成を遅らせるため採用しない。

## Implementation Sequence

1. #209でPortable Keyboard／Mouse EventとFrame Input Snapshotを実装する
2. #210でMonotonic Clock、Frame Timing、Update Contextを実装する
3. #211で単一Thread System Registry、Seal、Start／Update／逆順Stopを実装する
4. #212で`Cue.Runtime`とRuntime Scene Sessionの所有／Rollbackを実装する
5. #213でRuntimeHostのWindow LoopをInput、Clock、System、Scene Sessionへ接続する
6. #214でEditor Play Session ControllerとSnapshot境界を実装する
7. #215でPlay／Stop Toolbar、状態、Error、ConsoleをPresentationへ接続する
8. #216で10回以上のPlay／StopとFailure Recovery Workflowを統合する
9. #217で3構成Build、Headless／Process Test、手動Workflowを完了判定する

## Follow-up

- #209から#217で本契約を実装、統合、検証する
- Input Mapping、Gamepad、IMEはM14後の専用Issueで判断する
- Physics Fixed Step、Frame Pacing、Replay Timeは計測条件と要件を伴う別Research Issueで決定する
- Parallel System、Job System、Updater Graph、ECS改良はユーザー方針どおり後回しにする
- Scene Streaming、Multi-world Preview、Play Changes Applyは別Research Issueを先行する
- Runtime Package Discovery、Startup Scene Manifest、Runtime Data PublishはM16で決定する
