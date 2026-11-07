# M06-15 / Issue #144 — デスクトップ PC の現在の表示担当 GPU

- 調査日: 2026-10-24（日本時間）
- Branch: `m06/issue144/gpu-waits-and-telemetry`
- 調査対象 Commit: `ba1c485d895683cc50e7db390b3ac27a8f2a01a7`
- 目的: 引き継ぎ元のノート PC と区別し、このデスクトップ PC の表示担当 GPU を確認する。

## 結論

**現在有効なモニターへの表示は NVIDIA GeForce RTX 3060 が担当している。**
`QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` が返した Source / Target はどちらも同じ NVIDIA Adapter であり、
内蔵グラフィックスを経由する有効な表示経路は取得されなかった。
表示設定、GPU 選択、Driver 設定は変更していない。

## 取得した事実

| 項目 | 取得値 |
|---|---|
| PC | ASUS / System Product Name、PCSystemType 1（Desktop） |
| CPU | AMD Ryzen 7 3700X 8-Core Processor |
| Windows が認識する VideoController | NVIDIA GeForce RTX 3060、1 台 |
| Vendor / Device | `10DE / 2503` |
| DriverVersion（Windows 表記） | `32.0.16.1742` |
| 有効な表示経路 | 1 本、`\\.\DISPLAY2` |
| モニター | ASUS VP228 |
| Source Mode | 1920 × 1080 |
| Target Refresh | 60 / 1 Hz |
| Source Adapter LUID | `0x00000000000117F2` |
| Target Adapter LUID | `0x00000000000117F2` |
| Source / Target の Adapter Device Path | 両方 `PCI#VEN_10DE&DEV_2503&SUBSYS_B63019DA&REV_A1#4&2ae1b128&0&0019` |

Adapter Device Path から取り出した PnP Device ID は、`Win32_VideoController` の RTX 3060 の
PNPDeviceID と一致した。Source / Target の両方について一意に対応することを検査した。
LUID は今回の照会値であり、別 PC や再起動後に固定して使わない。

## 引き継ぎ元との違いと調査範囲

2026-10-23 の比較は RTX 3060 Laptop GPU と Intel UHD Graphics を持つ別環境である。
そこで確認された「RTX 描画 → Intel 表示」という経路を、このデスクトップ PC の原因として扱わない。

今回は Windows の現在の有効表示構成を読み取った。CueEngine の起動、FPS の再計測、ETW の再取得は
行っておらず、実行時の Command Queue / Present Context の対応や性能改善率は確認していない。
Engine Source / Build 設定は変更していないため、Build / CTest は実行していない。

## 証跡

以下は Git 管理外のローカル記録であり、別 PC には自動では引き継がれない。

- `out/validation/Performance20261024/Display144/InspectDisplay.ps1`: 読み取り専用の表示構成取得処理。
- `out/validation/Performance20261024/Display144/DisplayPaths.json`: 実際の照会結果。

取得処理は Native 構造体の寸法、各 API の戻り値、Adapter 対応の一意性を検査する。
表示構成の変化で容量不足が返った場合は再取得し、取得できなかった値を正常結果にしない。

## 同日の性能測定後の再照会

[Double Buffering 基準測定](M06-15-Desktop-DoubleBuffering-Baseline.md)後の再照会では、
BenQ GL2480（1920 × 1080 / 74.973 Hz）と ASUS VP228（1920 × 1080 / 60 Hz）の 2 経路が有効だった。
どちらも Source / Target は同じ RTX 3060 の LUID に対応する。
上記の 1 経路という記載は最初の照会時点の状態であり、後のモニター構成を固定するものではない。
再照会結果は `out/validation/Performance20261024/Desktop144/DisplayPathsAfter.json` に保存した。

参照: [QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig)、
[DISPLAYCONFIG_ADAPTER_NAME](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_adapter_name)。
