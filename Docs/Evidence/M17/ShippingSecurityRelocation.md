# M17 Monolithic Shipping Security and Relocation

## Result

Issue #306のRelease Shipping End-to-Endを2026-09-12に実行し、`Cue.Editor.Workflow.ProcessRoundTrip`が
93.70秒で成功した。テストはEditorの実CompositionからShipping Productを強制Configureして生成し、
Smoke起動、実Windowを伴うInteractive起動、要求停止、別Directoryへの移設、Source非表示、改ざん拒否、
Package Tree不変、Staging回収を一続きで検証する。

| Acceptance Gate | Result | Evidence |
| --- | --- | --- |
| Game DLL、LIB、PDB、Source、Build Logなし | Pass | Package全Treeを列挙し、Manifest、EXE、Runtime Project、Runtime Sceneの4 Fileだけと比較 |
| Package移動後の起動・正常停止 | Pass | `RelocatedShippingPackage`を無関係なCurrent DirectoryからSmoke起動しExit Code 0を確認 |
| 実Windowでの起動・停止 | Pass | `CueGameProduct.exe`と同一File Identityを持つProcess所有の可視Top-level HWNDを確認後にStopし、D3D12 Render Loopの正常Shutdown marker、5秒の強制終了fallback到達前の完了、`PackageReady`復帰を確認 |
| Build環境からの独立 | Pass | Project Sourceを一時的に別名へ移し、Engine Build Rootの絶対Pathを含まない移設Packageを無関係なCurrent Directoryから起動 |
| Tamper／不正Identity／不正Inventory拒否 | Pass | EXE、Runtime Scene、Project ID、Configuration、Architecture、Role、追加DLLの独立Copyを全て非0終了で拒否 |
| Package RootへのUser Data書込みなし | Pass | 実行前後の相対Path、Entry種別、全File Byte列が一致し、最終Inventoryも4 Fileだけ |
| Process／Staging cleanup | Pass | Interactive Stop後にChild Process完了、Active Stageなし、Recovery Stagingなし、Project Tree内のStaging命名なしを確認 |
| Size／Link／Startup Baseline | Pass | 本文の測定条件と数値を記録 |

Manifestの`fileCount`はManifest自身を除く3 Entryで、物理Packageは`CuePackage.json`を含む4 Fileである。

## Security Compatibility Findings

実Productで使われたMSVCのCFG metadataには`GuardAddressTakenIatEntryTable`が含まれる。このTableを一律拒否せず、
Load Configのstride、Import Descriptorから列挙した実在する非null `FirstThunk` RVAへの所属、8-byte境界、厳密昇順、
予約metadata byteの0、read-only Section、範囲上限、Base Relocationを検証した場合だけ受理する。不正RVA、IAT内の
null terminator、非整列、非昇順、予約Byte非0、writable／executable Section、Tableと一部だけ重なるDIR64、
Table PointerのDIR64欠落を独立した改変PEで拒否する。
Import Address Tableはallocation前に65,536 Entryを上限として検証し、過大なDirectory Sizeをresource-limit違反として
拒否する。各IAT slotは固定長bitmapで使用済み状態を追跡し、複数Import Descriptorによる同一slotの再利用を
収集時点で拒否する。

D3D12 Runtime Importは現行Windows SDK Linkで`d3d12.dll`のordinal 101として生成された。M17ではLibrary名と
ordinalの組を`d3d12.dll`／101だけに限定する。Test fixtureでそのordinalをAddress-taken IAT Tableへ列挙した場合は、
実在`FirstThunk` RVAとの一致を確認する。直接IAT Callだけを行いImport SymbolのAddressを取得しないProductには
Table登録を要求しない。ordinal 102と同じordinalを別Libraryへ設定した改ざんPEは拒否する。
ordinal encodingはflagと16-bit ordinalだけを含むcanonical値に限定し、予約bitを含む値も拒否する。
許可範囲は名前Import全般へ拡張せず、既存のLibrary allowlistとLoader API禁止を維持する。

