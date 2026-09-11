#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/RuntimeHost/RuntimeHostStartup.h>

namespace cue
{
class AssertContext;
}

namespace cue::runtime_host
{
/// @brief DynamicまたはStatic Providerから一回分のRuntime Host Startupを構築するFunction境界
using RuntimeHostStartupFactory = Result<RuntimeHostStartup> (*)(const AssertContext &) noexcept;

/// @brief 共通Runtime Host ProcessへExecutable固有のGame Module取得方針を渡す
struct RuntimeHostProcessDescriptor final
{
    RuntimeHostStartupFactory startupFactory = nullptr;
    bool startGameModuleByDefault = false;
};

/// @brief Command Line、Window、描画、Runtime main loopをProvider方式に依存せず実行する
///
/// startupFactoryはProcess Owner Threadで呼ばれ、返却Startupの移動、Callback呼出、最終破棄も同じThreadで行う。
/// Dynamic HostはstartGameModuleByDefaultをfalse、Shipping Productはtrueとして薄いExecutableから呼び出す。
/// @pre a_descriptor.startupFactoryはnullptrでないこと
[[nodiscard]] int run_runtime_host_process(
    int a_argumentCount, wchar_t **a_arguments,
    const RuntimeHostProcessDescriptor &a_descriptor) noexcept;
} // namespace cue::runtime_host
