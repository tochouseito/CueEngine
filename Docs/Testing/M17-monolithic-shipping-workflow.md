# M17 Monolithic Shipping Workflow Verification

## Purpose

Release Shipping ProductがEditor／Source／Build Treeから独立した最小Player Packageとして生成され、移設後も起動し、
改ざんをfail-closedで拒否し、停止後にProcessとStagingを残さないことを確認する。

## Automated Verification

正本の自動検証はRelease構成の次のTestである。

```powershell
cmake --build --preset windows-vs2026-release --target CueEditorWorkflowProcessTests -- /m
ctest --preset windows-vs2026-release -R "^Cue\.Editor\.Workflow\.ProcessRoundTrip$" --output-on-failure
```

Testは次を自動実行する。

1. Blank ProjectとSceneを生成し、Editor Workflowを実行する。
2. `StartShipping`でRelease／Monolithic／UnsignedLocal Productを強制Configureから生成する。
3. ProductをSmoke起動して正常終了を確認する。
4. ProductをInteractive起動し、対象ExecutableのProcessが所有する可視Top-level HWNDを観測してからStopする。
5. D3D12 Render Loopの正常Shutdown marker、5秒の強制終了fallback到達前のProcess回収、`PackageReady`復帰を確認する。
6. Packageを別DirectoryへCopyし、Project Sourceを一時的に利用不能にする。
7. PackageがEngine Build Rootの絶対Pathを含まないことを確認し、無関係なCurrent Directoryから移設済み
   `CueGameProduct.exe --package-smoke-test`を実行する。
8. EXE、Runtime Scene、Project ID、Configuration、Architecture、Role、追加DLLの各改ざんを拒否する。
9. Packageの相対Path、Entry種別、全File Byte列が実行前後で一致し、Project内にStaging Entryがないことを確認する。

## Manual Window Check

自動Testは対象ExecutableとFile Identityが一致するProcessの可視Window生成から正常Stopまで検証する。
目視で再確認する場合は、CueEditorToolで対象Projectを開き、
Build & PackageからRelease Shippingを生成してRunする。次を確認してからStopする。

- `CueGameProduct`のWindowが表示される。
- Editorが実行中状態を表示する。
- Stop後にWindowが閉じ、再度Runできる。
- Package Directoryに`CueGameProduct.exe`、`CuePackage.json`、Runtime Project、Runtime Scene以外が増えない。
- Projectの`Generated`配下に`CueStaging-`または`*-staging` Entryが残らない。

## Expected Failure Cases

改ざん用Copyは起動してはならず、非0 Exit Codeを返す。元Packageは変更しない。公開配布用署名を持たない
`UnsignedLocal` ProductはLocal開発用としてのみ扱い、配布可能と表示してはならない。

## Evidence

測定条件、Baseline値、未実行項目、Residual Riskは`Docs/Evidence/M17/ShippingSecurityRelocation.md`を参照する。
