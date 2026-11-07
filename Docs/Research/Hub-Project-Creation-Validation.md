# Hub の Project 新規作成と指定 Engine

- 日付: 2026-11-04
- 契約: [ADR-0021](../Decisions/0021-hub-and-engine-distribution.md)
- 既存の未コミット実装と Sample の手編集値を保持した追加実装

## 実装

`Cue.ProjectTools` は `IFileSystem`、正規 Template 配置、UUID 生成 Callback を呼出中だけ借用する。
公開結果は完成した `.cueproj` の Path / Project ID / 表示名 / Engine Version の所有値であり、
Staging に対する Project Object の借用を返さない。

親が存在する未作成の絶対 Directory だけを受け付ける。表示名、識別名、SemVer、
Link / Junction、Windows の末尾 Dot / Space Alias を作成前に検証する。
識別名は File 名と `CueGame_<name>` の CMake Target 名に使うため、ASCII 120 Byte 以下とする。
表示名は UTF-8 256 Byte 以下、制御文字と先頭・末尾 Space を拒否する。

保存先の兄弟 `.cue-create-<UUID>` に所有 Marker を CreateNew / 非共有で作る。
固定の Source / Content File も CreateNew で生成し、完全書込み・Flush・Close を確認する。
Project と Scene の実 Parser による検証後、非置換 Rename の成功で公開する。
失敗時は自分が作った既知 File と空 Directory だけを回収する。不明 File が混入した Staging は残し、
主原因へ Cleanup 失敗の診断を付記する。電源断での Directory Entry 永続化までは保証しない。

正規 Template は次の二つ。Samples の Directory や Build 生成物をコピーしない。

| Template | 初期 Scene | Game Module |
| --- | --- | --- |
| Empty3D | Camera 一つ | 未割当の `game.Empty` Behaviour 登録例 |
| RotateCube | Identity Transform の Cube 一つ、Camera 一つ | `sample.RotateCube`、90 度/秒、有効 |

Project / Scene / Object / Script Instance は新規 UUID を生成する。
Script Type ID と Built-in Mesh ID は Asset / Schema の識別なので維持する。

## Project と CLI

`.cueproj` v1 の読み取りを維持し、v2 では `engineVersion` を必須とする。
C++ と CMake の Parser が同じ Version 境界を持つ。
CMake は指定 Version と現在の `CUE_ENGINE_VERSION` が一致しない場合、ゲーム Source を評価する前に失敗する。

`CueProjectTool.exe` は UTF-16 の OS 引数を UTF-8 へ変換し、各 Flag を一度だけ受け付ける。
stdout は Schema 1 の JSON Object 一つ、成功 Exit 0、運用・入力失敗 Exit 1、例外 Exit 2。
Hub からは Shell を介さず個別の引数を渡す。

Tool は共通 EngineInstallation の配置解決と Lease を使う。
配布 `Bin/Tools` の Descriptor が不正・欠損していても生成元 checkout へ fallback しない。
Lease は CLI が終了するまで保持し、Hub の Engine 削除と Template 読込を排他にする。
`create` は、指定 Engine Version と Tool が所属する Engine の一致も確認する。
`validate` は一覧登録用の Metadata 検証とし、未導入の別版を指定する Project も受け付ける。
実際に開く / Build する境界で指定版を照合する。

## 検証

実行済み:

- `cmake -DCUE_SOURCE_ROOT=<root> -DCUE_FIXTURE_ROOT=<out/validation/ProjectTools-CMake> -P Engine/Tests/ProjectTools/ProjectVersionCMakeTests.cmake`: 成功
- 新規・変更 C++ の clang-format 適用と `git diff --check`: 成功

Native Build / Test は親担当の集中検証で実施する。
追加した Test の対象は以下。

- ProjectCreator: 両 Template、Unicode、先頭数字の識別名、新規 ID、同じ保存先への上書き拒否
- 失敗前検証: 無効 SemVer、予約 File 名、制御文字、Alias、Template 欠損、重複 UUID
- 途中失敗: 書込み失敗の Rollback、不明 File の保全、公開直前の保存先衝突と他者 File の保全
- Parser: v1 維持、v2 必須項目、旧形式への新 Key 混入拒否
- CMake: v1 / v2、Version 不一致・欠損・構文不正・先頭零・未来形式の拒否
- CLI: JSON、Unicode、Project ID 維持、上書き拒否、未知 Flag、無効 Version

Hub からの実 UI 新規作成、作成した Project の Editor 起動 / Build / Play は統合検証で確認する。
