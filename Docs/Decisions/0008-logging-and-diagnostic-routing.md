# ADR-0008: Logging の所有、出力と既存診断の接続

- Status: Accepted（ログ基盤と既存診断統合の採用指示に基づく）
- Date: 2026-10-09
- Issues: M08 #110、#111

## Decision

Foundation は LogLevel、同期借用の LogRecord、ILogger / ILogSink の最小契約を持つ。Cue.Logging は整形・Level フィルタ・同期配送と FileLogSink を実装し、抽象 File API に依存する。Windows の DebugSink は Platform.Windows に置き、OutputDebugStringW を使う。Host が Logger と Sink を所有し、利用者へ ILogger を借用する

Record は UTC の ms 時刻、Level、Thread ID の Hash、発生元、本文、任意の Error 分類・Native 値・呼出位置を一行へ整形する。UTF-8 を使用し、LF / CR / NUL / Backslash を Escape する。File と DebugSink は同じ整形結果を受け取る。Log は Result を置換せず、元の処理の失敗伝播は呼出側に残す

初期実装は Mutex で同期し、Sink から Logger への再入を拒否する。Sink の失敗後も他の Sink を試み、最初の失敗を返す。File の部分書込み後は同じ Record を再送しない。書込み失敗後の FileSink は以後の追記を停止する。明示 shutdown は Flush / Close の結果を報告し、利用者停止後に呼ぶ。Allocation 例外は通常 API では呼出側へ伝播する

既存の引数に ILogger がない診断 API は、Host が所有する登録の寿命に限定して明示的に Logger へ橋渡しする（#111）。登録解除は処理中の配送終了を待つ。登録前・解除後・再帰・ログ配送失敗は再帰しない緊急デバッグ出力へ退避する。この互換接続に Logger の所有権や汎用 Service 検索を持たせない

File 出力は全構成で既定有効、DebugSink は Debug / Development で既定有効、Release で既定無効とする。保存先は #108 の StoragePaths.logs。Host は Worker / GPU / UI / Window を停止してから登録解除・Logger 停止・FileSystem 回収を行う。Rotation・保持期限・非同期 Queue は必要性を計測した別機能とする

## Validation

公開 Header の Windows 非依存、Level 選別、同一行の複数配送、Unicode・改行、並列呼出し、再入、部分転送と失敗、Flush / Close と再試行をテストする。Host 接続では旧 API・正常起動・失敗 Rollback・停止・登録解除の競合を検証する。Native Debugger の画面上の受信は自動テスト結果と区別して報告する