PE／Load Config／CFGの判定根拠はMicrosoftの
[PE Format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)、
[IMAGE_LOAD_CONFIG_DIRECTORY64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-image_load_config_directory64)、
[PE metadata](https://learn.microsoft.com/en-us/windows/win32/secbp/pe-metadata)を正本とする。

Shipping Toolchain Evidenceは、生成Treeの実配置`Source/Game/CueGameProduct.vcxproj`を読み、明示された
完全な`VCToolsVersion`、`Microsoft.VCToolsVersion.<major.minor>.props` Import、Build metadataの完全なCompiler File
Versionを照合する。Visual Studio配置Pathは、生成ProjectのXML Attribute表現に合わせてEscapeしてから照合する。
Shipping Workspace Keyは長いSource PathでもWindowsのPath制限内へ収めるため、情報を欠落させず短縮した。

## Performance Baseline

これはM17最小Productの初回Baselineであり、性能目標の達成や最適化効果を主張する値ではない。今後は同じMachine、
Release x64、入力Project、UnsignedLocal Trust、計測方法を固定して比較する。

- 測定日: 2026-09-12
- OS: Windows 11 Home 10.0.26200（Build 26200）
- CPU: AMD Ryzen 7 3700X、8 Core／16 Logical Processor
- Memory: 17,058,983,936 bytes
- Build: Release x64、UnsignedLocal
- CMake: 4.2.3
- MSBuild: 18.10.1.42706
- MSVC Linker: 14.51.36257.0
- Product size: 779,264 bytes
- Link target: 440 ms（Link task 439 ms）
- Product-only Rebuild wall time: 4.46 s
- Startup first run: 173.15 ms
- Startup warm runs: 140.66、143.66、130.35、163.90、154.44 ms
- Startup warm median: 143.66 ms
- Startup Exit Code: 6/6で0
- Startup前後のPackage Tree: 一致

Linkは、依存Libraryを既に生成したShipping workspaceで`CueGameProduct.vcxproj`だけを`Rebuild`し、
`BuildProjectReferences=false`、`/m:1`、MSBuild `PerformanceSummary`で測定した。Source非表示を模した後の
workspaceを使用したため、削除済みProject Referenceに対するMSB9008が19件出たが、ProductのCompile／Linkは成功した。
このため440 msは最終Product LinkのBaselineであり、依存Moduleを含むClean Build時間ではない。

Startupは移設済みPackageを無関係なCurrent Directoryから`--package-smoke-test`で6回連続起動し、Process作成から
正常終了までをStopwatchで測定した。初回をcold-ish値、後続5回の中央値をwarm値とする。OS filesystem cacheを
明示flushしていないため、完全なCold Start値ではない。

## Automated Coverage

- `Cue.Build.Plan`: Shipping workspace keyの完全性、Trust分離、長さ上限
- `Cue.Build.Windows.ArtifactPublisher`: 実生成TreeからのToolchain Evidence取得と厳密照合
- `Cue.Build.Windows.ProductSecurity`: PE Hardening、Import allowlist、D3D12 ordinal 101、CFG auxiliary table、署名Policy、改ざん拒否
- `Cue.Editor.Workflow.ProcessRoundTrip`: Editorからの生成、実行、停止、移設、Source非表示、Package改ざん、後始末
- #304／#305の既存Security Test: Reparse Point、Snapshot Lease、TOCTOU候補、Malformed PE、署名Evidence

## Not Run

- 公開配布用CertificateによるPublisherSigned Productの実署名とTrust Chain検証
- 権限制限Tokenまたは別Standard User Accountでの実行
- ACLまたは隔離環境でEngine Source／Build TreeへのAccessを実際に拒否した起動
- Cold filesystem cacheを明示したStartup測定
- 別Machine、HDD、低性能CPU、Windows別Buildでの測定
- 長時間Soak、Crash dump、電源断相当の復旧
- AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer

## Remaining Risks

- `UnsignedLocal`はLocal開発専用であり、公開配布の改ざん耐性を保証しない。
- D3D12 ordinal 101の許可は現行Windows SDK／MSVC出力に固定される。Toolchain更新時は実ExportとPEを再検証する。
- Standard User相当GateはPackage RootのTree不変とRoot分離で検証したが、制限TokenによるACL試験ではない。
- Engine Build Tree非依存は絶対Path非包含、Project Source一時退避、移設、無関係なCurrent Directoryで模したもので、
  Engine TreeへのOS-level Access Denialではない。
- Link／Startup値は一台のMachine上の最小Productに限られ、将来のGame CodeやAsset規模を予測しない。
- `GuardAddressTakenIatEntryTable`の受理はMicrosoft定義の現行metadata範囲に限定し、未知の補助Tableはfail-closedを維持する。
