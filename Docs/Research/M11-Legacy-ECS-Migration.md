# M11 旧 ECS の移植

- Date: 2026-10-29
- 指示: 旧 Engine の ECS をそのまま独立 Module として移植する
- 作業 Branch: `m11/issue146/project-loading`（#146 の未コミット変更を保全。新規 Branch / Issue / Commit は作成していない）
- Source: `tochouseito/CueEngineLegacy` の `develop`
- Source Commit: `8476b656c24e4ef6262761fefed9b37566932306`
- 元 Directory: `Engine/Source/Runtime/ECS`
- 移植先: `Engine/Source/ECS`、Target `Cue.ECS`、Namespace `cue::ecs`

## 移植範囲

旧 `ECSManager.h` の構造を基に移植し、その後の明示指示で下記の Staging / MultiComponentSystem の不具合を修正した。

- Entity の生成・再利用・Active、Component の追加・取得・削除・複製
- 単一 Component の密な Vector Pool、複数 Component の Entity 別 Vector
- Archetype の条件による System 実行、優先度、有効無効
- Awake / Initialize / Update / Finalize と Context
- 独立した SystemPipeline、更新中の Staging、Deferred Command
- Entity / Component の Listener、コピー・削除優先度
- Prototype の複製・復元・子 Prefab
- System と段階ごとの経過時間取得

ゲーム用の Transform / Camera / Mesh Component、Script 実行、Scene 永続化、Runtime Session への接続は追加していない。
ECS 本体は Foundation のみへ依存し、Platform / Runtime / Math / Renderer / Editor に依存しない。

## 必要な適応

1. Namespace を `Cue::ECS` から `cue::ecs` へ変更した。
2. 公開 Include を `<ECS/ECSManager.h>` / `<ECS/ComponentIDHelper.h>` とし、CMake Target を `Cue.ECS` にした。
3. 自己完結した標準 Header と `std::size_t` / `std::uint32_t` の修飾を追加した。
4. 旧 `Core::Time::IClock` / `Math::TimeSpan` 依存を、`clockCallback` と `std::chrono::nanoseconds` に置き換えた。
5. 時間差の ms 換算を `std::chrono::duration<double, std::milli>` にした。未指定の時計は旧実装と同じ零計測。
6. 新規境界の所有・Thread・借用期間を追記し、Repository の clang-format を適用した。

時計は、Host の Clock を捕捉する Callback で接続できる。捕捉先は ECS より長く生存し、Callback は単調な ns を返し、
例外を送出してはならない。同一 Manager の全操作は外部で直列化し、実行中に再入・時計変更をしない。

## 互換性を優先した Coding Rules の例外

「そのまま移植」の明示指示を優先し、旧 API 名（`ECSManager` / `ComponentID` など）、内部名、既存コメント、
共有 Storage / Prototype の `shared_ptr`、Pointer / Reference / 例外を返す旧契約を維持した。
既存コメントすべてを `@brief` へ書き直す変更はしていない。新規の接続・Test・CMake は現行規約に従った。

Prototype の必須取得 API は欠損時に `std::runtime_error`、任意取得 API は nullptr を返す。Allocation と利用者 Callback の例外も
Result へ変換していない。新しい Result ラッパーを重ねると、部分更新後に継続できるように誤認しやすいため今回は導入しない。
Callback / Component 操作の例外後は更新を再試行せず、上位の停止境界で Manager を破棄する。自動 Rollback は保証しない。

## 移植後の不具合修正

- Staging は既存 Entity の Archetype と Active を引き継ぐ。確定時に旧 Bucket から新 Bucket へ一度だけ移し、
  新しく条件を満たす System だけを初期化対象にする。非 Active Entity の初期化は有効化まで保留する。
  必須 Component が非 Active の場合も通知を保持し、実際の Initialize Callback 呼出時に消費する。
  独自の `ISystem` で一括初期化を実装する場合も、処理した Entity ごとに保護 Member `mark_initialized` を呼ぶ。
- 新規 Entity、通常追加、Prefab 復元、コピーを共通の Staging 管理へ通す。全 Lifecycle 段階の終了時に確定し、
  削除時は Staging と初期化待ちを破棄する。単一 Component の重複追加は既存の実体を返す。
- MultiComponentSystem の Callback 引数を `std::vector<T>&` から `componentView<T>`（`std::span<T *const>`）へ変更した。
  Awake / Initialize / Update / Finalize の全経路で、Callback 開始時に Active な Component の本体を参照する。
  値の変更は Pool へ直接反映される。View と Pointer を Callback の外へ保持してはならない。
