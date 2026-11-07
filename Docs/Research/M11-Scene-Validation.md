# M11 .cuescene の実装と検証

- Date: 2026-10-30
- Branch: `m11/issue146/project-loading`
- Base: `9492422768c3b0350a3033aac07aa920b184450e`
- 契約: [ADR-0012](../Decisions/0012-scene-format-and-instantiation.md)
- 指示: Scene 拡張子を `.cuescene` として、保存用データ・読込・World 実体化・起動参照を実装する

## 実装

- `Cue.Scene`: 検証済み所有データ、Parse / Serialize、抽象 FileSystem 読込。
- `Cue.SceneRuntime`: Scene を World の新しい Instance へ実体化する Bridge。
- `Foundation/Uuid.h`: Project / Scene / Object に共通の UUID 表記検証。
- `.cueproj`: Content 基準の任意 `startupScene`。既存六項目形式の互換性を維持。
- Editor: Window 生成前に Scene と Main 所有 Preview World を準備し、停止後に World を回収する。
- `Samples/RotateCube/Content/Scenes/RotateCube.cuescene`: Cube 名の Object 一つと Transform。
  Mesh / Script / Camera はまだ含めない。

保存文字列を生成する API までを実装した。保存操作は `IFileSystem::replace_file` で行い、Scene Module 自体から外部出力しない。
World / ECS / Math の既存 Source は今回変更しない。

## 検証

- `pwsh -NoProfile -File scripts/codex_build.ps1`: 最終 Debug Build 成功。Compiler Warning / Error なし。
- `ctest --preset windows-vs2026-debug --output-on-failure`: **全 78 / 78 成功、65.30 秒**。
- 新規 `Cue.Scene.Core` / `PublicHeaders` / `Instantiation` / `InstantiationCapacity` / `FileLoading` が成功。
  UUID / 型容量、同一 Scene の独立実体化、Transform 復元、Serialize 往復、実 File の Unicode Path と読込失敗を確認した。
- `Cue.EditorHost.Scene`: Window 前の Preview World と行列準備、Backend 失敗時の回収、WARP Backend での起動・Step・正常停止、
  再初期化拒否を確認した。
- 実 Editor Process の Sample / 相対 / Unicode Project 起動、Resize・最小化・復帰、既存の不正 Project 拒否が成功。
  `Cue.EditorHost.SceneUnsupportedVersion` で不正 Scene の起動前拒否も成功した。
- 既存 World / ECS / Math Source の SHA-256 が作業前と一致することを確認した。
- `git diff --check`: 成功。

Development / Release の Build / 実行、手動の目視確認、実ゲーム Script / Mesh 描画との統合は今回実施していない。
Log は Git 管理外の `out/validation/SceneModule/DebugBuild.log` と `DebugCTest.log` に置く。
初回失敗 Log は `DebugBuildInitial.log` に保全した。

初回 Build では Editor Main の自動整形で shellapi.h が windows.h より先へ並び、Win32 の型宣言不足で失敗した。
依存する Include を別 Group にし、Win32 宣言後に Shell API を読むよう修正した。Source / PDB の破棄は行っていない。

読み取り専用レビューでは、Parser / Bridge / Project / Editor の今回範囲に追加の具体的指摘なし。
実体化時の終端失敗は World の停止が必要なこと、行列が次の World 境界で同期されることを契約に残した。

## 未実施・制約

- Scene の親子関係、Mesh / Script / Camera Schema、編集 UI と Save 操作、Play 用 World の生成は後続。
- Preview World は Main 所有で、Update / Render Worker から直接操作しない。
- 手動の目視操作、別 OS、Sanitizer、Plugin / DLL ABI、汎用データ互換は保証しない。
- 起動 Scene の Path は字句検証であり、親 Junction を含む物理的な閉じ込めを保証しない。
- 任意の確保失敗で旧 World を健康なまま継続させる保証はない。終端失敗では停止・破棄する。
- 現 Branch の Project / ECS / Math / World の未 Commit 変更を保全し、Commit / Push / Merge / Issue 作成は行わない。
