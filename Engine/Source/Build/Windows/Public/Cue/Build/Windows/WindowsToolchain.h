#pragma once

#include <Cue/Build/Toolchain.h>

namespace cue
{
class AssertContext;

/// @brief このCueEngine Binaryが対応するWindows Toolchain要件を返す
[[nodiscard]] BuildEnvironmentRequirements current_windows_build_requirements(
    const AssertContext &a_assertContext) noexcept;

/// @brief Current DirectoryやPATHに依存せずEngine Build MetadataからWindows Tool候補を検出する
///
/// Tool executableは起動せず、存在、File Version、x64 Binary種別を検査する。Windows SDKはRegistryのInstalled Rootsと
/// Engineが記録したVersionのHeader／x64 Libraryを検査する。Project Fileは変更しない。
[[nodiscard]] BuildEnvironmentInventory discover_current_windows_build_environment(
    const AssertContext &a_assertContext) noexcept;

/// @brief 現在のWindows HostをEngine Build要件と照合した実行前Reportを返す
[[nodiscard]] BuildEnvironmentReport validate_current_windows_build_environment(
    const AssertContext &a_assertContext) noexcept;
} // namespace cue
