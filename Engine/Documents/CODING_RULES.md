# CueEngine コーディング規約

## 目的と適用範囲

この規約は新CueEngineのFirst-party C++、HLSL、CMake、JSON、Build設定に適用する。命名と記述形式は[CueEngineLegacyの規約](https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Documents/CODING_RULES.md)を基準とし、Architectureは[ADR-0001](../../Docs/Decisions/0001-architecture-boundaries.md)、Build定義は[ADR-0002](../../Docs/Decisions/0002-build-system-source-of-truth.md)に従う。規約は未決定のAPI、ABI、永続形式、Error型を決定するものではない。

新規・変更箇所へ適用する。既存の雛形を規約だけのために一括整形せず、触れた範囲を整える。既存の周辺Codeとの一貫性が優先される場合は、差分と理由を作業報告に残す。

## 自動整形

- Repositoryの`.clang-format`をC++とHLSLの整形基準にする。BraceはAllman、Indentと継続行は半角Space 4個、Tabは使わない。
- 1行の目安は120文字とする。意味の分かれる式や宣言は読みやすい位置で分割する。
- CMakeとJSONも半角Space 4個でIndentする。文字Code、改行、末尾改行と末尾空白は`.editorconfig`に従う。
- 自動整形の適用範囲は変更箇所に限定し、無関係な行を大量に変更しない。

## 命名

| 対象 | 規則 | 例 |
| --- | --- | --- |
| `class`、`struct`、`enum` | PascalCase | `RenderEngine` |
| 型Alias | lowerCamelCase | `float4x4` |
| 関数 | snake_case | `get_device()` |
| 引数 | `a_` + camelCase | `a_deviceContext` |
| Local変数、`struct` Member | camelCase | `frameCount` |
| `class` Member | `m_` + camelCase | `m_frameCount` |
| 定数 | `k_` + camelCase | `k_maxBufferSize` |
| 真偽値 | 意味が分かる疑問形 | `isEnabled`、`hasData` |

- 名前はScope内で区別に必要な情報に絞り、対象、役割、属性が伝わる語を使う。型名や上位Contextを機械的に繰り返さない。
- 頭字語は単語として扱う。例: `Http`、`Xml`。既存の公開APIと外部APIの綴りは互換性を優先する。
- `String`、`Ptr`、`Array`など型だけを示すSuffixや、`bufferResource`のような同義語の重複を避ける。役割や所有権を識別する必要がある場合は、その意味を表す名前にする。

## File配置と依存

- First-partyのEngine Sourceと公開Header、内部Headerは`Engine/Source/`を正本とし、責務を持つModuleのDirectoryに置く。公開Headerと内部HeaderをBuild Targetの公開範囲で区別し、他Moduleの内部Headerへ直接依存しない。
- Engine Testは`Engine/Tests/`に、設計決定は`Docs/Decisions/`に、調査記録は`Docs/Research/`に、開発者向け規約は`Engine/Documents/`に置く。TargetとTestは利用経路が生じたIssueで追加し、空のModuleは作らない。
- File名とDirectory名はPascalCaseを基本とする。`CMakeLists.txt`、`README.md`、`.clang-format`、`AGENTS.md`などToolや慣習に指定された名前は例外とする。
- Runtimeの公開契約へEditor、ImGui、Win32、DirectX 12の具体型を漏らさない。Authoring Scene、Editor Document、Runtime World、Render Data、GPU Resourceの所有境界はADR-0001に従う。
- Build Targetと依存の正本はCMakeとする。生成されたSolutionやProject FileをSourceの正本にしない。
- 第三者LibraryのSource、Header、BinaryはFirst-party Sourceと分離する。導入時にVersion、License、出自、配布条件を記録し、`ThirdParty/`の管理方法はADR-0002に従う。第三者Sourceを直接変更せず、必要ならFirst-party Adapterを置く。

## HeaderとInclude

