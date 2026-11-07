# Project の Game Build 接続と検証

更新日: 2026-11-03

## 実装

契約は [ADR-0017](../Decisions/0017-project-game-build-contract.md) を参照する。

- `.cueproj` v1 に任意の gameTarget を追加。従来 Project の読込は維持
- `Project::game_cmake_file()` と Target 名 / 入力配置の検証を追加
- CMake helper が選択 Project の Source を取り込み、共通 Source から DLL / Static を生成
- Project ごとの Build / Intermediate へ生成物を分離し、完成 Manifest に SHA-256 を記録
- ProjectBuild Module が構成・Project Identity・Target・ABI・配置・Hash を検証
- Editor Main から Sample の Path 比較・固定ゲーム DLL 名を削除
- 初期化 / Reload / Play で Project 成果物を解決。未 Build 時も編集を継続でき、毎 Frame は File を読まない

## 利用方法

Sample の通常 Debug Build:

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1
```

別 Project を明示して Build:

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1 -GameProject C:/Games/MyGame/MyGame.cueproj
```

2026-11-05 更新: 未指定の GameProject は Preset の空値を使い、以前の既定 RotateCube の Cache も解除する。
ゲームを含める場合は `-GameProject` を明示する。
Sample へ戻す場合は `-GameProject Samples/RotateCube/RotateCube.cueproj` を明示する。
空文字を明示するとゲーム Target を Build Graph に追加しない。

Sample Source は次の一箇所だけで Build 対象を定義する。

```cmake
cue_add_game_module(RotateCubeGame SOURCES RotateCube.cpp)
```

出力は `Samples/RotateCube/Build/Windows/x64/Debug/` の Runtime / Libraries と GameModule.cuebuild。
Build 後に Editor を起動するか、起動済みなら Stop 済みの状態で Script → Reload Script Types を選ぶ。
Play も開始前に Manifest を再検証する。任意 Project の Source CMake はゲーム開発者が管理する。

## 検証対象

| Test | 内容 |
| --- | --- |
| Cue.Project.BuildContract | optional Key、旧形式、名前・予約名・配置・CMake Entry・I/O・読込無副作用 |
| Cue.Project.GameBuildArtifacts | 11 Key、SHA の既知 Vector、同一 Size の差替え、Path / File 種類、I/O、再公開、移動 |
| Cue.Project.GameBuildArtifactIntegration | 実 CMake の Manifest と DLL / Static の Hash・Path を確認 |
| Cue.Project.GameCMake | 日本語・空白配置の別名ゲーム、Configure と Build の分離、不正 UTF-8 / Key / Target / 配置の拒否 |
| Cue.Editor.PlayConfiguration | Project 解決、明示設定の優先、半指定拒否、未 Build 時の WARP Editor 継続、Frame 間で追加読込しない |

検証ログは `out/validation/ProjectGameFinalBuild.log`、`ProjectGameTests.log`、`ProjectGameFullCTest.log` に保存する。
独立 CMake Fixture の先行検証は `ProjectCMakeConfigure.log`、`ProjectCMakeBuild.log`、`ProjectCMakeAutoTests.log`。

検証結果（Windows x64 / Debug）:

- 公式 `scripts/codex_build.ps1` Build 成功
- 追加 5 Test 成功（9.24 秒）
- 既存を含む全 105 Test 成功（83.47 秒）
- `git diff --check` 成功
- Development / Release の全体 Build・実行と、一連の手動制作操作は未検証

## 検証の限界

- 現在の生成・解決先は Windows x64 / Debug、Development、Release の契約。今回の全体検証結果は以下の追記を正本とする
- CMake 4.2.3 の当該 Windows 環境で Unicode Path の cmake_path / file(REAL_PATH) が異常終了したため、get_filename_component を用いて回避した
- Project 読込時の存在検証は後の Build 成功を保証しない
- 成果物検証と子の Load の間の並行 Build は保証しない。Stop → Build 完了 → Reload / Play を使う
- ハッシュ読込は起動 / 操作境界の同期処理。非常に大きいゲームでは将来の Build / Reload Worker 化が必要
- Project は最大 64 KiB、Manifest は最大 4 KiB、Target は最大 128 Byte。Hash は 64 KiB の Buffer を使い、各成果物は 2 GiB までを扱う
- 独立 exe の生成・配布と Editor Build UI、手動制作経路全体の完走は後続

既存の未コミット変更を保全し、Commit / Push / Merge / Issue 更新は行っていない。
