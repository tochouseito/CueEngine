# M09 実装の検証記録

## 対象

M09 Frame Pacing and Latency Control、Issues #115〜#122。Issue 単位の Branch と PR で develop へ統合し、完了 Issue を閉じる。Milestone #10 は open を維持する

## 構成別 Gate

| 構成 / x64 | Build | CTest | 実行時間 |
| --- | --- | --- | --- |
| Debug | 成功 | 50 / 50 成功 | 55.15 s |
| Development | 成功 | 50 / 50 成功 | 46.25 s |
| Release | 成功 | 50 / 50 成功 | 44.67 s |

Debug は scripts/codex_build.ps1、他構成は windows-vs2026 の構成別 Build / CTest Preset を使用。NuGet restore は実行しない。ログは out/validation/M09/Final-{構成}-build.log と Final-{構成}-tests.log に保持する

## 重点確認

- Render 完了後の FPS 待機、周期完了通知と CPU 完了の分離、先行 0 / 1 / 2、停止での解除
- CPU / GPU 1〜3 枠の異なる組合せ、実 GPU / WARP の ImGui 画素と Font / Texture / Descriptor 寿命
- GPU 完了上限、Device 失敗、完了 Token の回収前の全検証、終了時の Device / Fence 参照解放順
- Waitable 初回通知、取得済み通知の保持、取消、Resize、最小化 / 復帰、製品 Process の Close
- 安全な構成切替、Worker / Single 切替、単調 Frame ID、再構築 Callback の failure / 例外後の停止状態
- Schema 付き保存 / 読込、保存失敗での旧 File 保全、保存値からの再起動、開発者 Policy
- 表示同期 Fallback、予測期限、表示通知が 10 ms 遅れた Single の実待機計測、開始遅延後の古い期限再利用防止
- 60 Hz 更新 / 120 FPS 描画、前後状態の数値補間、追いつき上限、余分 Tick の破棄と停止
- Update F1 が進んでも Render F0 の補間情報が不変、古い Frame ID の拒否、固定 / 可変周期の切替
- FPS 変更で固定更新 Hz を変えないこと、最小化休止時間の除外、失敗した更新の再実行拒否

## 調査と修正

内部レビューでは単一 Thread の Message Pump を塞ぐ表示待機、設定再構築失敗後の状態、例外境界、待機計測の混在を確認して修正した。設定切替テストのクラッシュは Debugger で ImGui::NewFrame → ImGui_ImplWin32_GetWindowDpiScale の PlatformUserData 参照切れを特定し、公式 DX12 Backend 再初期化時に Win32 の MainViewport 接続も復元した

## 未実施・残る制約

- 実入力機器から光子までの遅延、Tearing / 動きの見た目、VRR / 複数 Monitor / 他の Fullscreen App は未検証
- 新しい制御が旧実装より高速になったとは断定しない。同条件の Mode 比較値は [測定記録](README.md) に分離する
- DisplayPrediction は Refresh 位相から開始候補を導く機能。GPU 負荷を予測して指定した表示時刻を保証するものではない
- Native 再構築途中の GPU / Allocation / Callback 失敗は安全停止とし、自動的な World / GPU 復旧は行わない
- 初期 World と補間用 Snapshot の内容はゲームが所有する。previous/current 補間を使うか最新状態を直接描くかはゲームが選ぶ
