# M11 Build 対象選択と単独 Game アプリの検証

- 日付: 2026-11-03
- 対象: `m11/issue146/project-loading` の既存作業を保全した追加実装
- 契約: [ADR-0018](../Decisions/0018-build-target-and-standalone-game.md)
- Git の Commit / Push / Merge、Issue 作成は実施していない

## 追加した機能

1. Project に OS を保存せず、Build 要求で Platform / Architecture / Configuration を指定する。
   登録は Windows / x64 のみで、未知・未指定・Toolchain 不一致を拒否する。
2. Platform 非依存の GameSession へ World / Script の Lifecycle を集約し、Play と製品で共有する。
3. Windows の専用 Main / Host から静的 Game Module を登録し、Engine と Game Static Library を exe に組み込む。
4. 起動 Scene・Shader・Compiler / PIX Runtime と License を専用 Player Directory に配置する。
5. 実行 File を基準に設定と Scene を読み、別 CWD とリポジトリ外の配布先で起動する。

Sample の Cube に `sample.RotateCube` を割り当て、speed=90 / isRotating=true を保存した。
既存の Transform / Camera / 追加 Cube は保持している。

## 出力と操作

既定 Project の出力:

```text
Samples/RotateCube/Build/Windows/x64/<Debug|Development|Release>/Player/RotateCube.exe
```

Player Directory 一式をコピーして使用する。exe だけでは Scene / Shader / 依存 Runtime が足りない。

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1
pwsh -NoProfile -File scripts/codex_build.ps1 -Configuration Release -GamePlatform Windows -GameArchitecture x64 -PlayerOnly
```

製品用 Target はゲーム DLL / Editor / PlayHost を依存先に含めない。
Editor からの Build / Package / 起動 UI は今回の範囲外で、次の作業となる。

## 自動検証

- `Cue.Project.GameBuildTarget`: 対応一覧、明示対象の選択、未対応値の拒否、Native と製品選択の独立
- `Cue.Project.GameBuildArtifacts`: 明示 Target と既存 DLL / Library Manifest の照合
- `Cue.Runtime.GameSession`: Script 更新 / Property / Snapshot、異なる Thread の拒否、例外、停止と回収
- `Cue.Runtime.GameStartup`: Schema、必須 Key、数値・Path、不完全 Package / Link / I/O / 識別不一致
- `Cue.Project.GamePlayerCMake`: Sample 以外の静的アプリ、Unicode 配置、ゲーム DLL を生成しない Build、Scene 再配置、
  コピー失敗時の完成 Marker 無効化・再試行、旧所有 File だけの回収とユーザー追加 File の保全
- `Cue.GamePlayer.Relocated`: 配布物だけを TEMP の日本語 / 空白 Directory へコピーし、別 CWD から WARP で起動。
  可視 Window、ゲーム DLL / Editor / PlayHost の非 Load、90 Render 以上、Script Tick、Camera / Mesh、
  初期と終了時の行列の変化、正常終了、不完全 Marker の起動拒否を確認
- 従来の `Cue.Script.Play` を含む Debug 全体の回帰検証

## 結果

- Debug の標準 Build: 成功。`out/validation/StandaloneGameBuild.log`
- Debug 全体 CTest: **110 / 110 成功、101.84 秒**。`out/validation/StandaloneGameTests.log`
- Release の製品専用 Build: 成功。`out/validation/StandaloneGameReleaseBuild.log`
- Release のリポジトリ外 / 別 CWD / WARP 起動: 同じ `CueGamePlayerProcessTests` で成功。
  終了値 0。`out/validation/StandaloneGameReleaseRelocated.log` は成功時の出力なし。
- Release の実 GPU 起動: TEMP を CWD として起動し、Render 90 回、Simulation 89 Tick、Mesh 2 個、
  Camera あり、初期・終了時の行列が変化、静的登録・正常終了 true。
  終了時に一度保存した `out/validation/StandaloneGameReleaseNative.txt` で確認。
- `git diff --check`: 成功。

## 未実施と制限

- Windows 自動操作の起動承認が時間切れになったため、画面の目視確認は実施していない。
  自動テストの可視 Window / 行列変化 / Render 回数は、最終画素の Readback や実表示時刻の検証ではない。
- 新規 PC での C++ Runtime 導入、他 OS / Architecture の実装・実行は未実施。
- Development 構成の製品専用 Build / 起動は未実施。Debug 全体と Release 製品を確認した。
- Package 更新途中の失敗は起動拒否で扱い、直前の完成 Package 一式の保存は保証しない。
  実行中の再配布は非対応。Stop → Build / Package → 起動とする。
- 初期製品は World Owner を Main / Update の同一 Thread に置く。ゲームの多 Thread 更新は今回追加しない。
- Editor UI からの全手動制作・Build 経路の完走は後続作業。
