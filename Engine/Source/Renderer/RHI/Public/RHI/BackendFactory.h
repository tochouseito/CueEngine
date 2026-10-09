#pragma once

#include <memory>

#include <Foundation/Result.h>
#include <RHI/Backend.h>

namespace cue
{
class IFileSystem;
/// @brief 現在の Platform で使用する Backend を生成して所有権を渡す
///
/// 生成と破棄は呼出側の同一 Thread で行う。失敗時は部分生成した GPU 資源を公開しない
/// FileSystem 指定時は Backend の停止まで借用する。未指定なら実装内で一意所有する
[[nodiscard]] Result<std::unique_ptr<IBackend>> create_backend(IFileSystem *a_files = nullptr);
} // namespace cue
