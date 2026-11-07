# ImGui 導入の検証と Completion Gate（M05-08）

Issue: [#84](https://github.com/tochouseito/CueEngine/issues/84)

## 判定対象

対象は EditorHost の Test / TEST の Window 一つ、公式 Win32 / DX12 Backend、Main UI → Update → Render の進行と資源寿命。元 Issue の Demo / FinalColor Image 表示はユーザー指定の Test UI へ変更済み。動的 Texture の表示と更新は GPU Test で検証する。Scene 編集、DockSpace、Multi-Viewport、日本語 Font、任意 DrawCallback は対象外

## 環境と手順

- 2026-10-06、Windows x64、Visual Studio 2026 / MSBuild、Windows SDK 10.0.26100.0
- Hardware: NVIDIA GeForce RTX 3060（is_warp = false を Test 出力で確認）
- WARP: Microsoft Basic Render Driver（Warp 強制選択と is_warp = true を確認）
- Dear ImGui: vcpkg 固定 Port 1.92.9、Source v1.92.9b-docking
- Build は CMake 正本から生成した Visual Studio Project を使い、NuGet restore は行わない

```powershell
pwsh -NoProfile -File scripts/codex_build.ps1 -Configuration Debug
ctest --preset windows-vs2026-debug --output-on-failure
pwsh -NoProfile -File scripts/codex_build.ps1 -Configuration Development
ctest --preset windows-vs2026-development --output-on-failure
pwsh -NoProfile -File scripts/codex_build.ps1 -Configuration Release
ctest --preset windows-vs2026-release --output-on-failure
```

Visual Studio では生成済み `out/build/windows-vs2026/CueEngine.slnx` の CueEditorHost を起動対象にする。Exe は `out/build/windows-vs2026/bin/<Configuration>/CueEditorHost.exe`。製品 Main に動作確認用引数は追加しない。Test の `--warp` / `--sustain` / `--resize` は Test Executable に限定する

## 検証結果

| 項目 | 結果 / 証拠 |
| --- | --- |
| Debug Build / 全 CTest | 成功、38 / 38 |
| Development Build / 全 CTest | 成功、38 / 38 |
| Release Build / 全 CTest | 成功、38 / 38 |
| Hardware / WARP 画素 | ImGui 赤画素、通常 Renderer 復帰後の青画素、先行公開した赤 / 緑 Snapshot の画素を確認 |
| 動的 Texture | WantCreate / WantUpdates / WantDestroy、容量不足 Rollback、Slot 再利用、参照 Pin、更新待機の取消、停止回収 |
| UI 入力 | ImGuiManager Test で Mouse / Wheel / Keyboard / Unicode WM_CHAR / Focus の公式 Win32 配送と Capture、Context 復元を確認 |
| 連続描画 | 描画枠 1 / 2 × 単一 Thread / Worker の 4 構成で各 120 Render Frame 以上。Worker が Main と別 Thread で記録することを確認 |
| 製品 Process | 6 秒以上実行して自動 Layout 保存の時期を通過、WM_CLOSE 後に終了 Code 0 |
| Window 状態変更 | Editor / WindowsHost の最大化 → 最小化 → 復帰 → 元寸法へ復帰 → Close で正常終了 |
| 初期化 / Callback 失敗 | Window / Graph / Font / Descriptor 不正、UI 失敗 / 例外 / 再入、Main 待機中の Update 失敗と取消通知 |
| Debug Layer / GPU Leak | Debug の直接描画と Worker 転送を Hardware / WARP で検査。Error / Corruption と全 Owner 停止後の Device 自身以外の Live GPU Object を検出しない |
| Development / Release の GPU 診断 | Debug Layer は本番設定どおり無効。画素・転送・終了は実行し、Debug interface の存在は要求しない |
| 実画面の目視 / 手動操作 | 未完了。computer-use の状態取得が `foreground window did not report a process id` で失敗し、再取得時に対象 Window がなかった。ツール経由起動の終了原因は未確定。製品 Process 延長 Test では再現しない |

構成別の Build / CTest ログは `out/validation/M05-84/<configuration>-build.log` / `<configuration>-tests.log` に保存した（Git 管理対象外）。最新実行の個別 Test 出力は `out/build/windows-vs2026/Testing/Temporary/LastTest.log` を参照する。検査中だけ Device を保持し Live Object Report を取得するため、Test の終了検査では Warning の対話 Break を無効にし、Message 内容で Leak を失敗判定する

## M05 Gate に残る前提

**M05 全体の完了判定は保留**。#59 の GPU Resize 実装は develop へ統合済み

2026-10-07 の #59 検証では、SwapChain / FinalColor / RTV / SRV / Graph の再生成、WARP 四隅の画素、WindowsHost / EditorHost の最小化・復帰、ImGui Snapshot と Worker の継続を確認し、Debug の全 CTest は 38 / 38 成功した。詳細は [Presentation Resize](PresentationResize.md) を参照する。上の Development / Release 結果は #84 時点の記録であり、#59 変更後の両構成は未実施

WindowsHost の実画面ではクリアカラー、最大化、最小化、復帰、Close を確認した。Editor の Test / TEST 表示も取得できたが、操作前にツール経由の Window が消えたため、Editor の手動 Resize は未完了。終了原因は未確定。自動 Process 検証では Resize / 復帰 / 6 秒以上継続後の正常終了を確認している

UI は日本語文字を配送するが内蔵 Font に日本語 Glyph はない。公式 Backend 内の void / Assert GPU 失敗のすべてを回復可能な Result へ変換する保証はなく、任意 GPU 失敗注入は未実施。全面 Queue 待機の削減や性能改善は今回主張しない
