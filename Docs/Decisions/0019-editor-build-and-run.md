# ADR-0019: Editor の非同期 Game Build と起動

## 決定

Editor の `Build > Build Settings...` で Platform / Architecture / Configuration を選択し、
製品 Build / Cancel / Run を要求する。現在は Windows / x64 を登録する。
UI に未対応 OS を架空の候補として追加せず、実行直前にも選択を検証する。
Build Panel は初期非表示とし、Scene View の表示を優先する。

- `Cue.ProjectBuild` は要求対象、状態、所有結果と `IGameBuildService` の公開契約を持つ。
- `Cue.ProjectBuild.Windows` は Process / Pipe / Job と配布物の検証を実装する。
- `Cue.Editor.Ui` は表示値を draw 中だけ借用し、所有 Action を返す。Process / File を操作しない。
- `Cue.EditorHost` は Owner Thread で保存確認と要求を確定する。ImGui の借用を終えてから実行する。
- `CueEditorHost` の Main は生成時に決めた Engine / CMake / Toolchain / vcpkg の絶対 Path から
  Service を生成する。起動元 Shell の PATH / VCPKG_ROOT を前提にしない。
  Tool が利用できない場合も Editor を開き、理由を Build 表示へ渡す。

## 保存と起動 Scene

Build は Project の `startupScene` を保存 File から配布する。編集中の別 Scene を勝手に起動 Scene にしない。
Panel に起動 Scene の絶対 Path を表示する。

未保存なら ImGui で以下を確認する。

| 選択 | 動作 |
| --- | --- |
| Save and Build | 通常の Scene 保存が公開成功した後だけ Build を開始する |
| Build Saved Files | 未保存編集を保持し、ディスクの保存済み内容を Build する |
| Cancel / Escape / 閉じる | 要求を消費して開始しない |

保存先選択の取消・保存失敗でも要求を消費し、後で古い Build を自動再開しない。
確認中の二度目の要求は最初の選択を上書きしない。
Build 中は保存と新たな Build / Play 開始を抑止する。表示とメモリ上の Scene 編集は継続できる。
外部 IDE からの Source / 設定編集と Build の同期は開発者が行う。

## 非同期実行

専用生成先は Project の `Intermediate/EditorBuild/<Engine 識別値>/Windows/x64` とする。
Editor 自身を開いている生成 Solution を Configure で書き換えない。
同じ Tree は構成ごとに再利用し、構成は `--config` で指定する。

1. `cmake --preset windows-vs2026 -B <専用Tree>` に Project / Platform / Architecture / Toolchain を渡す。
2. Configure 成功後に `cmake --build <専用Tree> --config <構成> --target Cue.GamePlayerPackage --parallel` を実行する。
3. 完成 Marker、設定識別、起動 Scene、exe、必須 Runtime / Shader の通常 File を再検証し、成功を公開する。

Configure は Test / 手動性能 Tool を無効にし、製品だけを Build する。
未知対象を Windows へ補完せず、ゲーム DLL / Editor の Link を製品に要求しない。
Process 引数は Windows の quoting 規則で個別に構築し、Shell の Command Line として解釈しない。
標準 Handle の継承対象を限定し、Build の子孫を Kill-on-close Job に入れる。

Owner は `step()` の冒頭で非待機 poll する。最小化で Render が止まっても出力・終了を回収する。
1 poll の読込は上限 128 KiB、表示 Log は UTF-8 の末尾最大 1 MiB とし、古い出力の省略を表示する。
計測や Frame ごとの Log File 出力は追加しない。
状態は Idle / Configuring / Building / Succeeded / Failed / Cancelled とし、終了 Code と診断を保持する。

## 排他と寿命

同一 Project の `Intermediate/.cue-editor-build.lock` を共有なし File として保持する。
同一 Thread 上の別 Service、複数 Editor、構成違いの Build も同時に開始しない。
取消は Build Job に要求し、子孫の終了が確認できるまで排他を維持する。
Editor の終了では Build Job を回収する。

Run は選択対象と直前成功対象が一致する場合にだけ許可し、配布物を起動直前に再検証する。
起動する製品 Process は Build Job に含めず、Editor 終了後も独立して動作する。
監視中は再 Build / 二重起動を抑止する。製品の終了 Code は非待機で回収する。
外部から起動された exe の上書き可否も開始前に確認し、後発の競合は Package の失敗として扱う。

## 制限

配布先更新失敗で前の Package 一式を保存する保証は ADR-0018 と同様に持たない。
完成 Marker を欠く不完全な配布物を成功・起動可能とは扱わない。
Tool / Engine の移動後は Editor の生成時設定を更新する。
今回の Build 操作は製品用。ゲーム DLL の更新と Script Types Reload は従来の Play 契約に従う。
