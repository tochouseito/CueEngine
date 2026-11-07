# ADR-0017: Project の Game Target と Build 成果物

## 決定

`.cueproj` の設定からゲームの Build Target を選び、Editor の Sample 専用接続を廃止する。
ゲーム Source / 依存の正本は `Source/CMakeLists.txt` とし、Source 一覧や Build Command を `.cueproj` へ重複保存しない。

```ini
formatVersion=1
projectId=7f533c49-d416-47ea-98c1-026822b1e6ed
name=RotateCube
applicationName=RotateCube
sourceRoot=Source
contentRoot=Content
startupScene=Scenes/RotateCube.cuescene
gameTarget=RotateCubeGame
```

- `gameTarget` は v1 の任意 Key。未指定の従来 Project はゲーム Build / Play を構成せず、そのまま読み込める
- 指定時は Source Root の `CMakeLists.txt` が通常 File として存在することを検証する。読込だけでは Directory / 成果物を作らない
- Target 名は先頭 ASCII 英字 / `_`、以後英数字 / `_` / `-`、最大 128 Byte。CMake 予約名と Windows Device 名は大小文字を区別せず拒否する
- Build を指定した Project の Path に `;` / `$<` を使わない。日本語 / 空白を含む配置は扱う。表示名にはこの Path 制約を適用しない
- Source / Content と生成先の Build / Intermediate を重ねない。`.`、先頭要素の Build / Intermediate と Windows の末尾 Dot / Space Alias も拒否する
- 永続設定に PC 固有の絶対 Path、DLL の配置先、実行時 Handle を保存しない

## CMake の接続

Engine の `CUE_GAME_PROJECT` Cache で一つの `.cueproj` を指定し、`cue_add_game_project()` から Source CMake を取り込む。
2026-11-05 の変更以後、既定値は空の Project 未選択とし、ゲームを Build Graph に追加しない。
Sample を含むゲームの Build は `CUE_GAME_PROJECT` を明示したときだけ行う。
選択を変更しても Project の Source / Content を書き換えない。

ゲーム側は `cue_add_game_module(Target SOURCES ... LIBRARIES ...)` を使う。
同じ Source / 依存から開発用 `Target`（DLL）と製品用 `Target.Static`（静的 Library）を生成する。
`.cueproj` と Source が宣言する Target の不一致は Configure で拒否する。

```text
Project/
  Project.cueproj
  Source/CMakeLists.txt
  Content/...
  Intermediate/Build/<Engine Build Tree の識別値>/...
  Build/Windows/x64/<Debug|Development|Release>/
    Runtime/<Target>.dll
    Libraries/<Target>Static.lib
    Libraries/<Target>Import.lib
    GameModule.cuebuild
```

Engine 自身の中間生成物は引き続き通常の `out/build/...` に置く。
`Cue.GameProjectBuild` は両 Library が完成してから Manifest を公開する。
Visual Studio で CueEditorHost だけを Build した場合も、この Target と CuePlayHost を依存先に含める。
Configure だけでは完成 Manifest を生成しない。

## 完成 Manifest

`GameModule.cuebuild` は機械生成の 11 Key で、未知・重複・欠損 Key を受け付けない。

- formatVersion、projectId、gameTarget、configuration
- platform、architecture、abiVersion
- module、staticLibrary（構成 Directory からの固定相対 Path）
- moduleSha256、staticLibrarySha256

両成果物を Hash 計算後、同じ Directory の一意な一時 File から Rename で公開する。
出力先がゲーム CMake で契約外へ上書きされていた場合は公開しない。
現在の対象は Windows x64。実行時の DLL ABI は従来どおり子 Process 内で再検証する。

## Editor の接続

- ProjectBuild Module は Manifest の識別・構成・ABI・固定 Path・通常 File・SHA-256 を検証する
- DLL を Editor に Load しない。型情報取得と実行は引き続き別 Process
- 初期化後、Script Types の Reload、Play 開始前だけ成果物を読む。毎 Frame の読込 / Hash は追加しない
- 未 Build / 成果物不一致でも Editor は継続し、操作時に理由を表示する。Build 完了後の Reload / Play で再試行できる
- 再検証失敗では古い Metadata / DLL 選択を破棄する。稼働している子は置き換えない
- Library 利用者の playExecutable / scriptModule 明示指定は両方必須で優先する。不正な明示設定から Project の DLL へ黙って戻らない
- Play 実行 File は Editor と同じ配置の CuePlayHost.exe を利用する。ゲーム DLL は選択した Project の Manifest から得る

## 寿命と並行操作

初期の運用は **Stop → Build 完了 → Reload Script Types / Play** とする。
Manifest の原子公開は、DLL と静的 Library の二個の File を同時に置き換える保証ではない。
Hash 検証中のサイズ変化と Manifest 再公開は検出するが、検証後から子の Load までの外部書換えを Lock しない。
同じ Project / 構成へ複数 Build Tree から同時に Build しない。
将来の非同期 Build UI では Build / Play の排他と成果物 Generation の寿命を管理する。

## 後続

今回の契約はゲーム DLL / 静的 Library の Build と Project からの Play 接続まで。
独立製品 exe、配布 Data / Shader の Package、Editor の Build 進捗・取消・起動 UI は次の段階で実装する。
Source の変更後に自動 Build したり、既存 Source との差を Manifest だけで判定したりはしない。
