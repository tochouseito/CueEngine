#pragma once

#include <cstdint>
#include <memory>

#include <EditorIde/IdeService.h>

namespace cue
{
/// @brief 事前 Build した Bridge の配置と接続期限。Path は絶対値、Owner Thread で利用する
struct WindowsIdeConfig final
{
    Path bridgeExecutable;
    std::uint32_t timeoutMilliseconds = 120000;
};
/// @brief Windows Process / Job で C# Bridge を非同期実行する。生成だけでは IDE を操作しない
[[nodiscard]] Result<std::unique_ptr<IEditorIdeService>> create_windows_ide_service(WindowsIdeConfig a_config);
} // namespace cue