- First-party Headerには`#pragma once`を使う。Headerで`using namespace`を使わない。SourceでもGlobalな`using namespace`を避ける。
- Sourceは対応する自分のHeader、標準Library、外部Library、Project内の別Headerの順に配置し、Group間は空行で分ける。対応するHeaderがないEntry Point等はそのGroupを省く。各Group内は`.clang-format`の順序に従う。
- Headerは自身で必要な宣言をIncludeし、偶然の推移的Includeに依存しない。完全型が不要な箇所では前方宣言を使う。
- 公開Headerから不要なPlatform実装Headerや第三者実装Headerを露出させない。

## 所有権、寿命、Thread

- 一意所有は値または`std::unique_ptr`を基本とする。`std::shared_ptr`は共有所有が要件として必要な場合に限る。所有目的の生Pointerや直書きの`new`／`delete`を使わない。
- 生Pointer、Reference、View、Handleは非所有とし、Owner、失効条件、借用期間を分かるようにする。Callback登録や非同期Taskは解除・取消とOwner破棄の順序を決める。
- 公開APIにはOwner、寿命、呼出Thread、再入可否、失敗後状態を記載する。Thread-safeと明記しないAPIはThread-safeとみなさない。
- Member配置は意味的なまとまりを優先し、必要に応じてPaddingとCacheへの影響を確認する。可変Global状態でOwnerを隠さない。

## Error処理

- 呼出側が失敗を判定できるResultと診断情報を返す。Logだけで失敗を表現したり、Errorを握りつぶしたりしない。
- 変更前に入力と前提を検証する。途中失敗では部分生成物を公開せず、既存の正本と呼出側が再試行できる状態を保つ。保存、Package、Runtime実体化などの具体的なRollback契約は対応するADRとIssueに従う。
- `assert`を外部入力や運用時に起こり得る失敗の唯一の処理にしない。
- Result型、Error Code、Exception境界、Log形式は専用のResearch／ADRで決める。局所実装で全体方針を固定しない。

## コメントとDocumentation

- コメントはCodeから明らかなWhatの言い換えより、理由、制約、所有権、失敗時の意味を説明する。
- C++関数は宣言の直前、宣言のない内部関数は定義の直前に`/// @brief`で目的を記す。Constructor、Destructor、Operator、`main`、Test、内部補助関数も対象とする。Lambdaは非自明な契約や副作用がある場合に記す。
- 公開APIの`/// @brief`だけで契約が足りない場合はOwner、寿命、Thread、再入可否、失敗時の状態を追加する。関数名の言い換えだけを書かない。
- C++とその他のコメントは日本語を基本とし、`.hlsli`は英語とする。話し言葉と敬体を避け、文末の句点を使わない。日本語と英語の間は半角Spaceを置く。
- 区切りが必要なら`// === Include Group ===`、`// --- Function Group ---`、`// Single Purpose`を使い、装飾だけの区切りを増やさない。

## Legacyからの採否と現状の差分

| 項目 | 判断 | 理由・対応 |
| --- | --- | --- |
| Allman、4 Space、命名、`#pragma once`、非所有Pointer、第三者分離 | 採用 | Legacyの規約を新Repoの基本形とする |
| File配置 | 新Repo向けに調整 | `Engine/Source/`、`Engine/Tests/`を基準にし、実際のModule分割は利用経路とADRで決める |
| Error処理 | ADR-0001へ整合 | Resultと診断情報、失敗時の正本保全を必須とし、具体的な型とException境界は保留する |
| Build | ADR-0002へ整合 | CMakeを正本とし、固定名のSolutionを前提にしない |
| 全Lambdaへの`/// @brief` | 変更 | 非自明な契約を持つLambdaに限定し、短い局所処理は周辺Codeで意図を示す |

現行の`CueEngine/CueEngine.cpp`はVisual Studioの雛形で、Tab、Globalな`using namespace std;`、`main`の`/// @brief`不足がある。`CueEngine/`のSource配置、CMakeの2 Space等も本規約と一致しない。これらはIssue #5で最小TargetとBuild構成を作る際に、触れる範囲で移行する。本IssueではBuild定義や雛形を変更しない。3構成のPreset、Test配置、公開Headerの実際のTarget境界はIssue #5以降の実装で検証する。
