# M11 Math Module の実装と検証

- Date: 2026-10-30
- 作業 Branch: `m11/issue146/project-loading`
- Base: `9492422768c3b0350a3033aac07aa920b184450e`
- 契約: [ADR-0010](../Decisions/0010-math-values-and-transform.md)
- 指示: 数学型・計算を専用 Module に集約し、数学的な Transform も同じ Module に置く
- Project / ECS の未 Commit 変更を保全して作業。新しい Issue / Branch / Commit / Push / Merge は行わない

## 変更

`Cue.Math` を Header-only Target として追加し、依存は `Cue.Foundation` のみとした。
`Engine/Source/Math/Public/Math/` に以下を用意した。

- `Scalar.h`: 型制約、角度変換、許容誤差比較、線形補間
- `Vector2.h` / `Vector3.h` / `Vector4.h`: 型と Alias
- `VectorOperations.h`: 共通加減算・積・内積・長さ・正規化・安全な除算・補間
- `Quaternion.h`: Hamilton 積、正規化・逆元、軸回転・Euler XYZ、Vector 回転・球面補間
- `Matrix4x4.h`: 積・転置・逆行列・各変換行列・位置と方向の変換
- `Transform.h`: 位置・回転・拡縮、行列化、補間
- `Math.h`: まとめて利用する公開 Include

Math を使うために ECS や Renderer を Link する必要はない。
TransformComponent、Camera、投影行列、Shader 転送用 DTO、Runtime への接続は追加しない。

## レビューで修正した箇所

- Scalar を Vector と別に型推論し、成分型と同じ場合だけ積・除算を受け付ける。
  `int3 * 0.5` を誤ってゼロ倍に変換する経路を Compile 時に拒否する。
- 逆行列で零 Pivot のみを判定する方法を修正。行が従属している小さい整数の行列でも
  丸め誤差で有限な不正逆行列を返す場合があるため、左右の積の残差を検証する。
  該当する具体例と型混在を回帰 Test に追加した。

## 検証結果

- `pwsh -NoProfile -File scripts/codex_build.ps1`: 最終 Debug Build 成功、Log に Compiler Warning / Error なし。
- `ctest --preset windows-vs2026-debug -R '^Cue\.(Math|ECS)\.' --output-on-failure`: **4 / 4 成功、0.30 秒**。
  Math は Core / PublicHeaders、ECS は PublicHeaders / LegacyParity。
- 各公開 Header をそれぞれ別翻訳単位で Compile し、Include 順に依存しないことを確認した。
- Scalar 混在の拒否、double Storage の精度、constexpr と型制約も Compile 時に検証した。
- 独立した読み取り専用レビューの指摘を修正し、再確認で追加の具体的指摘なし。
- ECS 本体、Project 本体、Editor Main が作業前の SHA-256 と一致することを確認した。
- Serena の Source 検索で Math 内に Win32 / DX12 / ECS / Runtime / Renderer / Logger / FileSystem の参照がないことを確認した。
- `git diff --check`: 成功。

今回の変更は独立した数学 Header とその CMake 登録のため、最終検証は関連 Test に限定した。
全 CTest、Development / Release、他 OS / Compiler、Sanitizer は実行していない。

Log は `out/validation/MathModule/` に置く。`PreservedHashes.json` は作業前の ECS / Project / Editor Main の Hash。
未追跡 File も含む状態は `BeforeStatus.txt` で保全対象を記録した。これらの記録は File 本体のバックアップではない。

## 残る範囲

- GPU / Shader / Scene / Script / ECS との実統合は未実施。
- SIMD 化や高速化は実測していない。Header 化や constexpr が速度改善を保証するとは扱わない。
- 通常演算の Overflow 前提、逆行列の残差許容、整数と実数の機能差は ADR を参照する。
- 既存の Project / ECS の成果は本変更と同じ未 Commit Directory に残す。
