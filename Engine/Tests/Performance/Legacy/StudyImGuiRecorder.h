#pragma once

// #101 調査専用。第三者 Backend の private ABI を借用せず、所有 DrawData だけを読む
// 描画規約の参照元は Dear ImGui imgui_impl_dx12.cpp (MIT, Copyright (c) 2014-2026 Omar Cornut)
// 第三者 Source と License は ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src 以下に保持する
/*
The MIT License (MIT)
Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <wrl/client.h>

#include <Foundation/Result.h>

namespace cue
{
/// @brief Render 専用の Native Pipeline と Upload Ring を所有する調査用 Recorder
/// ImGui API と共有 Context は触らない。Texture Heap と Fence は Adapter が保持する
class StudyImGuiRecorder final
{
    struct Ring final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
        Microsoft::WRL::ComPtr<ID3D12Resource> indices;
        std::uint32_t vertexCapacity = 0;
        std::uint32_t indexCapacity = 0;
    };

  public:
    /// @brief 起動時に Shader と Linear / Nearest Pipeline を生成する
    [[nodiscard]] Result<void> initialize(ID3D12Device &a_device)
    {
        m_device = &a_device;
        std::array<wchar_t, 32768> module{};
        const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size())
            return Result<void>::failure({ErrorCategory::PlatformFailure, "StudyImGui.module"});
        auto path = std::filesystem::path(module.data()).parent_path() / L"EngineResources/Shader/Hlsl/StudyImGui.hlsl";
        std::error_code fileError;
        if (!std::filesystem::exists(path, fileError))
            path = std::filesystem::path(L"Engine/Shader/Hlsl/StudyImGui.hlsl");
        Microsoft::WRL::ComPtr<ID3DBlob> vertex, pixel;
        auto hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "vertex_main", "vs_5_0",
                                     0, 0, &vertex, nullptr);
        if (SUCCEEDED(hr))
            hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "pixel_main", "ps_5_0", 0,
                                    0, &pixel, nullptr);
        if (FAILED(hr))
            return Result<void>::failure({ErrorCategory::PlatformFailure, "StudyImGui.compile", hr});
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 16;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = static_cast<UINT>(parameters.size());
        root.pParameters = parameters.data();
        root.NumStaticSamplers = 1;
        root.pStaticSamplers = &sampler;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        const D3D12_INPUT_ELEMENT_DESC inputs[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(ImDrawVert, pos)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(ImDrawVert, uv)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, static_cast<UINT>(offsetof(ImDrawVert, col)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        pipeline.InputLayout = {inputs, 3};
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.SampleMask = UINT_MAX;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = true;
        pipeline.DepthStencilState.DepthEnable = false;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.DepthStencilState.StencilEnable = false;
        pipeline.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pipeline.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        pipeline.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
        auto &blend = pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = true;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (std::size_t index = 0; index < m_roots.size(); ++index)
        {
            sampler.Filter = index == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            Microsoft::WRL::ComPtr<ID3DBlob> serialized;
            hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
            if (SUCCEEDED(hr))
                hr = a_device.CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_roots[index]));
            if (SUCCEEDED(hr))
            {
                pipeline.pRootSignature = m_roots[index].Get();
                hr = a_device.CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipelines[index]));
            }
            if (SUCCEEDED(hr))
                hr = m_roots[index]->SetName(index == 0 ? L"Study ImGui Linear Root" : L"Study ImGui Nearest Root");
            if (SUCCEEDED(hr))
                hr = m_pipelines[index]->SetName(index == 0 ? L"Study ImGui Linear PSO" : L"Study ImGui Nearest PSO");
            if (FAILED(hr))
                return Result<void>::failure({ErrorCategory::PlatformFailure, "StudyImGui.pipeline", hr});
        }
        return Result<void>::success();
    }

    /// @brief GPU 完了確認済み Ring へ所有配列を Copy し、Context 非依存で描画する
    [[nodiscard]] Result<void> record(const ImDrawData &a_draw, ID3D12GraphicsCommandList &a_list, std::size_t a_ring,
                                      ImDrawCallback a_reset, ImDrawCallback a_linear, ImDrawCallback a_nearest)
    {
        if (a_draw.DisplaySize.x <= 0 || a_draw.DisplaySize.y <= 0)
            return Result<void>::success();
        auto &ring = m_rings.at(a_ring);
        auto allocated = ensure_buffer(ring.vertices, ring.vertexCapacity, a_draw.TotalVtxCount, sizeof(ImDrawVert),
                                       5000, L"Study ImGui Vertices");
        if (!allocated.has_value())
            return allocated;
        allocated = ensure_buffer(ring.indices, ring.indexCapacity, a_draw.TotalIdxCount, sizeof(ImDrawIdx), 10000,
                                  L"Study ImGui Indices");
        if (!allocated.has_value())
            return allocated;
        void *vertices = nullptr, *indices = nullptr;
        const D3D12_RANGE noRead{0, 0};
        auto hr = ring.vertices->Map(0, &noRead, &vertices);
        if (FAILED(hr))
            return Result<void>::failure({ErrorCategory::PlatformFailure, "StudyImGui.vertex.map", hr});
        hr = ring.indices->Map(0, &noRead, &indices);
        if (FAILED(hr))
        {
            ring.vertices->Unmap(0, &noRead);
            return Result<void>::failure({ErrorCategory::PlatformFailure, "StudyImGui.index.map", hr});
        }
        auto *vertexCursor = static_cast<std::byte *>(vertices);
        auto *indexCursor = static_cast<std::byte *>(indices);
        for (const auto *drawList : a_draw.CmdLists)
        {
            if (drawList->VtxBuffer.Size)
                std::memcpy(vertexCursor, drawList->VtxBuffer.Data, drawList->VtxBuffer.size_in_bytes());
            if (drawList->IdxBuffer.Size)
                std::memcpy(indexCursor, drawList->IdxBuffer.Data, drawList->IdxBuffer.size_in_bytes());
            vertexCursor += drawList->VtxBuffer.size_in_bytes();
            indexCursor += drawList->IdxBuffer.size_in_bytes();
        }
        ring.vertices->Unmap(0, nullptr);
        ring.indices->Unmap(0, nullptr);
        const float left = a_draw.DisplayPos.x, right = left + a_draw.DisplaySize.x;
        const float top = a_draw.DisplayPos.y, bottom = top + a_draw.DisplaySize.y;
        const float projection[16] = {2.0f / (right - left),
                                      0,
                                      0,
                                      0,
                                      0,
                                      2.0f / (top - bottom),
                                      0,
                                      0,
                                      0,
                                      0,
                                      0.5f,
                                      0,
                                      (right + left) / (left - right),
                                      (top + bottom) / (bottom - top),
                                      0.5f,
                                      1.0f};
        /// @brief Reset と Sampler 切替で Root Constants を再設定する
        const auto setup = [&](std::size_t a_sampler)
        {
            a_list.SetPipelineState(m_pipelines[a_sampler].Get());
            a_list.SetGraphicsRootSignature(m_roots[a_sampler].Get());
            a_list.SetGraphicsRoot32BitConstants(0, 16, projection, 0);
        };
        const D3D12_VIEWPORT viewport{
            0, 0, a_draw.DisplaySize.x * a_draw.FramebufferScale.x, a_draw.DisplaySize.y * a_draw.FramebufferScale.y,
            0, 1};
        const D3D12_VERTEX_BUFFER_VIEW vertexView{ring.vertices->GetGPUVirtualAddress(),
                                                  ring.vertexCapacity * static_cast<UINT>(sizeof(ImDrawVert)),
                                                  sizeof(ImDrawVert)};
        const D3D12_INDEX_BUFFER_VIEW indexView{ring.indices->GetGPUVirtualAddress(),
                                                ring.indexCapacity * static_cast<UINT>(sizeof(ImDrawIdx)),
                                                sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        a_list.RSSetViewports(1, &viewport);
        a_list.IASetVertexBuffers(0, 1, &vertexView);
        a_list.IASetIndexBuffer(&indexView);
        a_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float blend[4]{};
        a_list.OMSetBlendFactor(blend);
        setup(0);
        UINT indexOffset = 0;
        INT vertexOffset = 0;
        for (const auto *drawList : a_draw.CmdLists)
        {
            for (const auto &command : drawList->CmdBuffer)
            {
                // 初期化時に固定した標準 Callback を識別し、共有 Context と非所有 UserData は触らない
                if (command.UserCallback)
                {
                    if (command.UserCallback != ImDrawCallback_ResetRenderState && command.UserCallback != a_reset &&
                        command.UserCallback != a_linear && command.UserCallback != a_nearest)
                        return Result<void>::failure({ErrorCategory::InvalidArgument, "StudyImGui.callback"});
                    setup(command.UserCallback == a_nearest ? 1 : 0);
                    continue;
                }
                const D3D12_RECT scissor{
                    static_cast<LONG>((command.ClipRect.x - a_draw.DisplayPos.x) * a_draw.FramebufferScale.x),
                    static_cast<LONG>((command.ClipRect.y - a_draw.DisplayPos.y) * a_draw.FramebufferScale.y),
                    static_cast<LONG>((command.ClipRect.z - a_draw.DisplayPos.x) * a_draw.FramebufferScale.x),
                    static_cast<LONG>((command.ClipRect.w - a_draw.DisplayPos.y) * a_draw.FramebufferScale.y)};
                if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
                    continue;
                a_list.RSSetScissorRects(1, &scissor);
                a_list.SetGraphicsRootDescriptorTable(1, {command.GetTexID()});
                a_list.DrawIndexedInstanced(command.ElemCount, 1, indexOffset + command.IdxOffset,
                                            vertexOffset + command.VtxOffset, 0);
            }
            indexOffset += drawList->IdxBuffer.Size;
            vertexOffset += drawList->VtxBuffer.Size;
        }
        return Result<void>::success();
    }

  private:
    /// @brief 完了済み旧 Buffer だけを置換し、部分生成の失敗では既存 Owner を保つ
    [[nodiscard]] Result<void> ensure_buffer(Microsoft::WRL::ComPtr<ID3D12Resource> &a_buffer,
                                             std::uint32_t &a_capacity, int a_count, UINT a_stride, UINT a_spare,
                                             const wchar_t *a_name)
    {
        if (a_count < 0 || static_cast<std::uint64_t>(a_count) + a_spare > UINT_MAX / a_stride)
            return Result<void>::failure({ErrorCategory::InvalidArgument, "StudyImGui.buffer.size"});
        if (a_buffer && a_capacity >= static_cast<UINT>(a_count))
            return Result<void>::success();
        const UINT capacity = static_cast<UINT>(a_count) + a_spare;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width = static_cast<UINT64>(capacity) * a_stride;
        resource.Height = 1;
        resource.DepthOrArraySize = resource.MipLevels = 1;
        resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> created;
        auto hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&created));
        if (SUCCEEDED(hr))
            hr = created->SetName(a_name);
        if (FAILED(hr))
            return Result<void>::failure({ErrorCategory::PlatformFailure, "StudyImGui.buffer", hr});
        a_buffer = std::move(created);
        a_capacity = capacity;
        return Result<void>::success();
    }

    ID3D12Device *m_device = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> m_roots;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> m_pipelines;
    std::array<Ring, 2> m_rings;
};
} // namespace cue
