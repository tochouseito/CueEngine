# ADR-0002: Build Systemの正本と構成

- Status: Accepted（Build定義の変更はIssue #5）
- Date: 2026-09-24
- Issue: [M00-03 Build Systemの正本と構成をADRで決定する](https://github.com/tochouseito/CueEngine/issues/3)
- 関連: [ADR-0001](0001-architecture-boundaries.md)、[M00-01 Feature Parity Matrix](../Research/M00-01-Legacy-Feature-Parity-Matrix.md)

## Context

2026-09-24時点の新RepositoryはCMakeの初期雛形である。ルートの`CMakeLists.txt`は`CueEngine/`を追加し、`CueEngine/CMakeLists.txt`は`CueEngine.cpp`と`CueEngine.h`から`Hello CMake.`を出力する実行Targetを作る。C++20指定はCMake 3.12より新しい場合に限られるが、宣言された最小Versionは3.10である。`CMakePresets.json`はNinjaによるx64/x86のDebug/Releaseを持ち、DevelopmentとTestsはない。`Cue Engine.slnx`と`.vcxproj`は存在しない。

引継ぎ資料には、LegacyでCMake正本とMSBuild/Solution直接使用の指示が混在していたと記されている。この作業に提示された`AGENTS.md`にもMSBuildと`Cue Engine.slnx`の運用記述があるが、それを新Repositoryの正本選択済みとは扱わない。**ユーザーは2026-09-24に、将来のRuntimeの複数Platform対応を考慮し、CMakeを正式なBuild定義にする案を選択した。** この決定を正本とし、古い運用記述はIssue #4で新Repositoryに合う形へ整える。

Legacyの[3構成Preset](https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/CMakePresets.json)は参考にする。新Repositoryでも、Editorで作成したProjectをBuild→Package→Runできる到達基準を保持する。ただしLegacyのTarget名やDirectory構造を機械的にコピーしない。

## Decision

### 1. Build定義の正本とBackend

| 要素 | 決定 |
| --- | --- |
| 正式なBuild定義 | Git管理する`CMakeLists.txt`、必要な`CMake/`Module、`CMakePresets.json`を唯一の正本とする。Target、Source、Compiler/Linker Option、依存関係、構成はここから決める。 |
| Windows Generator | 初期の正式経路は`Visual Studio 18 2026`、`x64`。CMakeが生成するVisual Studio ProjectをMSBuildでBuildする。このGeneratorが追加されたCMake 4.2.0を最小Versionとし、Preset SchemaはVersion 9を使う。現在のCMake 4.2.3とVisual Studio 2026のInstallを確認済み。 |
| IDEとCLI | Visual StudioはCMake Projectとして開き、CLI/CIは同じPresetでConfigure/Build/Testする。生成されたSolution/ProjectはBuild Tree内の成果物で、Git管理しない。固定名の`Cue Engine.slnx`は要求しない。 |
| その他のBackend | Ninjaなどは将来同じCMake定義から選択できる。ただし初期Gateで検証していないBackendを正式対応と表示しない。 |
| NuGet | RestoreをBuild手順に含めない。第三者Libraryの扱いは後述する。 |

CMakeのVisual Studio GeneratorはVisual Studio Projectを生成し、Visual Studioは`CMakePresets.json`を直接利用できる。[Visual Studio 18 2026 Generator](https://cmake.org/cmake/help/v4.2/generator/Visual%20Studio%2018%202026.html)、[Visual StudioのCMake対応](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio)を参照する。

### 2. ConfigurationとPlatform

初期の正式構成はWindows x64の`Debug`、`Development`、`Release`。通常開発の既定はDebugとし、Build/Test Presetで構成を選ぶ。Visual Studio Generatorは複数構成を同じBuild Treeで扱うため、`CMAKE_BUILD_TYPE`による構成指定を正式経路に使わない。`CMAKE_CONFIGURATION_TYPES`もこの3構成に限定し、中間生成物と成果物は構成別に衝突しない場所へ出す。

| 構成 | 用途と境界 |
| --- | --- |
| Debug | 開発・診断。SymbolとAssertを有効にし、Debug Runtimeを使う。 |
| Development | 非Shippingの実運用寄り構成。最適化とSymbol・診断、Assertを併用し、Runtime/第三者Libraryの選択を明示する。 |
| Release | 製品条件の検証。最適化、Shipping定義、配布依存閉包を確認する。 |

具体的なCompiler Flag、MSVC Runtime、Assert/Logの挙動はIssue #5で3構成を実際にBuildして確定・記録する。既存のx86 Debug/Release Presetは正式対象から外す。別Architectureや別Platformは必要性と検証環境が決まった時点でPresetを追加する。

### 3. Target分類とTests

| 分類 | 責務とBuild/Package境界 |
| --- | --- |
| Engine / Runtime Library | EditorとStandaloneが共有する移植可能な契約・実装。ADR-0001の依存方向に従う。 |
| Editor | Windows/ImGuiのAuthoring Host。Editor UI固有の依存をRuntime公開契約へ漏らさない。 |
| Tools | Project生成、Build、Import/Cook、Packageなど開発時の実行Target。 |
| Tests | 通常の開発CMake Projectに含め、3構成でBuildできるようにする。Test実行は明示的なCTest PresetとCIで行う。 |
| Product | Standalone Runtimeと製品生成経路。Editor/Tools/TestsをShipping Packageの依存閉包に含めない。 |

空のTargetは先行作成しない。Issue #5ではC++20の最小実行TargetとSmoke/Test経路を作り、Editor/Tools/Productは対応機能のIssueで追加する。Testsは通常Buildに含めるが、Visual StudioでのF5ごとに全Testを自動実行しない。Build時間が問題になった場合は計測してTarget選択を見直す。

### 4. 第三者Libraryと将来のPlatform

新規LibraryはIssue #3やIssue #5で追加しない。導入が必要になった時は、Version/Registry baselineを固定したvcpkg Manifest Modeを基本とする。Manifestは`ThirdParty/vcpkg.json`、生成Install Treeは`ThirdParty/vcpkg_installed/`とする。CMakeから`VCPKG_MANIFEST_DIR`と`VCPKG_INSTALLED_DIR`を明示し、`VCPKG_MANIFEST_INSTALL=OFF`でBuild中の暗黙Installを避ける。依存取得は別の明示的な準備手順とし、NuGet restoreをBuildの前提にしない。`ThirdParty/THIRD_PARTY_NOTICES.md`と`ThirdParty/Licenses/`に出自と配布条件を残す。具体的なToolchainと取得Commandは最初のLibrary導入Issueで、そのVersionとともに検証する。[vcpkg Manifest](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode)を参照する。

初期Build対象はWindows x64である。CMakeを正本にするのは将来の別Platform用Targetを同じ定義から追加できるようにするためであり、RuntimeのSourceが自動的に移植可能になる意味ではない。Platform API、Graphics Backend、Toolchain、依存Library、CI/実機Testは対象Platformごとに検証する。

## Alternatives

| 案 | 判断 | 理由 |
| --- | --- | --- |
| `Cue Engine.slnx`と`.vcxproj`をGit管理し、MSBuild Projectを正本にする | 採用しない | Windowsの固定Solution運用には適するが、将来のRuntime別Platformに別のBuild定義か正本移行が必要になる。現RepositoryにもSolutionはまだない。 |
| CMakeと`.vcxproj`を両方正本として手動維持する | 採用しない | Source、Flag、依存、Configurationがずれる。 |
| 現在のNinja x64/x86 Debug/Release雛形をそのまま正式採用する | 採用しない | Development、Tests、Target分類、Windows VS Generator経路がない。 |
| 最初から全PlatformのBuildを用意する | 採用しない | 実装と検証環境が未整備で、動かないPresetを増やす。 |

## Issue #5へのMigrationと検証

1. 現行`Hello CMake.` TargetとC++20設定を確認し、CMakeの最小Versionを4.2.0、Preset SchemaをVersion 9へ合わせる。移行が通るまで現行の動作を壊さない。
2. Windows x64のVisual Studio 2026 Configure Presetと、Debug/Development/ReleaseのBuild/Test Presetを追加する。現行Ninja/x86 Presetは検証した正式経路と区別し、不要なものは整理する。
3. SourceとTargetごとにC++20を必須とし、3構成のCompiler/Linker/Runtime設定、出力Directory、TestsのBuildとCTest登録を整える。未実装のEditor/Tools/Productは作らない。
4. `scripts/codex_build.ps1`をCMake Presetの薄い入口として整え、既定Debug x64でBuildする。READMEに環境、Configure/Build/Test/実行、生成物の場所を記録する。CIも同じPresetを用いる。
5. 3構成のConfigure/Build、最小実行、CTest結果を記録する。Release Packageなど未実装機能の結果を先取りしない。失敗した構成は原因と残作業を明記する。

Issue #5で用意するPreset名と基本コマンドは次を契約とする。現在のRepositoryにはまだこれらのPresetがないため、実行はIssue #5の変更後に行う。

```powershell
cmake --preset windows-vs2026
cmake --build --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-development
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-debug --output-on-failure
ctest --preset windows-vs2026-development --output-on-failure
ctest --preset windows-vs2026-release --output-on-failure
pwsh -NoProfile -File scripts/codex_build.ps1
```

このADRではBuild定義やC++ Sourceを変更しない。現時点で確認できたのはCMake 4.2.3、`Visual Studio 18 2026` Generator、Visual Studio 2026のInstall、既存のNinja Preset一覧までであり、新しい3構成のBuild/Test結果はIssue #5で得る。
