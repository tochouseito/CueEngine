#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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
    append(a_bytes, 2, 4);
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

/// @brief Test 用の Float 値を Protocol の固定幅へ変換する
void real(std::vector<std::uint8_t> &a_bytes, float a_value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &a_value, sizeof(bits));
    append(a_bytes, bits, 4);
}

/// @brief Test の Header を境界確認済み Buffer から読む
std::uint64_t integer(const std::vector<std::uint8_t> &a_bytes, std::size_t a_offset, unsigned a_count)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < a_count; ++index)
        value |= static_cast<std::uint64_t>(a_bytes[a_offset + index]) << (index * 8);
    return value;
}

/// @brief 親の取得要求を期限付きで受け、その Request ID を不正 Payload の応答に使う
std::uint64_t wait_view(HANDLE a_pipe)
{
    const auto deadline = GetTickCount64() + 5000;
    std::vector<std::uint8_t> bytes;
    while (GetTickCount64() < deadline)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(a_pipe, nullptr, 0, nullptr, &available, nullptr))
            return 0;
        if (available)
        {
            const auto previous = bytes.size();
            if (previous + available > 5 * 1024 * 1024)
                return 0;
            bytes.resize(previous + available);
            DWORD received = 0;
            if (!ReadFile(a_pipe, bytes.data() + previous, available, &received, nullptr))
                return 0;
            bytes.resize(previous + received);
            while (bytes.size() >= 32)
            {
                const auto length = integer(bytes, 12, 4);
                if (bytes.size() - 32 < length)
                    break;
                if (integer(bytes, 8, 4) == 5)
                    return integer(bytes, 24, 8);
                bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(32 + length));
            }
        }
        Sleep(1);
    }
    return 0;
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
        if (module.find(L"bad-view-") != std::wstring::npos)
        {
            DWORD written = 0;
            if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
                written != bytes.size())
            {
                CloseHandle(pipe);
                return 1;
            }
            bytes.clear();
            const auto request = wait_view(pipe);
            if (!request)
            {
                CloseHandle(pipe);
                return 1;
            }
            std::vector<std::uint8_t> payload;
            append(payload, 1, 8);
            append(payload, 1, 4);
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned column = 0; column < 4; ++column)
                    real(payload, row == column ? 1.0f : 0.0f);
            real(payload, module.find(L"nan") != std::wstring::npos ? std::numeric_limits<float>::quiet_NaN() : 1.0f);
            real(payload, 0.1f);
            real(payload, 1000.0f);
            append(payload, module.find(L"count") != std::wstring::npos ? 4097 : 0, 4);
            append(payload, 0, 4);
            message(bytes, 106, static_cast<std::uint32_t>(payload.size()), session,
                    module.find(L"request") != std::wstring::npos ? request + 1 : request);
            bytes.insert(bytes.end(), payload.begin(), payload.end());
        }
        else if (module.find(L"bad-transform-ack") != std::wstring::npos)
        {
            std::vector<std::uint8_t> payload;
            append(payload, 1, 4);
            text(payload, "9a00d731-7759-424c-9ade-fd086d624551");
            for (unsigned index = 0; index < 10; ++index)
                real(payload, index >= 6 ? 1.0f : 0.0f);
            text(payload, "");
            message(bytes, 107, static_cast<std::uint32_t>(payload.size()), session, 999);
            bytes.insert(bytes.end(), payload.begin(), payload.end());
        }
        else if (module.find(L"bad-ack") != std::wstring::npos)
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
