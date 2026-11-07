# Logging

対象: M08 #110（ログ基盤）、#111（Host と既存診断の接続）

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

## Host と既存呼出し

WindowsHost / EditorHost は既定で FileSink と DebugSink を所有する。HostLoggingConfig で最低 Level、File 有効、Debug 出力の Automatic / Enabled / Disabled を選ぶ。Automatic は Debug / Development で有効、Release で無効。File 出力は全構成で有効。File の Open 失敗は初期化の Result に返す。ログの write 失敗は処理の主原因を置き換えず緊急出力へ報告する

保存先は StoragePaths.logs。Repository の実行 Target は全構成で `<RepoRoot>/out/logs`、製品 Host の既定は `%LOCALAPPDATA%/<company>/<application>/logs`、Portable は実行 File 配置先の logs。Override も #108 と同じ。`CueEngine-<Unix時刻ms>-<PID>-<連番>.log` を CreateNew で作り、既存の起動ログは上書きしない。ログ保持期限は未実装で、長期運用時の回収方針は別機能とする

logger() / log_file_path() は構築 Thread だけで取得する非所有借用。初期化 Callback から shutdown 完了まで有効で、Worker が借りた Logger は Host 停止前に利用を終える。FileSystem、Logger、診断登録、Runtime / GPU / UI の逆順破棄で寿命を保つ。Close 失敗後は Host が所有先を保持し、次の shutdown で再試行する

Engine 内の既存呼出しは report_log / report_log_error に移行した。旧 report_error / report_message は外部利用者の移行用 Wrapper として同じ経路を使用する。Fatal 報告では配送後の Flush も試みる。通常ログは停止時、または ILogger.flush の明示呼出しで OS Buffer を Flush する

引数に Logger のない既存 API の接続は、一つの Process に一つの DiagnosticRegistration だけを許す。二重登録は InvalidState。Host はこの登録を所有し、解除時に実行中の配送終了を待つ。現在の WindowsHost / EditorHost の同時起動は同一 Process 内では一つに限定する。別 Process の起動は独立する

登録前・解除後・再入・配送失敗時は Allocation を使わない緊急デバッグ出力へ戻る。緊急出力は Debug / Development で有効、Release では無効。起動時の File / 保存先を取得する前の失敗は File へ保存できない。Init の Rollback 前、停止失敗と終了通知は登録が有効な間に記録する。Native D3D12 / DXGI Debug Layer 自身の出力は各 Native API のままとする

```cpp
// Host が生存する間だけ借用し、Result の伝播は処理側で行う
if (auto *logger = host.logger())
{
    auto written = logger->log(cue::LogLevel::Info, "Application", "started");
    // 出力失敗の扱いは呼出側で選ぶ
}
```

## 検証

Cue.Logging.Logger は Level フィルタ、二出力先の同一行、UTC・Unicode・改行 Escape、4 Thread の 400 Record、再入拒否、Sink 失敗 / 例外、停止後の拒否、File の新規作成 / 追記、部分転送、進捗なし、Flush / Close 失敗と再試行を確認する。PublicHeaders Test は Windows SDK を含めず公開 Header を Compile する

Cue.Logging.DiagnosticRouting は旧 / 新 API、二重登録、Fatal Flush、200 並列 Record、再帰・Sink 失敗 / 例外、配送中の登録解除待機、解除後の非配送と再登録を確認する。Cue.Logging.Host は実 File の UTF-8 と Error Metadata、初期化失敗の保存と Handle 解放、Editor の通常 Frame / 停止、Open 失敗 / File 無効、Close 失敗後の Host 再試行を検証する

2026-10-09 の最終コードで `scripts/codex_build.ps1 -Configuration <構成>` と構成別 CTest Preset を実行した。

| 構成 | Build | CTest | Test 所要時間 |
| --- | --- | --- | --- |
| Debug | 成功 | 45 / 45 成功 | 31.22 秒 |
| Development | 成功 | 45 / 45 成功 | 24.75 秒 |
| Release | 成功 | 45 / 45 成功 | 24.82 秒 |

MSVC の警告・Error と `git diff --check` の整形違反はなかった。DebugSink の UTF-8 変換・Native 呼出し・無効化は Test 済みだが、Visual Studio の受信画面の目視確認は未実施。
