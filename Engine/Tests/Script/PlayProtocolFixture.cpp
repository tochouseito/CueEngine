#include <cstdint>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Shell API は Win32 の型宣言後に Include する
#include <shellapi.h>

namespace
{
/// @brief Protocol の既知 Header を作る Test 用 Little Endian Writer
void append(std::vector<std::uint8_t> &a_bytes, std::uint64_t a_value, unsigned a_count)
{
    for (unsigned index = 0; index < a_count; ++index)
        a_bytes.push_back(static_cast<std::uint8_t>(a_value >> (index * 8)));
}

/// @brief 正常な Metadata / Ready の後に不正応答を混ぜ、同時終了時の検証を試す
void message(std::vector<std::uint8_t> &a_bytes, std::uint32_t a_kind, std::uint32_t a_length, std::uint64_t a_session,
             std::uint64_t a_request = 0, bool a_isValid = true)
{
    append(a_bytes, a_isValid ? 0x4355504c : 0, 4);
    append(a_bytes, 1, 4);
    append(a_bytes, a_kind, 4);
    append(a_bytes, a_length, 4);
    append(a_bytes, a_session, 8);
    append(a_bytes, a_request, 8);
}

/// @brief Test の既知 ASCII 値を長さ付きの文字列へする
void text(std::vector<std::uint8_t> &a_bytes, const std::string &a_value)
{
    append(a_bytes, a_value.size(), 4);
    a_bytes.insert(a_bytes.end(), a_value.begin(), a_value.end());
}
} // namespace

/// @brief 不正な子応答を返して Code 0 で終了する。Game DLL は一切読み込まない
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    auto **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return 1;
    std::wstring pipeName;
    std::wstring module;
    std::uint64_t session = 0;
    for (int index = 1; index + 1 < count; ++index)
    {
        const std::wstring key = arguments[index];
        if (key == L"--pipe")
            pipeName = arguments[++index];
        else if (key == L"--module")
            module = arguments[++index];
        else if (key == L"--session")
            session = std::stoull(arguments[++index]);
    }
    LocalFree(arguments);
    if (pipeName.empty() || !session)
        return 1;
    const auto pipe =
        CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    std::vector<std::uint8_t> bytes;
    if (module.find(L"large-metadata") != std::wstring::npos)
    {
        std::vector<std::uint8_t> payload;
        append(payload, 256, 4);
        for (unsigned type = 0; type < 256; ++type)
        {
            text(payload, "fixture.Type" + std::to_string(type));
            text(payload, std::string(200, 'T'));
            append(payload, 1, 4);
            append(payload, 32, 4);
            for (unsigned property = 0; property < 32; ++property)
            {
                text(payload, "property" + std::to_string(property));
                append(payload, 1, 4);
                append(payload, 0, 8);
                append(payload, 0, 4);
                text(payload, std::string(240, 'P'));
            }
        }
        message(bytes, 101, static_cast<std::uint32_t>(payload.size()), session);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    else
    {
        message(bytes, 101, 4, session);
        append(bytes, 0, 4);
        message(bytes, 102, 0, session);
        if (module.find(L"bad-ack") != std::wstring::npos)
        {
            std::vector<std::uint8_t> payload;
            append(payload, 1, 4);
            text(payload, "9a00d731-7759-424c-9ade-fd086d624551");
            text(payload, "6c78eb99-aee8-4602-9fba-66fa96080f99");
            text(payload, "speed");
            append(payload, 1, 4);
            append(payload, 0, 8);
            append(payload, 0, 4);
            text(payload, "");
            message(bytes, 103, static_cast<std::uint32_t>(payload.size()), session, 999);
            bytes.insert(bytes.end(), payload.begin(), payload.end());
        }
        else if (module.find(L"bad-truncated") != std::wstring::npos)
        {
            message(bytes, 104, 100, session);
            append(bytes, 4, 4);
        }
        // Debug 開始許可前の Types 自体を検査する Case では、不正 Header で先に失敗させない
        else if (module.find(L"premature-debug-types") == std::wstring::npos)
            message(bytes, 102, 0, session, 0, false);
    }
    DWORD written = 0;
    const bool sent = WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                      written == bytes.size();
    // 親の poll より先に Process を終了し、Protocol 異常を exitCode 0 で隠せないことを確認する
    CloseHandle(pipe);
    return sent ? 0 : 1;
}
