# ADR-0007: Legacy RHI クラス境界の移植と DX12 名称

- Status: Draft
- Date: 2026-09-29
- Issues: [#45](https://github.com/tochouseito/CueEngine/issues/45)、[#46](https://github.com/tochouseito/CueEngine/issues/46)、[#47](https://github.com/tochouseito/CueEngine/issues/47)、[#48](https://github.com/tochouseito/CueEngine/issues/48)、[#49](https://github.com/tochouseito/CueEngine/issues/49)、[#50](https://github.com/tochouseito/CueEngine/issues/50)
- 関連: [ADR-0005](0005-minimal-renderer-presentation.md)、[ADR-0006](0006-framegraph-and-fixed-mesh.md)

## 決定

Legacy `develop` の RHI が持つ Backend、Device、Command、Queue、Buffer、Texture、View、Pipeline、FrameGraph の境界を現行 `cue` 名前空間へ移す。Windows 実装の所有者、フォルダ、File、Target の独自名は `DX12` に統一する。Windows SDK の `D3D12_*`、`ID3D12*` など正式 API 名は変更しない。

生成と失敗の契約は現行の `Result<T>`、`static create`、`[[nodiscard]]` を維持する。Device の Adapter 選択、Debug Layer、DRED、WARP への代替処理は現行の `DX12RenderDevice::create` を維持する。Swap Chain の設定も現行実装を維持し、Legacy にあった Monitor 設定からの Refresh Rate 取得は導入しない。

`WindowsHost` は RHI の `BackendFactory` から `IBackend` を受け取る。Factory が具体的な `DX12Backend::create` を呼ぶ。Backend は Device、Queue、Descriptor、Resource、各 Manager、Swap Chain、FrameGraph 実行用 Pool の寿命順を管理する。FrameGraph の論理 Resource と物理 DX12 Resource の対応は Handle の Graph ID と Index を検証して作る。

この ADR は Legacy の全機能が移植済みであることを表さない。実装範囲と残作業は [移植対応表](../Research/M04-Legacy-RHI-Port-Checklist.md)で追跡する。ADR-0005／0006 の旧クラス名と構成説明は当時の決定の記録として残す。
