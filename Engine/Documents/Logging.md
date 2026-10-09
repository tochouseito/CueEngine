# Logging

対象: M08 #110。Host と既存診断の接続は #111

設計: [ADR-0008](../../Docs/Decisions/0008-logging-and-diagnostic-routing.md)

## Module と所有権

- Foundation: LogLevel、呼出中だけ文字列を借用する LogRecord、ILogger / ILogSink
- Cue.Logging: Logger の Level フィルタ・整形・同期配送、抽象 File API を使う FileLogSink
- Cue.Platform.Windows: Windows DebugSink の OutputDebugStringW
- Host: Logger / Sink の所有、保存先、利用者停止後の Flush / Close（#111）

Logger は Sink を一意所有する。ILogger を借りる利用者は Logger より先に停止する。Sink は入力を同期で消費し、借用文字列を保存しない。FileSystem は FileLogSink より長く維持する。Logging は Windows / DX12 / Editor の実装へ依存しない

## 出力と失敗

Trace / Debug / Info / Warning / Error / Fatal を持ち、既定の最低 Level は Info。UTC の ms 時刻、Level、Thread ID の Hash、発生元、本文、任意の Error 分類と Native 値、呼出位置を UTF-8 の一行へ整形する。LF / CR / NUL / Backslash は Escape し、File と DebugSink には同じ行を渡す。入力文字列は有効な UTF-8 を使用する。DebugSink は不正 UTF-8 を Error にする

Logger の write / flush / shutdown は同期し、Sink から Logger への再入は InvalidState。別 Logger を経由して元の Mutex を待つ循環も拒否する。低頻度の診断を想定した同期実装であり、非同期 Queue・Rotation・容量上限は今回含めない。Sink 失敗・例外後も他の Sink を試み、最初の失敗を Result に返す。ログ結果は業務処理の Result を置換しない。整形や Error 値生成の Allocation 例外は呼出側で扱う

FileLogSink は File を保持し部分書込みを繰り返す。進捗なし・書込み失敗後は以後の追記を拒否し、部分行の重複を避ける。既定は CreateNew で既存 File を保全する。明示した append は既存末尾を使うが同時 Writer は許可しない。Flush は OS Buffer の Flush で、電源断耐性の保証ではない

全利用者を停止して Logger.shutdown の Result を確認する。全 Sink を Flush / Close し、以後の write は拒否する。Close の失敗は次の shutdown で再試行する。Destructor は最後の回収だけを試みるため、停止失敗の確認は明示 shutdown で行う

DebugSink は UTF-16 に変換して OutputDebugStringW へ渡す。Debugger の受信確認は Native API から取得できない。受信画面の確認と変換・配送テストは別の検証として扱う

## 検証

Cue.Logging.Logger は Level フィルタ、二出力先の同一行、UTC・Unicode・改行 Escape、4 Thread の 400 Record、再入拒否、Sink 失敗 / 例外、停止後の拒否、File の新規作成 / 追記、部分転送、進捗なし、Flush / Close 失敗と再試行を確認する。PublicHeaders Test は Windows SDK を含めず公開 Header を Compile する
