# M16 Standalone Package Relocation and Reproducibility

## Scope

Issue #235は、M16のPackageがProject Source、Workspace Build Cache、Current Directoryへ暗黙依存せず、
同一の公開入力から決定的な内容を生成することを検証する。

## Automated Verification

`Cue.Package.Publisher`は、同じCanonical Manifestと同じPayload Byte列を異なる新規Destinationへ公開し、
Manifestを含む全Fileの相対Path、File数、Byte列が一致することを確認する。Failure Injection、Cancel、
既存Destination拒否、Rollback、Durability Unknownの既存検証も同じTestで維持する。

`Cue.Editor.Workflow.ProcessRoundTrip`は、実Editor Compositionから同じProjectを2回Build／Packageし、
次を確認する。

- 2回のPackageでRuntime Project DataとStartup Scene Runtime DataがByte一致する
- Package内のJSON DataにDrive Absolute PathまたはUNC Pathが含まれない
- PackageをProject外の新規DirectoryへCopyしてもManifest Byte列が変わらない
- 元Project Rootを削除せず別名へ移して参照不能にする
- 無関係なCurrent Directoryから、移動後の`CueRuntimeHost.exe --package-smoke-test`が成功する

このProcess TestはDebug、Development、Releaseで同じ経路を使用する。

## Reproducibility Boundary

Package Publisherへの入力は、Build Artifact Inventory、RuntimeHost Byte列、Canonical Runtime Dataである。
同じ入力SnapshotからのPackage ContentはByte一致する。別Build Operationは新しいArtifact Identityを持つため、
`CueGameModule.metadata.json`の`artifactId`とそれを参照するManifest Hashは意図的に変わる。

MSVCが生成するDebug情報付きPEとPDBには、ToolchainのTimestamp、GUID、Build Workspace Pathが含まれる場合がある。
PDBはStandalone Package対象外であり、PackageのJSON、Runtime Data、RuntimeHost探索契約へMachine Pathを持ち込まない。
別Machine間のGame Module Binary再現性とSymbol Server向けPath正規化はM16の対象外とし、必要なら独立したBuild
Reproducibility Issueで扱う。

## Validation Matrix

| Configuration | Package Publisher | Editor Build／Package×2 | Relocated Standalone |
| --- | --- | --- | --- |
| Debug | Passed | Passed | Passed |
| Development | Passed | Passed | Passed |
| Release | Passed | Passed | Passed |
