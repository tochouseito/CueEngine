# M11 World の実装と検証

- Date: 2026-10-30
- Branch: `m11/issue146/project-loading`
- Base: `9492422768c3b0350a3033aac07aa920b184450e`
- 契約: [ADR-0011](../Decisions/0011-runtime-world-and-scene-instances.md)
- 指示: Entity / Component / Instance / Handle / Transform / System の最小 World を Runtime に実装する

## 変更

- `Runtime/World.h` / `Private/World.cpp`: ECS の所有、世代付き Slot、Instance 内検索、構造変更の予約、Lifecycle。
- `Runtime/WorldHandle.h`: EntityHandle / SceneInstanceHandle。
- `Runtime/WorldSystem.h`: World を借用する System と nanoseconds / Tick の更新入力。
- `Runtime/Components/TransformComponent.h`: Local の正本、Dirty、同期済み行列。
- Runtime Target に `Cue.ECS` / `Cue.Math` の依存を追加した。
- 既存 Runtime / FrameController / Host の実装は変更せず、World の自動生成は追加しない。
- ECS の削除時に Component.initialize を再実行していた cleanup を修正した。World で削除したときの初期化回数で回帰検証する。

## 検証する内容

- 同じ Scene ID から二つの Instance を作り、同じ Object ID / 名前を独立して検索する。
- 同一 Instance の重複 ID・曖昧な名前、不正文字列、古い世代・別 World の Handle を拒否する。
- Slot 再利用と Instance 破棄で、他 Instance を壊さず古い Handle を無効にする。
- Transform の回転と位置を別 Instance へ反映せず、Dirty 行列を確定する。
- System の要求優先度・同順位の登録順・停止の逆順・一度だけの停止を確認する。
- 借用中の直接追加と Lifecycle 再入を拒否し、削除中の参照を確定まで保持する。
- 予約追加・入れ子予約、自動削除予約の取消、無限予約の上限を確認する。
- Multi Component の借用、Active による Query 対象制御を確認する。
- 部分初期化失敗、更新 Callback 例外、停止失敗、複数 Entity の停止走査でも回収し、最初の原因を維持する。
- 別 Thread の操作、Transform 未使用で Global 型 ID が 256 を超えた場合の経路を確認する。

## レビュー

読み取り専用レビューで次の二点を修正した。

1. 更新失敗後の停止 Query が最初の Entity で中断していた。停止段階では既存の失敗による中断を除外した。
2. 未使用の Transform 型を Bitset で照会していた。Pool の有無を先に調べ、同期不要なら成功するようにした。

再確認では追加の具体的問題なし。Storage 変更中の再入、Commit 中の Lifecycle 再入、
Component / System の Destructor から半破棄状態を借用する経路も拒否した。

## Build / Test 結果

- `pwsh -NoProfile -File scripts/codex_build.ps1`: 最終 Debug Build 成功。Compiler Warning / Error なし。
- `ctest --preset windows-vs2026-debug -R '^Cue\.(Runtime|Math|ECS)\.' --output-on-failure`:
  **11 / 11 成功、0.93 秒**。
  World 3 件、Math 2 件、ECS 2 件、既存 Runtime / FrameController / FramePacing / Simulation 4 件。
- World 本体の Test は Instance と安全な参照に加え、失敗後の全 Entity 回収や予約の上限を含む。
- 型 ID 上限は別 Process の専用 Test で 260 型を登録して再現し、他の Test の Global Registry を汚さない。
- Project 本体、Editor Main、既存 Runtime.cpp の SHA-256 が作業前と一致することを確認した。
  ECSManager.h は上記 cleanup 修正のため意図して変更している。
- `git diff --check`: 成功。
- Source 検査で新規 World に Win32 / DX12 / Logger / FileSystem の依存がないことを確認した。

独立した World と ECS 削除の変更範囲に対して関連 Test を実行し、全 CTest は再実行していない。
Development / Release、他 OS / Compiler、Sanitizer、Editor の目視操作は今回実施していない。

Log は Git 管理外の `out/validation/WorldModule/DebugBuild.log` と `DebugCTest.log` に保存する。
`BeforeStatus.txt` と `PreservedHashes.json` は保全対象を記録したもので、未 Commit File 本体のバックアップではない。

## 未実施・制約

- Scene File の読込・一括実体化、Script、GPU Resource、描画 Snapshot、Host / Worker との実接続は後続。
- 親子関係と階層 Path 検索は未実装。
- World は作成 Thread を Owner とし、Thread 所有の移譲は未実装。
- 検索・空き Slot 探索は線形で、性能改善は実測していない。
- 旧 ECS の型 ID は Global Registry、最大 256 型。DLL 境界・Plugin の ABI としての利用は未検証。
- Component 借用 Pointer は Handle と違い自動で世代検証されない。構造変更・確定を越えて保持しない。
- Callback 失敗後の状態 Rollback は保証せず、World を停止・破棄して作り直す。
- Commit / Push / Merge、Issue / Branch の新規作成は行っていない。既存 Project / ECS / Math の未 Commit 変更を保持する。
