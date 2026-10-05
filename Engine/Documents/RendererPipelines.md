# Pass の Pipeline 設定と FrameGraph Build

関連 Issue: #71

## 生成と利用

`DX12Backend` が `DX12PipelineManager` を所有し、`DX12ResourceContext` の Getter で貸し出す。
抽象層では `IPipelineManager` を非所有で束ねた `FrameGraphBuildContext` を Builder に渡す。
Manager は Graph より長く生存させ、生成、Build、記録と破棄を直列化する。

`FrameGraph::build()` が各 Pass の `setup()` を呼ぶ。
Pass は Builder の `create_root_signature`、`create_shader_blob`、`create_graphics_pipeline` または
`create_compute_pipeline` を使って構成を指定する。
Builder が Manager に生成を依頼し、成功した Handle を Graph の回収対象として記録する。
この生成 API は Graph の Build 中だけ利用できる。

Pass は非所有の `PipelineStateHandle` を保持し、`execute()` で `set_graphics_pipeline` または
`set_compute_pipeline` を呼ぶ。対応する Root Signature は PSO と一緒に設定する。
Graphics の記録は Pipeline、RenderTarget、Texture Binding、Viewport／Scissor、`draw_instanced` の順に行う。
`PresentToSwapChainPass.h` が具体例であり、DirectX の型や API を含まない。
全画面三角形の HLSL は `Engine/Shader/Hlsl/FullscreenTriangle.hlsl` に置く。

## 所有権と回収

Root、Shader Blob と PSO の実体は Manager の Registry が所有する。
Handle は種類、所属 Manager と Slot の世代で検証する。
別 Manager、古い世代、異なる Shader Stage の組合せは Result で失敗する。

Build 失敗時と Graph 破棄時に、Builder は PSO、Shader Blob、Root の順で Handle を返却する。
Build 失敗後の Graph は破棄して再生成する。Graph が回収する Handle を外部から retire しない。
Root は依存 PSO と共有するため、Root Handle の失効だけでは既存 PSO を無効化しない。
Shader Blob は PSO の生成入力であり、生成済み PSO の GPU 実行には必要ない。

Native Pipeline と Root の共有参照を Command Context が記録開始から保持する。
GPU 提出後は対応 Fence の完了を確認して Command List を Reset した後、または安全な停止後に返却する。
Graph の Handle が失効しても記録・提出済みの Command の参照は維持する。
Host は Render 処理と Graph を停止し、SwapChain、Pool、PipelineManager、ViewManager、Device の順で破棄する。

## DXC と HLSL の配置

CMake は開発環境の Windows SDK にある DXC を選び、SDK の Header を使って Runtime Compiler を構築する。
SDK が提供する `dxcompiler.dll` と `dxil.dll` を実行 File の隣へ配置し、HLSL は
`EngineResources/Shader` に配置する。DLL と Header の出自はインストール済みの Windows SDK であり、
配布時はその SDK の License と再配布条件に従う。SDK の Binary は Repository に登録しない。
SDK Version は Configure の出力、選択された DXC Directory は CMake の生成 Header で確認できる。
今回の検証環境は Windows SDK 10.0.26100.0、DXC DLL Version 1.8.2502.11 を使用した。

配置 Target は Build ごとに差分を Copy し、HLSL だけを編集した場合も更新する。
Compiler は実行 File 基準で読み込み、配置がない開発環境では Configure 時の SDK／Source Path を使う。
相対 Shader Path はこの Shader Directory 基準、絶対 Path はそのまま解決する。
Shader の更新反映には Graph の再生成が必要であり、実行中の自動再コンパイルは行わない。

Shader Model は既定で 6.0、Stage は Vertex、Pixel、Compute を扱う。
Debug は `-Zi -Qembed_debug -Od`、Development／Release は `-O3` を指定する。
File 不在やコンパイル失敗は DXC の診断とともに Result へ返し、失敗した Blob を登録しない。

## 現在の対応範囲

Graphics PSO は非 MSAA の単一 Color Target を扱い、入力 Vertex Layout と Depth を使わない。
Topology、Cull、Wireframe、Alpha Blend を設定できる。MRT、Depth、Vertex／Index Buffer の契約は今後追加する。
Root Signature 生成は SRV／CBV／UAV Table、Root Descriptor、Constants と Static Sampler を扱う。
抽象 Context の Binding は単一 Texture の Graphics SRV Table に対応し、未設定や未対応の Root Binding で Draw しない。
Compute PSO の生成と設定にも対応するが、現行の抽象 `dispatch` は Resource Binding のない Root を対象とする。
CBV、UAV、Constants と Compute Resource Binding の抽象記録 API は今後追加する。
