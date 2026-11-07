# ADR-0011: Runtime World と SceneInstance の最小契約

- Status: Accepted（Scene File / Script / 描画接続前の実行基盤）
- Date: 2026-10-30
- 関連: [境界](0001-architecture-boundaries.md)、[Math](0010-math-values-and-transform.md)

## 目的と配置

`Cue.Runtime` に World を追加し、ECS、SceneInstance、Entity、System の実行寿命をまとめて所有する。
公開 API は `Runtime/World.h`、`WorldHandle.h`、`WorldSystem.h`、`Components/TransformComponent.h`。
Namespace は既存 Runtime と同じ `cue`。数学型・演算は `cue::math` を利用する。

Runtime Target が `Cue.ECS` と `Cue.Math` を利用する。ECS / Math 側から Runtime へ依存させない。
World は Window / Editor / DX12 / FileSystem / Logger / GPU Resource / FPS 待機を所有しない。
既存 Runtime / FrameController / Host は今回 World を自動生成しない。呼出側から明示的に接続する。

## 所有と Thread

- `World::create()` は `Result<std::unique_ptr<World>>` を返す。World は Copy / Move 不可。
- 作成 Thread が Owner。全操作と破棄を同じ Thread 上で直列化し、Lifecycle を再入しない。
- World の System / ECS / Component / Command は World が所有する。System は `unique_ptr` で渡す。
  登録に失敗した場合も引数の所有権を呼出側へ返す API ではない。
- Worker への所有 Thread 移譲は実装しない。Update Worker へ接続するときは、その Thread で生成・停止するなど、
  Host 側で寿命と Thread を設計する。Render は更新中の World を直接借用せず、後続で Snapshot を接続する。
- 借用 Component / Span / Query 参照は、同じ Pool の構造変更・確定・World 破棄で失効し得る。
  Handle 自体は保存できるが、生存を保証しない。

## Handle と SceneInstance

EntityHandle と SceneInstanceHandle は World ID / Slot Index / 64-bit Generation を保持する。
World ID は単一 Runtime 実体で発行し、再利用しない。Slot 再利用時は世代を進め、世代上限の Slot は再利用しない。
別 World、削除済み、古い世代、初期値の Handle を Component 取得・破棄へ使用しても失敗する。
この値は Process 内の参照であり、Scene の永続形式や DLL ABI には使用しない。

SceneInstance は同じ Scene 識別文字列を持つ複数の独立実体を表す。現在は文字列を所有する管理単位であり、
Scene File の読込・Asset 解決・参照 Cache は行わない。生成・実体化の将来の入口として使う。

- Entity は必ず生存中の Instance に所属する。
- Object ID は Instance 内で一意。別 Instance で同じ ID を使える。
- 名前は重複を許可するが、名前検索で複数一致した場合は InvalidState として曖昧さを報告する。
- Scene ID / Object ID / 名前は、非空・最大 1024 Byte・NUL / 制御文字なしの UTF-8。
  Object ID は今回 Opaque な文字列とし、Scene の UUID / Wire Format を確定するものではない。
- 検索と Entity 一覧は Instance を明示する。World 全体から最初の同名 Object を返す API は作らない。
- Instance の破棄はその所属 Entity 群だけを破棄する。別 Instance や別 World に影響しない。
- 初期実装の検索・空き Slot 探索・Transform 同期対象の走査は線形。Index / Cache の高速化は未実測。

## Component と構造変更

World は ECSManager を Private に所有し、生の ECS ID や Manager を外部へ返さない。
World の add / get / remove は世代を検証し、Component 型 ID の 256 型上限も変更前に確認する。
単一型は重複追加で既存実体を返す。Multi 型は追加・全削除・Span 取得を提供する。
`for_each<T...>` は単一 Component 群の Active Entity と Active Component を一度ずつ処理する。

- System / Query Callback 内の直接の Entity / Instance 生成・Component 追加は禁止し、`defer` で予約する。
- Component 削除は自動遅延する。Entity / Instance 削除は直ちに検索・新しい借用から外し、実体の破棄は確定へ遅延する。
- 現在 Callback が借用している実体は確定まで保持する。更新末尾で追加した Entity は次の更新の System 実行から対象になる。
- 自動予約した削除で、先に対象 Entity が削除済みになった場合は無処理とし、新しい同 Slot の Entity に作用させない。
- 予約 Callback は所有し、捕捉する借用先は実行または予約破棄まで生存させる。
- 予約実行中に次の予約を作れる。配列を分離して走査し、一つの確定境界で最大 4096 Command。
  自己予約などで超過した場合は終端失敗にする。
