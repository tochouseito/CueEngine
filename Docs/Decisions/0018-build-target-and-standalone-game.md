# ADR-0018: Build 対象と単独 Game アプリ

## 決定

Platform / Architecture は Project 作成時に固定せず、Build 要求で指定する。
`.cueproj` に対象 OS を追加しない。現在の登録実装は Windows / x64 のみで、
未指定・未対応・Toolchain と不一致の要求はエラーにする。

- `Cue.ProjectBuild` の `GameBuildTarget` が Platform / Architecture / Configuration を表す。
- `supported_game_build_platforms()` が対応範囲を返す。Native Editor / Play の対象は
  `current_native_game_build_target()` から独立して取得する。
- CMake 側は `CUE_GAME_PLATFORM` / `CUE_GAME_ARCHITECTURE` と構成を受け取る。
  Windows Preset は Windows / x64 を明示する。未対応値を Windows に置換しない。
- Platform の Host / 拡張子 / 配布依存は `cmake/Platforms/WindowsGame.cmake` へまとめる。
  C++ Provider と CMake Provider は異なる責務の登録情報であり、対象追加時は両方を更新する。

## 実行の分離

`Cue.GameRuntime` の `GameSession` は World / Script の初期化・更新・Snapshot・停止を所有する。
公開契約に Win32 / DX12 / Editor を含めない。PlayHost と製品 Host が共用する。
World の操作は一つの Owner Thread で行い、Renderer へ所有 Snapshot を渡す。

Windows 固有の Window / Backend / 起動引数 / 診断は `Cue.GameHost.Windows` が担当する。
製品の最小 Main は静的ゲーム登録関数と Build 識別情報を Host へ渡す。
初期構成は Main と Update を同じ Thread、CPU / GPU Frame 数を 1 とする。
汎用 Runtime に Windows 起動処理を移さない。

ゲーム Source は従来と同じ `cue_add_game_module()` で登録し、製品 exe は
Engine の静的 Library と `Target.Static` をリンクする。
開発用 DLL は Editor の別 Process Play 用として残すが、製品はゲーム DLL を Load しない。
Engine の生成 Solution に独立したアプリ Target を追加し、Editor との同時 Build を必須にしない。

## 配布物

```text
Project/Build/<Platform>/<Architecture>/<Configuration>/Player/
  <applicationName>.exe
  GameStartup.cueapp
  GamePackage.cuepackage
  Content/<startupScene>
  EngineResources/Shader/...
  dxcompiler.dll / dxil.dll / WinPixEventRuntime.dll
  Runtime の License File
```

Link の直接出力は Project の Intermediate 配下とし、Player へ配布物だけをコピーする。
ゲーム DLL / Library、Editor / PlayHost、Source は Player に含めない。
OS / DirectX / C++ Runtime など外部 Runtime の導入条件までは静的ゲームリンクで解消しない。

`GameStartup.cueapp` は実行開始 Scene・Window・FPS と Build 識別情報を持つ機械生成設定。
実行 File の Directory を基準に読む。CWD / Repository の配置を前提にしない。
Platform Host は Build 時の識別値との一致と必須 Shader / Compiler の存在を検証する。
GameStartup Parser は Platform を勝手に選択しない。

Package は専用 Player Directory の所有印と所有 File 一覧を使う。
既存の他用途 Directory や Link を拒否し、以前自分が配置した不要 File だけを回収する。
コピー前に完成 Marker を無効化し、全配置後に最後の `GamePackage.cuepackage` を公開する。
失敗時は不完全な Package として次回起動を拒否する。以前の Player 一式の保存は保証しない。
Source / Content / Editor の保存データは Package の削除対象にしない。
実行中の Package 置換は非対応であり、Stop → Build / Package → 起動とする。

## Build 操作

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1 -Configuration Release `
    -GamePlatform Windows -GameArchitecture x64 -PlayerOnly
```

別 Project は `-GameProject <absolute .cueproj path>` で選択する。
Editor の Build / Package / 起動 UI は次段階とし、今回は追加しない。

## 検証境界

Owner Thread / Script 停止、設定の不正・欠損・Link、未対応 Target の拒否、
ゲーム DLL を生成しない静的製品 Build、Package コピー失敗と再試行を自動確認する。
製品 Process は配布物だけをリポジトリ外の日本語 / 空白 Path へ移し、別 CWD から起動する。
WARP で可視 Window、描画 Callback 完了、Camera / Mesh、回転 Transform の変化、正常終了を確認する。
Frame 数は実際の画面表示時刻や GPU Readback による画素検証を意味しない。
新規 PC の前提 Runtime 導入と Editor の全手動制作経路は別検証とする。
