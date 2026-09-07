# M13 Native File Dialog Manual Test

Issue #203のNative File／Folder DialogはModal UIを表示するため、CTestではOwner、Request、Thread、Error分類だけを
非対話で検証する。実Dialogは次の手順で確認する。

## Preconditions

1. `cmake --preset windows-vs2026`を実行する。
2. 対象構成をBuildする。
3. `out/build/windows-vs2026/bin/<Configuration>`を開く。

## Open File

1. `CuePlatformWindowsFileDialogTests.exe open`を実行する。
2. DialogがCueEngine所有WindowをOwnerとして表示されることを確認する。
3. `Cue Project`と`All Files`のFilterを切り替える。
4. Fileを選択し、Consoleへ`Selected (unverified):`とAbsolute UTF-8 Pathが表示されることを確認する。
5. 再実行してCancelし、`Cancelled`と表示され、Error終了しないことを確認する。

## Save File

1. `CuePlatformWindowsFileDialogTests.exe save`を実行する。
2. `Cue Scene` Filterが表示されることを確認する。
3. Extensionを省略したFile名を入力し、`.cuescene`がDefault Extensionとして扱われることを確認する。
4. 既存File名では上書き確認が表示されることを確認する。実際の保存処理は行わない。
5. 選択後にConsoleへ未検証Absolute Pathが表示されることを確認する。

## Select Folder

1. `CuePlatformWindowsFileDialogTests.exe folder`を実行する。
2. FileではなくFolderだけを選択できることを確認する。
3. Folder選択時は未検証Absolute Path、Cancel時は`Cancelled`が表示されることを確認する。

選択PathはDialog結果だけでは信頼済みProject Locatorではない。Editorは`ProjectFileService`、Project Hubは
`ProjectHubPlatform`と`Cue.Project`でRoot、Area、Entry Type、Project Descriptorを再検証してから使用する。
