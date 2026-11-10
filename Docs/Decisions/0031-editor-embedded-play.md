# ADR-0031: 別 Process の Play を Editor SceneView に表示する

- 状態: 採用
- 日付: 2026-11-09
- 関連: ADR-0016、ADR-0020、ADR-0022

## 目的

Editor 内で実行中の Scene を観察・操作しながら、Native Game Code の Crash を Editor から分離する。
同じ Process 内で Access Violation を捕捉して継続する方式は採用しない。

## 所有と実行

- Editor は引き続き Game DLL を読み込まない。`CuePlayHost` が GameSession と Script を所有する。
- Editor が解決する Play 設定は `isHeadless=true`、`isEmbedded=true` とし、子に Window / SwapChain を作らない。
- 子の単一 Owner Thread で IPC、更新、Snapshot 抽出を直列化する。
- 更新の開始頻度は既存 FrameController の単一 Thread / 60 FPS 構成で制御する。
  現行の埋込み Play は採用 Frame ごとに 16,666,667 ns の更新を一回行う。
  過負荷や Debugger 停止中の時間を大量の追い付き更新へ変換しない。
- Window を表示する従来の PlayHost 起動設定、製品 App の実行は維持する。

## SceneView

- 子は更新確定後の `SceneRenderSnapshot` と永続 Object ID / Local Transform / Tick を所有値で返す。
- Editor は既存 SceneRenderer で値を描画し、既存 SceneView の Texture に表示する。
- 共有 Texture や子 Window の埋込みは用いない。現行の Cube / Camera 描画契約を転送する。
  将来、任意の Runtime 固有 Pass の最終画面を表示する GameView は別の画像転送契約として設計する。
- SceneView の Camera は Editor のみの所有値で上書きできる。
  右 Mouse Drag、WASD / QE、Wheel で操作し、`Scene camera` で実行 Scene の Camera 表示へ戻す。
  Runtime Camera / 保存 Scene は変更しない。

## IPC と失敗

- Protocol を v2 に更新し、同じ Protocol の Editor / PlayHost を組み合わせる。
- 親からの View 取得要求は常に一件だけとする。子は要求後の更新確定値を一件返し、Frame を蓄積しない。
- Session / Request、件数、UUID、Mesh ID、有限行列、Camera 投影、Transform を検証する。
  上限は描画 Object / Transform 各 4096、既存の Message / Queue 容量制限も維持する。
- Editor は実 Aspect / Editor Camera を反映した行列積も Render Worker へ渡す前に検証する。
  Game 由来の数値異常では Play だけを停止し、編集用 Renderer を終端失敗へ遷移させない。
- Stop の予約時点で表示 Snapshot を失効させ、停止・失敗・再起動でも古い値を公開しない。
- Crash は既存 Process 終了監視で検出する。通常の View 無応答は 15 秒、Stop 無応答は 2 秒で所有 Job を回収する。
  Debugger 接続中の通常待機期限は延長し、Breakpoint と Hang を混同しない。Stop の期限は維持する。

## Inspector と編集正本

- Play 中の Transform 表示は子の最新確定値を使い、入力中だけ同期を抑止する。
- Transform 変更は Session / Request 付き Command とし、子の次回更新前に適用・確認応答する。
- Script Property の既存の Command / Ack 経路も維持する。
- Play の Transform 編集は Authoring の Begin / Preview / Commit、Undo、保存処理を呼ばない。
- Escape は入力開始時の姿勢を子へ要求する。Script が同じ値を更新する場合、その後の Tick で再変更され得る。
- Stop / Crash で Play 用の表示と入力を破棄し、未保存の SceneDocument と Preview World をそのまま表示する。

## 検証

- UI: Play 数値入力、入力中の同期抑止、Escape、Session 分離、Camera 操作、編集正本の不変性
- 実 Process / WARP Editor: 回転 Snapshot、Transform の変更、Stop / 再 Play、Crash / Hang 後の描画継続
- Protocol: 不正 Snapshot、容量超過、未知 Request、未要求 Transform 応答の拒否
- Game 由来の有限な巨大行列による描画 Overflow を Play 停止へ限定

ゲームへの入力転送、動的 Entity の Inspector 同期、製品への遠隔 Profiler 接続は別契約とする。
