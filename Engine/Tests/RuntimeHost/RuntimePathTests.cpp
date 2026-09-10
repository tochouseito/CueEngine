#include "RuntimePath.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace
{
/// @brief UTF-8 PathがNative UTF-16とUTF-8 Round Tripの両方を保持するか検証する
[[nodiscard]] bool verify_path(std::string_view a_utf8Path, std::wstring_view a_expectedFileName)
{
    const std::filesystem::path path = cue::runtime_host::detail::filesystem_path_from_utf8(a_utf8Path);
    if (path.filename().native() != a_expectedFileName)
    {
        return false;
    }
    const std::u8string roundTrip = path.generic_u8string();
    std::string roundTripBytes;
    roundTripBytes.reserve(roundTrip.size());
    for (const char8_t byte : roundTrip)
    {
        roundTripBytes.push_back(static_cast<char>(byte));
    }
    return roundTripBytes == a_utf8Path;
}
} // namespace

/// @brief 非ASCII Runtime Dependency名をWindows Native Pathへ損失なく変換できることを検証する
int main()
{
    constexpr std::string_view japanesePath = "Runtime/\xe6\x97\xa5\xe6\x9c\xac.dll";
    constexpr std::string_view supplementaryPath = "Runtime/\xe4\xbe\x9d\xe5\xad\x98\xf0\x9f\x98\x80.dll";
    return verify_path(japanesePath, L"日本.dll") && verify_path(supplementaryPath, L"依存😀.dll") ? 0 : 1;
}
