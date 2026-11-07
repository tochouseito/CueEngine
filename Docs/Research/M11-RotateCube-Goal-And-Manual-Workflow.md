# M11 RotateCube の目標と配布・手動制作の完了条件

- 更新日: 2026-11-06
- 状態: 目標と完了条件を更新

## 目標

**CueEngine が未導入の Windows x64 環境で、第三者が配布物と手順書を使って Engine をインストールし、
Editor で手作業で Scene を作成して、Script で Cube が回り続ける単独アプリを完成できること。**

手順書には配布物の入手先と、Visual Studio の C++ ツールセットや Windows SDK などの必要条件を記載する。
利用者は開発元の Repository を取得せず、配布版の Editor と SDK を使って自分で Scene を組み立てる。
自動 Test、配布物の個別検証、第三者による手動操作の完走は、それぞれ別に記録する。

## 手動の完走手順

1. 手順書に沿って必要な開発ツールを用意し、配布されたインストーラーで CueHub を導入する。
2. Hub で対象バージョンの Engine SDK を導入する。
3. Hub で新しい Project を作成し、導入した Engine の Editor で開く。
4. Editor で Scene を作成し、Cube を追加して Object 一覧から選択する。
5. Inspector で Transform を調整し、Scene View で見た目を確認する。
6. 手順書に沿って回転 Script を用意し、Editor で Cube に割り当てて回転速度などを設定する。
7. `.cuescene` を保存し、開き直して設定が復元されることを確認する。作成した Scene を起動 Scene に指定する。
8. Editor からアプリを Build する。失敗した場合は、Editor で理由を確認できること。
9. 生成アプリを起動し、Editor を閉じた状態でも Cube が回転し続けることと、アプリが正常終了することを確認する。

Scene / Project 設定の手編集や、利用者による Build コマンドの直接実行なしで完走できることを完了条件とする。
Script の編集には外部 IDE を使える。
配布契約は [ADR-0021](../Decisions/0021-hub-and-engine-distribution.md) を参照する。

## 必要な最小 Editor 操作

| 機能 | 到達条件 |
| --- | --- |
| Scene 操作 | 作成・読込・保存・再読込ができる |
| Object 操作 | Cube 追加、一覧表示、選択ができる |
| Inspector | Transform と必要な Component / Script の設定を編集できる |
| 描画表示 | Cube の配置や変更結果を確認できる |
| Script 接続 | Script を対象 Object へ割り当て、回転に必要な値を設定できる |
| アプリ出力 | 起動 Scene と Build 対象を指定し、Build / 起動と結果確認ができる |

## 現在の状態と次の実装への影響

現在は Project / `.cuescene` 読込、ECS、Math、World、Instance 管理、Transform、Preview World 生成まで実装している。
Bounds を含まない CPU MeshData、組み込み Cube、属性別の統合 Page を持つ MeshPool も追加している。
Camera / Mesh の Scene Component と World 実体化、Snapshot 抽出、Depth 付き ExecuteIndirect 描画、
Editor Scene View への FinalColor 表示まで実装した。Sample の Cube は静的な初期回転を持つ。
続いて Scene の新規・読込・保存、Object 一覧からの Cube 追加・選択、Transform の編集、
Undo / Redo、未保存確認を追加した。編集正本は Cue.Editor、Panel は Cue.Editor.Ui が担当する。
さらに C++ Behaviour / ScriptSystem、Script の割当・設定保存・Undo / Redo と、別プロセスの Play / Stop を追加した。
Editor はゲーム DLL を読み込まず、子プロセスの型情報と Property の受理応答だけを利用する。
Editor の Build 設定・保存確認・非同期 Build / Package・結果表示・製品起動を追加した。

Project ごとの gameTarget と Source CMake の接続、構成別の DLL / 静的 Library の生成、
完成 Manifest からの汎用 Play 接続を追加した。契約は `Docs/Decisions/0017-project-game-build-contract.md` を参照する。
続いて Build 時の Platform / Architecture 選択、静的リンクした独立アプリ exe、
Scene / Shader / Runtime の配布と実行 File 基準の起動を追加した。
契約は `Docs/Decisions/0018-build-target-and-standalone-game.md` を参照する。
Sample の既存 Cube に回転 Script の保存設定を付け、配置や Camera は保持する。
`Build > Build Settings...` から対象を選択し、Build / Cancel Build / Run を操作する。
保存済みの Project startupScene を配布し、未保存編集の保存失敗では開始しない。
契約は `Docs/Decisions/0019-editor-build-and-run.md` を参照する。
現状では起動 Scene を `.cueproj` の `startupScene` から読み取る。保存した Scene を Editor で起動 Scene に指定する機能が必要。
署名付き SDK のローカル導入、Project 作成、配布版 Editor の起動、独立アプリの Build / 実行には
[個別の検証記録](Hub-Distribution-Validation.md) がある。第三者が未導入の環境で入手から完成まで通した確認は未実施。
今回の MeshData は Bounds を含めない。最初の選択操作は Object 一覧から行い、Bounds を使う選択・カリングは後続とする。
各機能の設計・実装を段階的に進め、最後にこの手動手順を実際に完走して確認する。
この文書の追加だけで M11 完了、手動検証済み、Build / Script 実装済みとは扱わない。

## 今回行ったこと

目標文書・引き継ぎ書の更新後、Cube / MeshPool、続いて Camera と最初の Scene 描画経路を追加した。
描画契約は `Docs/Decisions/0014-camera-and-initial-scene-rendering.md`、編集契約は
`Docs/Decisions/0015-editor-scene-document-and-ui.md` を参照する。
Script 基盤の契約は `Docs/Decisions/0016-native-script-and-isolated-play.md`、検証範囲は
`Docs/Research/M11-Script-Foundation-Validation.md` を参照する。
Script の割当から別ウィンドウ Play までを追加したが、アプリ出力を含む手動制作経路の完走は未達成。
Commit / Push / Merge、Issue / Milestone 更新は行っていない。
