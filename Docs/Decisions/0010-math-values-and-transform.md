# ADR-0010: Math の値型と Transform

- Status: Accepted（基本数学型。Camera / Projection / GPU 転送は後続）
- Date: 2026-10-30
- 関連: [ADR-0001](0001-architecture-boundaries.md)、[Build](0002-build-system-source-of-truth.md)

## 目的と配置

回転する Cube を表示する前に、ゲームと描画から共用する数学型・計算関数を `Cue.Math` へ集約する。
公開 Include は `<Math/Vector3.h>` など、Namespace は `cue::math`。
Template の実装を公開する Header-only Target とし、失敗を返す `Cue.Foundation` の Result のみへ依存する。
Platform / ECS / Runtime / Renderer / Editor / Win32 / DX12 は参照しない。

型は所有する値であり、Global 状態、Heap 所有、File / Log 出力は持たない。
同じ値を並行して書き換えない限り独立した Thread から利用できる。Component / GPU Handle / 親子・Dirty・Cache は持たない。

## 型

| 型 | 成分型 | 既定値 |
| --- | --- | --- |
| `Vector2<T>` / `Vector3<T>` / `Vector4<T>` | float / double / int32_t / uint32_t | ゼロ |
| `Quaternion<T>` | float / double | x=y=z=0、w=1 |
| `Matrix4x4<T>` | float / double | 単位行列 |
| `Transform<T>` | float / double | position=0、rotation=単位回転、scale=1 |

Alias は `float2/3/4`、`double2/3/4`、`int2/3/4`、`uint2/3/4`、`float4x4` / `double4x4`、
`floatQuaternion` / `doubleQuaternion`、`floatTransform` / `doubleTransform`。
Quaternion / Matrix / Transform の Template 省略型は float。bool、狭い整数、long double は今回の対応に含めない。

- Vector は x / y / z / w の必要成分だけを保持する。色の別名や生配列と重ねた Union は使わない。
- Vector は同じ型の加減算、Scalar 積、内積、成分積、3D 外積を提供する。
- Scalar 積・除算で暗黙に別の数値型へ変換しない。例: `int3 * 0.5` は Compile Error。
- 整数 Vector に長さ・正規化・補間などの実数演算は提供しない。
- `operator==` は成分の厳密一致。実数の誤差比較は `nearly_equal` を明示する。
- 強制的な 16-byte 整列、SIMD 型、GPU Constant Buffer 配置は公開契約に含めない。

## 基本演算の規約

- 角度の関数引数は Radian。Degree との変換関数を別に提供する。
- Matrix Storage は row-major、`values[row][column]`。行 Vector を左側から掛ける。
- 行列積 `A * B` は Vector に A、B の順に作用する。平行移動は最後の行。
- Transform は `S * R * T` の順で行列化する。回転は Quaternion を正本とする。
- Quaternion は x / y / z / w、積は Hamilton 積。`a * b` は Vector に b、a の順に作用する。
  従って `matrix(a * b) = matrix(b) * matrix(a)`。行列と Quaternion の積の作用順を混同しない。
- 軸回転は右手則。Z 軸の正の 90 度で +X が +Y へ移る。
- Euler 入力関数は固定軸 X → Y → Z、Quaternion では `qz * qy * qx` として合成する。
- `transform_point` は w=1 の位置を変換して w 除算する。`transform_direction` は w=0 とし、非零 w の結果を拒否する。
  法線の非一様 Scale 補正は逆転置行列の線形成分を使う必要があり、単なる方向変換と同一にしない。

行優先・行 Vector・SRT は旧 CueEngine develop と合わせた。これは Camera の前方軸、World の左右手系、
Projection の Depth 範囲、HLSL の Packing / 転置要否まで確定するものではない。それらは接続する機能で設計する。

## Transform

`Math/Transform.h` に position、rotation、scale を定義する。`to_matrix()` は必要時に行列を作り、Cache を保持しない。
単位長でない Quaternion は変換時に正規化し、入力の正本を書き換えない。零・負の Scale は許可する。
`interpolate` は位置・Scale を線形、回転を最短経路の球面で補間する。係数は [0, 1]。

親子 Transform の合成結果には Shear が含まれる場合があるため、SRT への損失付き変換を暗黙に提供しない。
World 行列の合成・保持は将来の World / TransformComponent 側の責務とする。汎用 ECS へゲーム用 Component を組み込まない。

## 失敗と数値範囲

- 普通の加減乗算・内積・外積は、中間値と結果が型の表現範囲内であることを前提にする。
  signed 整数 Overflow の検査や Saturation は行わない。
- Vector 除算は `/` 演算子にせず `divide` で Result を返す。ゼロ除算、非有限値、signed 最小値 / -1、実数結果の Overflow を拒否する。
- 正規化は最大絶対成分で Scale してから長さを計算し、極小・巨大入力の途中の二乗 Overflow / Underflow を避ける。
  ゼロ長・非有限値は Result の InvalidArgument。単位値への無言の置換はしない。
- 軸回転のゼロ軸、無効 Quaternion、補間係数の範囲外なども Result で拒否する。
- Matrix の逆行列は部分 Pivot 選択で計算し、両側の積と単位行列との差を検証する。
  非有限値、零 Pivot、中間 Overflow、残差超過は失敗。入力・出力引数を途中まで更新しない。
- 逆行列の既定最大絶対残差は `128 * epsilon(T)`。大きな座標等では利用側が許容値を明示できる。
  許容値を増やすと結果の誤差も許す。Condition Number の評価や任意の悪条件行列への精度保証はしない。
- `Result` の Error は Foundation と同じ所有文字列を持つ。失敗の記述には確保が伴う場合があり、
  メモリ不足を含めた無例外・無確保 API を保証するものではない。通常の値演算は Heap を使用しない。

## 旧実装との関係

参照元は `CueEngineLegacy` develop の Math と GpuData/Transform。第三者数学 Library は導入しない。
全体コピーではなく、今回の型・演算契約で基本実装を作る。

- 旧 `Matrix4<T>` の実 Storage が float 固定だった点は継承せず、全成分・演算に T を使う。
- 旧 GpuData の LocalTransform は Euler 回転だったが、今回の数学型の回転は Quaternion。
- 時間単位は既存の `std::chrono` を維持し、旧 TimeUnit を Math へ移さない。
- Windows の Macro、旧 Module 依存、失敗時に identity へ無言で置き換える動作を持ち込まない。

## 検証

各公開 Header の単独翻訳単位、型制約、値型の配置、constexpr、整数計算、float / double の精度、
正規化の極端値、既知座標の回転、Euler / Hamilton の順序、Quaternion と行列の一致、SRT、逆変換、
行従属の特異行列、無効入力、Transform 補間を Test する。結果は Research に記録する。

Camera / Projection、SIMD / 性能最適化、ECS 接続、描画での利用、他 OS の実機検証は本変更の対象外。
