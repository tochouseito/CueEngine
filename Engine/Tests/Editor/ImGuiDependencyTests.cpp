#include <memory>
#include <string_view>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#ifndef IMGUI_HAS_DOCK
#error Dear ImGui の Docking 版が必要
#endif

/// @brief Docking／Demo の CPU Frame と Win32／DX12 Backend の Link を GPU 初期化前に検証する
int main()
{
    IMGUI_CHECKVERSION();
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                            &ImGui::DestroyContext);
    if (!context || std::string_view(ImGui::GetVersion()) != "1.92.9b")
    {
        return 1;
    }

    // 関数の実体を Link し、公式 Backend が選択されていることを確認する
    bool (*volatile initializeWin32)(void *) = &ImGui_ImplWin32_Init;
    bool (*volatile initializeDx12)(ImGui_ImplDX12_InitInfo *) = &ImGui_ImplDX12_Init;
    if (initializeWin32 == nullptr || initializeDx12 == nullptr)
    {
        return 2;
    }

    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    if (!io.Fonts->Build())
    {
        return 3;
    }

    // 初回は Window の Layout を確定し、次の Frame で描画 Data を確認する
    for (int frameIndex = 0; frameIndex < 2; ++frameIndex)
    {
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport();
        ImGui::ShowDemoWindow();
        ImGui::Render();
    }
    const auto *drawData = ImGui::GetDrawData();
    if (drawData == nullptr || !drawData->Valid || drawData->TotalVtxCount == 0 || drawData->TotalIdxCount == 0)
    {
        return 4;
    }
    return 0;
}
