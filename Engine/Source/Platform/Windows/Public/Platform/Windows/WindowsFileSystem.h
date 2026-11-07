#pragma once

#include <Platform/FileSystem.h>

namespace cue
{
/// @brief Native Handle / UTF-16 を内部に閉じ込める Windows FileSystem を一意所有で返す
///
/// FileSystem の独立した呼出しは複数 Thread で同時に行える。作業 Directory を並行変更しない
/// 一つの IFile は外部で直列化する。Owner は全借用者・File の停止後に破棄する
[[nodiscard]] Result<std::unique_ptr<IFileSystem>> create_windows_file_system();
} // namespace cue