- View の参照先を安定させるため、Callback 内の Entity 生成・Component 追加・コピーは `defer` で予約する。
  直接実行は変更前に `std::logic_error` で拒否する。削除は自動的に遅延され、値や Active の変更は直接行える。
  Pool を直接操作して Storage を再配置することは禁止する。
- `defer` で即時反映する生成・追加・Prefab 復元・コピーでも、新しく Query に一致した Entity を初期化待ちに登録する。
  削除の再予約による無限ループを避けるため、遅延 Command の実行と System の走査中を区別する。

新しい View の利用例:

```cpp
[](cue::ecs::Entity, cue::ecs::componentView<MyComponent> a_components)
{
    for (auto *component : a_components)
    {
        component->value += 1;
    }
}
```

## 引き継いだ制約・未解決の旧問題

2026-10-30 の World 実装時に、削除前の `ComponentPool::cleanup` が `initialize` を再実行していた問題も修正した。
生成の副作用を削除時に再発させず、所有値の回収は Pool の削除 / RAII に任せる。
World の削除時初期化回数と ECS 関連 Test の再実行を含む結果は `M11-World-Validation.md` を参照する。

- Entity は世代なしの 32-bit 再利用 ID。別 Manager、削除後、保存データへそのまま渡さない。生存 Entity の削除を一度だけ行う。
- Component ID は RTTI 名を使う実行時 ID。Archetype は 256 型。永続 ID / DLL ABI として使わない。
  複数 DLL 間での Registry 共有は未検証で、将来の Game Module 境界で別途設計する。
- Component Pointer は Vector の増設・削除・末尾移動・Staging Flush・Manager 破棄で失効し得る。
- SystemPipeline は Manager 所有 System を借用するため、Manager より先に利用を終える。Listener は weak_ptr で、利用側が所有を保持する。
- `add_component` / `ensure_pool` は既存 Pool があっても一時 Pool の予約 Allocation を行う。高速化は今回の対象にしていない。
- 同じ種類の System が複数ある場合、型名で保持する計測値は最後の実行値になる。

上記の制約は今回の修正範囲に含めていない。旧問題を観測していた Test は `test_fixed_behaviors` へ置き換え、
修正後の契約を確認する。複数操作を組み合わせる回帰 Test は `Engine/Tests/ECS/RegressionTests.cpp` に置く。

## 出典と License

同じ所有者の旧 Repository の自作実装で、第三者 ECS Library は利用していない。
元 Root の MIT License 本文を `Engine/Source/ECS/LICENSE.txt` にそのまま保持した。
Copyright の placeholder は元の表記であり、新しい権利者や年を推定して書き換えていない。

| 元 File | SHA-256 |
| --- | --- |
| `ECSManager.h` | `7D5DB797F448E71A2EA549B53C62F26AE5D8F57CF92CB453E7744A2F94C27C98` |
| `ComponentIDHelper.h` | `9C5B758E0E8D608002012B9E861F52E949D21EF0E08CA5406D8FEEECCF889504` |
| `EngineComponentID.cpp` | `799461AA34B529541E5E673BA8FD37D872975DF1012135E18607B2C4CA52F6EE` |

## 検証

`Cue.ECS.PublicHeaders` は Platform / Math / Renderer の Header なしで ECS を Compile する。
`Cue.ECS.LegacyParity` は別翻訳単位・並行登録、Entity / 単一・複数 Pool、通知、コピー、Active、実行段階・順序、
Staging / Deferred 削除、Prototype・子 Prefab、時計の 1 ms 換算と未指定時の零計測を確認する。

最終結果:

- `pwsh -NoProfile -File scripts/codex_build.ps1`: Debug Build 成功。
- `ctest --preset windows-vs2026-debug -R '^Cue\.ECS\.' --output-on-failure`: 2 / 2 成功（0.18 秒）。
- `git diff --check`: 成功。
- 初回移植時に元と移植先の License の SHA-256 一致を確認した。
- 既存 Archetype / Active の保持、Bucket の一意性、初期化回数、Prefab と Staging コピー、
  削除と ID 再利用、Pipeline の保留初期化を検証した。
- Multi View の全 Lifecycle と Context 経路で本体 Pointer と値の反映を確認し、
  Staging 本体の参照、直接追加の拒否、遅延追加・削除と初期化も検証した。
- Single / Multi の Initialize Callback が後続 Entity の Active を切り替える場合も、通知の欠落・重複がないことを検証した。
- 修正後の Log は Git 管理外の `out/validation/ECSFixes/DebugBuild.log` と `DebugCTest.log` に保存した。

今回は独立 Module の追加のため ECS の関連 Test を実行し、既存の全 Test は再実行していない。
Development / Release、Sanitizer、DLL 連携、実ゲーム Scene での利用は未検証。