- Component の Constructor / initialize / Destructor 等、Storage 変更中の World API 再入を拒否する。

## System と Lifecycle

WorldSystem は Result を返す initialize / update / shutdown を持つ。
内部 Adapter が既存 ECS の ISystem と Pipeline を使い、公開 Callback には World と DeltaTime / Tick を渡す。
System を登録できるのは初期化前の通常操作時だけ。要求優先度の昇順・同順位は登録順とし、
ECS の同順位 Sort に依存しない一意の順位へ変換する。

1. `initialize`: 事前予約を確定 → System 初期化 → 初期化中の予約と Transform を確定 → Running。
2. `update`: 事前予約を確定 → Tick を進めて System 更新 → 予約と Transform を確定。
3. `shutdown`: 初期化を開始した System を逆順に一度ずつ停止 → 予約と全所有値を破棄。

DeltaTime は呼出側が nanoseconds で渡す。負値、未初期化更新、再初期化、Lifecycle 再入を拒否する。
Query の入れ子は可能だが、外側の借用期間が終わるまで構造変更の制約を維持する。

WorldSystem の停止は Entity ごとの削除に連動させない。旧 ECS は Component 型ごとに finalize_entity を呼ぶため、
Adapter はこの通知を no-op にし、World 全体の shutdown と混同しない。

## Transform

TransformComponent は数学的な Local Transform の正本、最後に同期した World 行列、Dirty を所有する。
`set_local` は有効性を検証してから変更し、失敗時は既存の正本・行列を保持する。
World 初期化・更新の確定境界で Dirty 行列を同期する。まだ親を持たないので World 行列は Local の SRT 行列。
行列を同期する前は `world_matrix` に直前の値が残る。System 内で即時の行列が必要なら Local の数学計算を明示する。

Transform は Entity の必須 Component としない。Transform Pool がない World は同期不要とする。
親子、階層 Path、Shear 分解、描画 Snapshot は後続で設計する。

## 失敗と停止

入力検証で変更前に拒否できる失敗は通常の Result とし、World を継続できる。
System / Query / Deferred Callback の失敗、構造操作途中の例外は終端失敗とし、最初の原因を保持する。
状態の一部が更新され得るため、自動 Rollback / 再更新を行わず shutdown で回収する。
読み取り専用の一覧複写の確保失敗は所有状態を変更しない。

Result の失敗は内部 Adapter で保持し、後続 System を抑制して ECS Pipeline を正常復帰させる。
予期しない例外で旧 ECS の段階 Flag が残る場合も、同じ ECS を再更新しない。
shutdown は Pipeline を再実行せず、開始済み Adapter を直接逆順に止めて最後に ECS を破棄する。

停止 Callback 中は回収用の Component 読取・走査を許可する。更新失敗が既にあっても回収対象を途中で打ち切らない。
個々の停止が失敗・例外となっても残りを回収する。全 Component の実破棄前に World を Stopped にし、
Destructor から半破棄 ECS を借用させない。shutdown の再呼出しは同じ最初の失敗を返し、Callback を再実行しない。

World の Destructor は Owner Thread 上で未停止なら shutdown を試みる。停止の診断が必要な利用側は必ず明示 shutdown の Result を確認する。
Error 文字列の確保などを含むメモリ不足に対する無例外保証、他 Thread からの破棄は提供しない。

## 必要になった旧 ECS の修正

ComponentPool::cleanup は削除時に initialize を再実行していた。World の削除で生成の副作用を再発させるため、
再初期化を除去した。所有値の解放は Pool の削除 / Component の RAII に任せる。
旧 API / Layout は維持し、それ以外の ECS の再設計を行わない。

## 完了範囲

同じ Scene の二つの Instance、Instance 内検索、世代検証、独立 Transform、予約と借用寿命、
System の順序・終了、更新失敗後の回収を Window / GPU なしの Test で確認する。
Scene File / Script / Asset / MeshPool / Renderer / Host Frame Loop への自動接続は実装しない。
