#pragma once

#include <cstdint>
#include <string>

namespace cue
{
/// @brief 呼出側が失敗の種類を判定するための分類
enum class ErrorCategory
{
    InvalidArgument,
    InvalidState,
    WrongThread,
    PlatformFailure,
    Fatal,
};

/// @brief 失敗の分類、処理名、Native診断値を所有する
///
/// 文字列は値として所有し、Win32型を公開しない
/// 回復可能な失敗と致命的な失敗を表し、Allocation失敗などの例外境界は後続Issueで定める
struct Error final
{
    ErrorCategory category;
    std::string operation;
    std::int64_t nativeCode = 0;
};
} // namespace cue
