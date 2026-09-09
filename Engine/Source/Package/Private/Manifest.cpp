#include <Cue/Package/Manifest.h>

#include "Sha256.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Package/Error.h>
#include <Cue/Scene/Identity.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t k_maximumJsonNodes = 4096U;
constexpr std::size_t k_hashBufferBytes = 64U * 1024U;

/// @brief Manifest処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_manifest_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Unexpected exception escaped Package Manifest processing");
    std::abort();
}

/// @brief Package Manifest固有Errorを構築する
[[nodiscard]] cue::Error manifest_error(const cue::AssertContext &a_assertContext, cue::package::PackageError a_error,
                                        std::string_view a_summary) noexcept
{
    return cue::package::make_package_error(a_assertContext, a_error, a_summary);
}

/// @brief UTF-8 Byte列がUnicode Scalar Valueの正規Encodingだけを含むか返す
[[nodiscard]] bool is_valid_utf8(std::string_view a_text) noexcept
{
    std::size_t offset = 0U;
    while (offset < a_text.size())
    {
        const auto first = static_cast<unsigned char>(a_text[offset]);
        if (first <= 0x7fU)
        {
            ++offset;
            continue;
        }
        std::size_t length = 0U;
        std::uint32_t value = 0U;
        std::uint32_t minimum = 0U;
        if (first >= 0xc2U && first <= 0xdfU)
        {
            length = 2U;
            value = first & 0x1fU;
            minimum = 0x80U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
            length = 3U;
            value = first & 0x0fU;
            minimum = 0x800U;
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
            length = 4U;
            value = first & 0x07U;
            minimum = 0x10000U;
        }
        else
        {
            return false;
        }
        if (offset + length > a_text.size())
        {
            return false;
        }
        for (std::size_t index = 1U; index < length; ++index)
        {
            const auto continuation = static_cast<unsigned char>(a_text[offset + index]);
            if ((continuation & 0xc0U) != 0x80U)
            {
                return false;
            }
            value = (value << 6U) | (continuation & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
        {
            return false;
        }
        offset += length;
    }
    return true;
}

/// @brief Unicode Scalar ValueをUTF-8へ追加する
[[nodiscard]] bool append_utf8(std::string &a_output, std::uint32_t a_value)
{
    if (a_value <= 0x7fU)
    {
        a_output.push_back(static_cast<char>(a_value));
    }
    else if (a_value <= 0x7ffU)
    {
        a_output.push_back(static_cast<char>(0xc0U | (a_value >> 6U)));
        a_output.push_back(static_cast<char>(0x80U | (a_value & 0x3fU)));
    }
    else if (a_value <= 0xffffU && !(a_value >= 0xd800U && a_value <= 0xdfffU))
    {
        a_output.push_back(static_cast<char>(0xe0U | (a_value >> 12U)));
        a_output.push_back(static_cast<char>(0x80U | ((a_value >> 6U) & 0x3fU)));
        a_output.push_back(static_cast<char>(0x80U | (a_value & 0x3fU)));
    }
    else if (a_value <= 0x10ffffU)
    {
        a_output.push_back(static_cast<char>(0xf0U | (a_value >> 18U)));
        a_output.push_back(static_cast<char>(0x80U | ((a_value >> 12U) & 0x3fU)));
        a_output.push_back(static_cast<char>(0x80U | ((a_value >> 6U) & 0x3fU)));
        a_output.push_back(static_cast<char>(0x80U | (a_value & 0x3fU)));
    }
    else
    {
        return false;
    }
    return a_output.size() <= cue::package::k_maximumPackageManifestStringBytes;
}

enum class JsonKind : std::uint8_t
{
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

struct JsonValue final
{
    JsonKind kind = JsonKind::Null;
    std::string text;
    std::vector<JsonValue> elements;
    std::vector<std::pair<std::string, JsonValue>> members;
};

/// @brief Package Manifest制約付きJSONを所有Treeへ解析する
class JsonParser final
{
  public:
    /// @brief Parser寿命中だけ入力Byte列を借用する
    explicit JsonParser(std::string_view a_input) noexcept : m_input(a_input)
    {
    }

    /// @brief JSON文書全体を解析し末尾Dataと重複Memberを拒否する
    [[nodiscard]] bool parse(JsonValue &a_output)
    {
        skip_whitespace();
        if (!parse_value(a_output, 1U))
        {
            return false;
        }
        skip_whitespace();
        return m_offset == m_input.size();
    }

  private:
    /// @brief JSON Value一つを型に応じて解析する
    [[nodiscard]] bool parse_value(JsonValue &a_output, std::size_t a_depth)
    {
        if (m_nodeCount >= k_maximumJsonNodes || m_offset >= m_input.size())
        {
            return false;
        }
        ++m_nodeCount;
        const char current = m_input[m_offset];
        if (current == '{')
        {
            return parse_object(a_output, a_depth);
        }
        if (current == '[')
        {
            return parse_array(a_output, a_depth);
        }
        if (current == '"')
        {
            a_output.kind = JsonKind::String;
            return parse_string(a_output.text);
        }
        if (current == '-' || (current >= '0' && current <= '9'))
        {
            a_output.kind = JsonKind::Number;
            return parse_number(a_output.text);
        }
        if (consume_literal("true"))
        {
            a_output.kind = JsonKind::Boolean;
            a_output.text = "true";
            return true;
        }
        if (consume_literal("false"))
        {
            a_output.kind = JsonKind::Boolean;
            a_output.text = "false";
            return true;
        }
        if (consume_literal("null"))
        {
            a_output.kind = JsonKind::Null;
            return true;
        }
        return false;
    }

    /// @brief JSON Objectを深さとMember上限内で解析する
    [[nodiscard]] bool parse_object(JsonValue &a_output, std::size_t a_depth)
    {
        if (a_depth > 16U || !consume('{'))
        {
            return false;
        }
        a_output.kind = JsonKind::Object;
        skip_whitespace();
        if (consume('}'))
        {
            return true;
        }
        for (;;)
        {
            if (a_output.members.size() >= cue::package::k_maximumPackageFileEntries * 4U)
            {
                return false;
            }
            std::string name;
            if (!parse_string(name))
            {
                return false;
            }
            /// @brief 解析済みMember名との重複を検出する
            const bool duplicate =
                std::any_of(a_output.members.begin(), a_output.members.end(),
                            [&name](const auto &a_member) noexcept { return a_member.first == name; });
            if (duplicate)
            {
                return false;
            }
            skip_whitespace();
            if (!consume(':'))
            {
                return false;
            }
            skip_whitespace();
            JsonValue value;
            if (!parse_value(value, a_depth + 1U))
            {
                return false;
            }
            a_output.members.emplace_back(std::move(name), std::move(value));
            skip_whitespace();
            if (consume('}'))
            {
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skip_whitespace();
        }
    }

    /// @brief JSON Arrayを深さと要素上限内で解析する
    [[nodiscard]] bool parse_array(JsonValue &a_output, std::size_t a_depth)
    {
        if (a_depth > 16U || !consume('['))
        {
            return false;
        }
        a_output.kind = JsonKind::Array;
        skip_whitespace();
        if (consume(']'))
        {
            return true;
        }
        for (;;)
        {
            if (a_output.elements.size() >= cue::package::k_maximumPackageFileEntries)
            {
                return false;
            }
            JsonValue value;
            if (!parse_value(value, a_depth + 1U))
            {
                return false;
            }
            a_output.elements.push_back(std::move(value));
            skip_whitespace();
            if (consume(']'))
            {
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skip_whitespace();
        }
    }

    /// @brief JSON StringをEscapeとUnicode Surrogate検証付きで復号する
    [[nodiscard]] bool parse_string(std::string &a_output)
    {
        if (!consume('"'))
        {
            return false;
        }
        while (m_offset < m_input.size())
        {
            const unsigned char current = static_cast<unsigned char>(m_input[m_offset++]);
            if (current == '"')
            {
                return a_output.size() <= cue::package::k_maximumPackageManifestStringBytes && is_valid_utf8(a_output);
            }
            if (current < 0x20U)
            {
                return false;
            }
            if (current != '\\')
            {
                a_output.push_back(static_cast<char>(current));
            }
            else
            {
                if (m_offset >= m_input.size())
                {
                    return false;
                }
                const char escape = m_input[m_offset++];
                switch (escape)
                {
                case '"':
                case '\\':
                case '/':
                    a_output.push_back(escape);
                    break;
                case 'b':
                    a_output.push_back('\b');
                    break;
                case 'f':
                    a_output.push_back('\f');
                    break;
                case 'n':
                    a_output.push_back('\n');
                    break;
                case 'r':
                    a_output.push_back('\r');
                    break;
                case 't':
                    a_output.push_back('\t');
                    break;
                case 'u':
                    if (!parse_unicode_escape(a_output))
                    {
                        return false;
                    }
                    break;
                default:
                    return false;
                }
            }
            if (a_output.size() > cue::package::k_maximumPackageManifestStringBytes)
            {
                return false;
            }
        }
        return false;
    }

    /// @brief 一つまたはSurrogate PairのUnicode EscapeをUTF-8へ追加する
    [[nodiscard]] bool parse_unicode_escape(std::string &a_output)
    {
        std::uint32_t first = 0U;
        if (!parse_hex_quad(first))
        {
            return false;
        }
        if (first >= 0xdc00U && first <= 0xdfffU)
        {
            return false;
        }
        if (first >= 0xd800U && first <= 0xdbffU)
        {
            if (m_offset + 2U > m_input.size() || m_input[m_offset] != '\\' || m_input[m_offset + 1U] != 'u')
            {
                return false;
            }
            m_offset += 2U;
            std::uint32_t second = 0U;
            if (!parse_hex_quad(second) || second < 0xdc00U || second > 0xdfffU)
            {
                return false;
            }
            first = 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
        }
        return append_utf8(a_output, first);
    }

    /// @brief 4桁のhexadecimalをUnicode Code Unitへ変換する
    [[nodiscard]] bool parse_hex_quad(std::uint32_t &a_output) noexcept
    {
        if (m_offset + 4U > m_input.size())
        {
            return false;
        }
        a_output = 0U;
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            const char value = m_input[m_offset++];
            std::uint32_t digit = 0U;
            if (value >= '0' && value <= '9')
            {
                digit = static_cast<std::uint32_t>(value - '0');
            }
            else if (value >= 'a' && value <= 'f')
            {
                digit = 10U + static_cast<std::uint32_t>(value - 'a');
            }
            else if (value >= 'A' && value <= 'F')
            {
                digit = 10U + static_cast<std::uint32_t>(value - 'A');
            }
            else
            {
                return false;
            }
            a_output = (a_output << 4U) | digit;
        }
        return true;
    }

    /// @brief JSON Number文法を検証し元Byte列を保持する
    [[nodiscard]] bool parse_number(std::string &a_output)
    {
        const std::size_t begin = m_offset;
        if (m_input[m_offset] == '-')
        {
            ++m_offset;
        }
        if (m_offset >= m_input.size())
        {
            return false;
        }
        if (m_input[m_offset] == '0')
        {
            ++m_offset;
            if (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                return false;
            }
        }
        else if (m_input[m_offset] >= '1' && m_input[m_offset] <= '9')
        {
            while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                ++m_offset;
            }
        }
        else
        {
            return false;
        }
        if (m_offset < m_input.size() && m_input[m_offset] == '.')
        {
            ++m_offset;
            const std::size_t fraction = m_offset;
            while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                ++m_offset;
            }
            if (fraction == m_offset)
            {
                return false;
            }
        }
        if (m_offset < m_input.size() && (m_input[m_offset] == 'e' || m_input[m_offset] == 'E'))
        {
            ++m_offset;
            if (m_offset < m_input.size() && (m_input[m_offset] == '+' || m_input[m_offset] == '-'))
            {
                ++m_offset;
            }
            const std::size_t exponent = m_offset;
            while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                ++m_offset;
            }
            if (exponent == m_offset)
            {
                return false;
            }
        }
        a_output.assign(m_input.substr(begin, m_offset - begin));
        return true;
    }

    /// @brief 現在位置の一文字が一致する場合だけ消費する
    [[nodiscard]] bool consume(char a_value) noexcept
    {
        if (m_offset >= m_input.size() || m_input[m_offset] != a_value)
        {
            return false;
        }
        ++m_offset;
        return true;
    }

    /// @brief 現在位置の固定Literalが一致する場合だけ消費する
    [[nodiscard]] bool consume_literal(std::string_view a_value) noexcept
    {
        if (m_input.substr(m_offset, a_value.size()) != a_value)
        {
            return false;
        }
        m_offset += a_value.size();
        return true;
    }

    /// @brief JSONで許可されるASCII空白を読み飛ばす
    void skip_whitespace() noexcept
    {
        while (m_offset < m_input.size() && (m_input[m_offset] == ' ' || m_input[m_offset] == '\t' ||
                                             m_input[m_offset] == '\r' || m_input[m_offset] == '\n'))
        {
            ++m_offset;
        }
    }

    std::string_view m_input;
    std::size_t m_offset = 0U;
    std::size_t m_nodeCount = 0U;
};

/// @brief JSON Objectから名前一致Memberを返す
[[nodiscard]] const JsonValue *find_member(const JsonValue &a_object, std::string_view a_name) noexcept
{
    /// @brief Member名が要求名と一致するか比較する
    const auto found = std::find_if(a_object.members.begin(), a_object.members.end(),
                                    [a_name](const auto &a_member) noexcept { return a_member.first == a_name; });
    return found == a_object.members.end() ? nullptr : &found->second;
}

/// @brief JSON Objectが指定Memberだけを一つずつ持つか返す
[[nodiscard]] bool has_exact_members(const JsonValue &a_object, std::span<const std::string_view> a_names) noexcept
{
    if (a_object.kind != JsonKind::Object || a_object.members.size() != a_names.size())
    {
        return false;
    }
    /// @brief 必須MemberがObject内に存在するか判定する
    return std::all_of(a_names.begin(), a_names.end(), [&a_object](std::string_view a_name) noexcept
                       { return find_member(a_object, a_name) != nullptr; });
}

/// @brief JSON Numberを符号なし整数として完全変換する
template <typename Value> [[nodiscard]] bool parse_unsigned(const JsonValue &a_value, Value &a_output) noexcept
{
    if (a_value.kind != JsonKind::Number || a_value.text.empty() || a_value.text.front() == '-' ||
        a_value.text.find_first_of(".eE") != std::string::npos)
    {
        return false;
    }
    const auto converted = std::from_chars(a_value.text.data(), a_value.text.data() + a_value.text.size(), a_output);
    return converted.ec == std::errc{} && converted.ptr == a_value.text.data() + a_value.text.size();
}

/// @brief ASCII英字だけをlowercaseへ変換して比較Keyを返す
[[nodiscard]] std::string ascii_case_key(std::string_view a_value)
{
    std::string result(a_value);
    for (char &value : result)
    {
        if (value >= 'A' && value <= 'Z')
        {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return result;
}

/// @brief Windows予約Device名に一致するPath Segmentか返す
[[nodiscard]] bool is_reserved_device_segment(std::string_view a_segment)
{
    const std::size_t extension = a_segment.find('.');
    const std::string base = ascii_case_key(a_segment.substr(0U, extension));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul")
    {
        return true;
    }
    return base.size() == 4U &&
           ((base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9');
}

/// @brief Manifestへ保存可能な正規化済みPackage相対Pathか返す
[[nodiscard]] bool is_valid_package_path(std::string_view a_path) noexcept
{
    if (a_path.empty() || a_path.size() > cue::package::k_maximumPackageRelativePathBytes || a_path.front() == '/' ||
        a_path.back() == '/' || a_path.find('\\') != std::string::npos || !is_valid_utf8(a_path))
    {
        return false;
    }
    std::size_t segmentCount = 0U;
    std::size_t begin = 0U;
    while (begin < a_path.size())
    {
        const std::size_t end = a_path.find('/', begin);
        const std::string_view segment =
            a_path.substr(begin, end == std::string_view::npos ? a_path.size() - begin : end - begin);
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' || segment.back() == ' ' ||
            is_reserved_device_segment(segment))
        {
            return false;
        }
        for (const unsigned char value : segment)
        {
            if (value < 0x20U || value == 0x7fU || value == ':' || value == '"' || value == '<' || value == '>' ||
                value == '|' || value == '?' || value == '*')
            {
                return false;
            }
        }
        ++segmentCount;
        if (segmentCount > cue::package::k_maximumPackagePathSegments)
        {
            return false;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

/// @brief lowercase hexadecimal一文字か返す
[[nodiscard]] bool is_lower_hex_digit(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief lowercase 64桁SHA-256 hexadecimalか返す
[[nodiscard]] bool is_valid_sha256(std::string_view a_value) noexcept
{
    return a_value.size() == 64U && std::all_of(a_value.begin(), a_value.end(), is_lower_hex_digit);
}

/// @brief Build ConfigurationをManifestの固定文字列へ変換する
[[nodiscard]] std::string_view configuration_text(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return {};
}

/// @brief Manifest文字列をBuild Configurationへ変換する
[[nodiscard]] std::optional<cue::BuildConfiguration> parse_configuration(std::string_view a_value) noexcept
{
    if (a_value == "Debug")
    {
        return cue::BuildConfiguration::Debug;
    }
    if (a_value == "Development")
    {
        return cue::BuildConfiguration::Development;
    }
    if (a_value == "Release")
    {
        return cue::BuildConfiguration::Release;
    }
    return std::nullopt;
}

/// @brief Package File RoleをManifestの固定文字列へ変換する
[[nodiscard]] std::string_view role_text(cue::package::PackageFileRole a_role) noexcept
{
    switch (a_role)
    {
    case cue::package::PackageFileRole::RuntimeHost:
        return "runtimeHost";
    case cue::package::PackageFileRole::GameModule:
        return "gameModule";
    case cue::package::PackageFileRole::GameModuleMetadata:
        return "gameModuleMetadata";
    case cue::package::PackageFileRole::ProjectRuntimeData:
        return "projectRuntimeData";
    case cue::package::PackageFileRole::StartupSceneRuntimeData:
        return "startupSceneRuntimeData";
    case cue::package::PackageFileRole::RuntimeDependency:
        return "runtimeDependency";
    }
    return {};
}

/// @brief Manifest文字列をPackage File Roleへ変換する
[[nodiscard]] std::optional<cue::package::PackageFileRole> parse_role(std::string_view a_value) noexcept
{
    using cue::package::PackageFileRole;
    if (a_value == "runtimeHost")
    {
        return PackageFileRole::RuntimeHost;
    }
    if (a_value == "gameModule")
    {
        return PackageFileRole::GameModule;
    }
    if (a_value == "gameModuleMetadata")
    {
        return PackageFileRole::GameModuleMetadata;
    }
    if (a_value == "projectRuntimeData")
    {
        return PackageFileRole::ProjectRuntimeData;
    }
    if (a_value == "startupSceneRuntimeData")
    {
        return PackageFileRole::StartupSceneRuntimeData;
    }
    if (a_value == "runtimeDependency")
    {
        return PackageFileRole::RuntimeDependency;
    }
    return std::nullopt;
}

/// @brief Engine Versionをcanonical major.minor.patchへ追加する
void append_engine_version(std::string &a_output, const cue::EngineVersion &a_version)
{
    a_output.append(std::to_string(a_version.major));
    a_output.push_back('.');
    a_output.append(std::to_string(a_version.minor));
    a_output.push_back('.');
    a_output.append(std::to_string(a_version.patch));
}

/// @brief canonical major.minor.patch文字列をEngine Versionへ変換する
[[nodiscard]] bool parse_engine_version(std::string_view a_value, cue::EngineVersion &a_output) noexcept
{
    const std::size_t first = a_value.find('.');
    const std::size_t second = first == std::string_view::npos ? first : a_value.find('.', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        a_value.find('.', second + 1U) != std::string_view::npos)
    {
        return false;
    }
    const std::array parts = {a_value.substr(0U, first), a_value.substr(first + 1U, second - first - 1U),
                              a_value.substr(second + 1U)};
    std::array<std::uint32_t, 3U> values{};
    for (std::size_t index = 0U; index < parts.size(); ++index)
    {
        if (parts[index].empty() || (parts[index].size() > 1U && parts[index].front() == '0'))
        {
            return false;
        }
        const auto converted =
            std::from_chars(parts[index].data(), parts[index].data() + parts[index].size(), values[index]);
        if (converted.ec != std::errc{} || converted.ptr != parts[index].data() + parts[index].size())
        {
            return false;
        }
    }
    a_output = {values[0U], values[1U], values[2U]};
    return true;
}

/// @brief SceneAssetIdをcanonical lowercase UUID文字列へ変換する
[[nodiscard]] std::string scene_id_text(const cue::scene::SceneAssetId &a_id)
{
    const cue::scene::IdentityText text = a_id.canonical_text();
    return std::string(text.data(), text.size());
}

/// @brief Manifest Roleが要求する固定Path規則を満たすか返す
[[nodiscard]] bool role_matches_path(cue::package::PackageFileRole a_role, std::string_view a_path,
                                     std::string_view a_scenePath) noexcept
{
    using cue::package::PackageFileRole;
    switch (a_role)
    {
    case PackageFileRole::RuntimeHost:
        return a_path == "CueRuntimeHost.exe";
    case PackageFileRole::GameModule:
        return a_path == "Game/CueGameModule.dll";
    case PackageFileRole::GameModuleMetadata:
        return a_path == "Game/CueGameModule.metadata.json";
    case PackageFileRole::ProjectRuntimeData:
        return a_path == "Data/CueProject.runtime.json";
    case PackageFileRole::StartupSceneRuntimeData:
        return a_path == a_scenePath;
    case PackageFileRole::RuntimeDependency:
        return a_path.starts_with("Runtime/") && a_path.size() > std::string_view("Runtime/").size();
    }
    return false;
}

/// @brief Manifest内のPDB Entryを拡張子のASCII case-insensitive比較で検出する
[[nodiscard]] bool is_pdb_path(std::string_view a_path)
{
    const std::string key = ascii_case_key(a_path);
    return key.ends_with(".pdb");
}

/// @brief SHA-256 Digestをlowercase hexadecimalへ変換する
[[nodiscard]] std::string digest_text(const cue::package_private::Sha256Digest &a_digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(a_digest.size() * 2U);
    for (const std::uint8_t value : a_digest)
    {
        output.push_back(digits[(value >> 4U) & 0x0fU]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

/// @brief UTF-8 Path文字列をNative filesystem Pathへ変換する
[[nodiscard]] std::filesystem::path native_path(std::string_view a_path)
{
    std::u8string value;
    value.reserve(a_path.size());
    for (const char byte : a_path)
    {
        value.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(std::move(value));
}

/// @brief WindowsではExtended-length形式、それ以外では元のPathをNative検査へ渡す
[[nodiscard]] std::filesystem::path native_inspection_path(const std::filesystem::path &a_path)
{
#if defined(_WIN32)
    std::wstring native = a_path.native();
    std::replace(native.begin(), native.end(), L'/', L'\\');
    if (native.starts_with(L"\\\\?\\"))
    {
        return std::filesystem::path(std::move(native));
    }
    if (native.starts_with(L"\\\\"))
    {
        return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2U));
    }
    return std::filesystem::path(L"\\\\?\\" + native);
#else
    return a_path;
#endif
}

/// @brief Path自身がReparse PointまたはSymbolic LinkかNative属性で判定する
[[nodiscard]] bool is_indirect_path(const std::filesystem::path &a_path, std::error_code &a_error)
{
#if defined(_WIN32)
    const std::filesystem::path inspectionPath = native_inspection_path(a_path);
    const DWORD attributes = GetFileAttributesW(inspectionPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        a_error.assign(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    a_error.clear();
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    const std::filesystem::file_status status = std::filesystem::symlink_status(a_path, a_error);
    return !a_error && std::filesystem::is_symlink(status);
#endif
}
} // namespace

namespace cue::package
{
PackageFileEntry::PackageFileEntry(PackageFileRole a_role, std::string a_relativePath, std::uint64_t a_byteSize,
                                   std::string a_sha256) noexcept
    : m_role(a_role), m_relativePath(std::move(a_relativePath)), m_byteSize(a_byteSize), m_sha256(std::move(a_sha256))
{
}

Result<PackageFileEntry> PackageFileEntry::create(PackageFileRole a_role, std::string a_relativePath,
                                                  std::uint64_t a_byteSize, std::string a_sha256,
                                                  const AssertContext &a_assertContext) noexcept
{
    if (role_text(a_role).empty())
    {
        return Result<PackageFileEntry>::failure(
            manifest_error(a_assertContext, PackageError::InvalidPackageManifest, "Package file role is invalid"));
    }
    if (!is_valid_package_path(a_relativePath))
    {
        return Result<PackageFileEntry>::failure(
            manifest_error(a_assertContext, PackageError::InvalidPackagePath, "Package relative path is invalid"));
    }
    if (a_byteSize == 0U || a_byteSize > k_maximumPackagedFileBytes)
    {
        return Result<PackageFileEntry>::failure(
            manifest_error(a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                           "Package file size is outside the Manifest resource contract"));
    }
    if (!is_valid_sha256(a_sha256))
    {
        return Result<PackageFileEntry>::failure(
            manifest_error(a_assertContext, PackageError::InvalidPackageManifest, "Package file SHA-256 is invalid"));
    }
    try
    {
        return Result<PackageFileEntry>::success(
            PackageFileEntry(a_role, std::move(a_relativePath), a_byteSize, std::move(a_sha256)));
    }
    catch (...)
    {
        terminate_manifest_exception(a_assertContext);
    }
}

PackageFileRole PackageFileEntry::role() const noexcept
{
    return m_role;
}

std::string_view PackageFileEntry::relative_path() const noexcept
{
    return m_relativePath;
}

std::uint64_t PackageFileEntry::byte_size() const noexcept
{
    return m_byteSize;
}

std::string_view PackageFileEntry::sha256() const noexcept
{
    return m_sha256;
}

Result<std::vector<PackageFileEntry>> validate_runtime_dependency_inventory(
    BuildConfiguration a_expectedConfiguration, std::vector<RuntimeDependencyCandidate> a_candidates,
    const AssertContext &a_assertContext) noexcept
{
    if (configuration_text(a_expectedConfiguration).empty())
    {
        return Result<std::vector<PackageFileEntry>>::failure(manifest_error(
            a_assertContext, PackageError::InvalidPackageManifest, "Runtime Dependency configuration is invalid"));
    }
    if (a_candidates.size() > k_maximumPackageFileEntries)
    {
        return Result<std::vector<PackageFileEntry>>::failure(
            manifest_error(a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                           "Runtime Dependency inventory exceeds the entry limit"));
    }
    try
    {
        std::vector<std::string> caseKeys;
        caseKeys.reserve(a_candidates.size());
        std::vector<PackageFileEntry> files;
        files.reserve(a_candidates.size());
        std::uint64_t totalBytes = 0U;
        for (RuntimeDependencyCandidate &candidate : a_candidates)
        {
            const std::string caseKey = ascii_case_key(candidate.file.relative_path());
            if (candidate.sourceConfiguration != a_expectedConfiguration ||
                candidate.file.role() != PackageFileRole::RuntimeDependency ||
                !role_matches_path(candidate.file.role(), candidate.file.relative_path(), {}) ||
                is_pdb_path(candidate.file.relative_path()) ||
                std::find(caseKeys.begin(), caseKeys.end(), caseKey) != caseKeys.end())
            {
                return Result<std::vector<PackageFileEntry>>::failure(
                    manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                                   "Runtime Dependency inventory mixes configurations or contains an invalid entry"));
            }
            if (totalBytes > k_maximumPackageInventoryBytes - candidate.file.byte_size())
            {
                return Result<std::vector<PackageFileEntry>>::failure(
                    manifest_error(a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                                   "Runtime Dependency inventory exceeds the total byte limit"));
            }
            caseKeys.push_back(caseKey);
            totalBytes += candidate.file.byte_size();
            files.push_back(std::move(candidate.file));
        }
        std::sort(files.begin(), files.end(),
                  /// @brief Runtime DependencyをPackage相対Path順へ並べる
                  [](const PackageFileEntry &a_left, const PackageFileEntry &a_right) noexcept
                  { return a_left.relative_path() < a_right.relative_path(); });
        return Result<std::vector<PackageFileEntry>>::success(std::move(files));
    }
    catch (...)
    {
        terminate_manifest_exception(a_assertContext);
    }
}

PackageManifest::PackageManifest(std::string a_projectId, EngineVersion a_engineVersion,
                                 BuildConfiguration a_configuration, std::string a_startupSceneAssetId,
                                 std::string a_startupSceneRuntimeDataPath,
                                 std::vector<PackageFileEntry> a_files) noexcept
    : m_projectId(std::move(a_projectId)), m_engineVersion(a_engineVersion), m_configuration(a_configuration),
      m_startupSceneAssetId(std::move(a_startupSceneAssetId)),
      m_startupSceneRuntimeDataPath(std::move(a_startupSceneRuntimeDataPath)), m_files(std::move(a_files))
{
}

Result<PackageManifest> PackageManifest::create(std::string a_projectId, EngineVersion a_engineVersion,
                                                BuildConfiguration a_configuration, std::string a_startupSceneAssetId,
                                                std::string a_startupSceneRuntimeDataPath,
                                                std::vector<PackageFileEntry> a_files,
                                                const AssertContext &a_assertContext) noexcept
{
    auto projectId = ProjectId::parse(a_projectId, a_assertContext);
    auto sceneId = scene::SceneAssetId::parse(a_startupSceneAssetId, a_assertContext);
    if (!projectId || !sceneId || configuration_text(a_configuration).empty())
    {
        return Result<PackageManifest>::failure(
            manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                           "Package project, scene, or configuration identity is invalid"));
    }
    const std::string canonicalSceneId = scene_id_text(*sceneId.try_value());
    const std::string expectedScenePath = "Data/Scenes/" + canonicalSceneId + ".cueruntime.json";
    if (a_startupSceneRuntimeDataPath != expectedScenePath || a_files.size() < 5U ||
        a_files.size() > k_maximumPackageFileEntries)
    {
        return Result<PackageManifest>::failure(
            manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                           "Package startup scene path or required file count is invalid"));
    }
    try
    {
        std::sort(a_files.begin(), a_files.end(),
                  /// @brief Package File EntryをUTF-8相対Path順へ並べる
                  [](const PackageFileEntry &a_left, const PackageFileEntry &a_right) noexcept
                  { return a_left.relative_path() < a_right.relative_path(); });
        std::array<std::size_t, 6U> roleCounts{};
        std::uint64_t totalBytes = 0U;
        std::vector<std::string> caseKeys;
        caseKeys.reserve(a_files.size());
        for (std::size_t index = 0U; index < a_files.size(); ++index)
        {
            const PackageFileEntry &file = a_files[index];
            const std::size_t roleIndex = static_cast<std::size_t>(file.role()) - 1U;
            const std::string caseKey = ascii_case_key(file.relative_path());
            if (roleIndex >= roleCounts.size() || !is_valid_package_path(file.relative_path()) ||
                !is_valid_sha256(file.sha256()) || file.byte_size() == 0U ||
                file.byte_size() > k_maximumPackagedFileBytes ||
                !role_matches_path(file.role(), file.relative_path(), expectedScenePath) ||
                is_pdb_path(file.relative_path()) ||
                std::find(caseKeys.begin(), caseKeys.end(), caseKey) != caseKeys.end() ||
                totalBytes > k_maximumPackageInventoryBytes - file.byte_size())
            {
                return Result<PackageManifest>::failure(
                    manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                                   "Package file inventory contains an invalid, duplicate, or oversized entry"));
            }
            ++roleCounts[roleIndex];
            totalBytes += file.byte_size();
            caseKeys.push_back(caseKey);
        }
        for (std::size_t index = 0U; index < roleCounts.size() - 1U; ++index)
        {
            if (roleCounts[index] != 1U)
            {
                return Result<PackageManifest>::failure(
                    manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                                   "Package Manifest requires exactly one entry for every mandatory role"));
            }
        }
        return Result<PackageManifest>::success(
            PackageManifest(std::move(a_projectId), a_engineVersion, a_configuration, std::move(a_startupSceneAssetId),
                            std::move(a_startupSceneRuntimeDataPath), std::move(a_files)));
    }
    catch (...)
    {
        terminate_manifest_exception(a_assertContext);
    }
}

std::uint32_t PackageManifest::schema_version() const noexcept
{
    return k_packageManifestSchemaVersion;
}

std::string_view PackageManifest::project_id() const noexcept
{
    return m_projectId;
}

const EngineVersion &PackageManifest::engine_version() const noexcept
{
    return m_engineVersion;
}

BuildConfiguration PackageManifest::configuration() const noexcept
{
    return m_configuration;
}

std::string_view PackageManifest::startup_scene_asset_id() const noexcept
{
    return m_startupSceneAssetId;
}

std::string_view PackageManifest::startup_scene_runtime_data_path() const noexcept
{
    return m_startupSceneRuntimeDataPath;
}

std::span<const PackageFileEntry> PackageManifest::files() const noexcept
{
    return m_files;
}

Result<std::string> serialize_package_manifest(const PackageManifest &a_manifest,
                                               const AssertContext &a_assertContext) noexcept
{
    try
    {
        std::string output;
        output.reserve(1024U + a_manifest.files().size() * 192U);
        output.append("{\"schemaVersion\":1,\"projectId\":\"");
        output.append(a_manifest.project_id());
        output.append("\",\"engineVersion\":\"");
        append_engine_version(output, a_manifest.engine_version());
        output.append("\",\"configuration\":\"");
        output.append(configuration_text(a_manifest.configuration()));
        output.append("\",\"startupScene\":{\"sceneAssetId\":\"");
        output.append(a_manifest.startup_scene_asset_id());
        output.append("\",\"runtimeDataPath\":\"");
        output.append(a_manifest.startup_scene_runtime_data_path());
        output.append("\"},\"files\":[");
        for (std::size_t index = 0U; index < a_manifest.files().size(); ++index)
        {
            const PackageFileEntry &file = a_manifest.files()[index];
            if (index > 0U)
            {
                output.push_back(',');
            }
            output.append("{\"role\":\"");
            output.append(role_text(file.role()));
            output.append("\",\"path\":\"");
            output.append(file.relative_path());
            output.append("\",\"sizeBytes\":");
            output.append(std::to_string(file.byte_size()));
            output.append(",\"sha256\":\"");
            output.append(file.sha256());
            output.append("\"}");
        }
        output.append("]}\n");
        if (output.size() > k_maximumPackageManifestBytes)
        {
            return Result<std::string>::failure(manifest_error(a_assertContext,
                                                               PackageError::PackageManifestResourceLimitExceeded,
                                                               "Serialized Package Manifest exceeds its byte limit"));
        }
        return Result<std::string>::success(std::move(output));
    }
    catch (...)
    {
        terminate_manifest_exception(a_assertContext);
    }
}

Result<PackageManifest> parse_package_manifest(std::string_view a_json, const AssertContext &a_assertContext) noexcept
{
    if (a_json.empty() || a_json.size() > k_maximumPackageManifestBytes || !is_valid_utf8(a_json))
    {
        return Result<PackageManifest>::failure(
            manifest_error(a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                           "Package Manifest input exceeds its byte or UTF-8 limit"));
    }
    try
    {
        JsonValue root;
        JsonParser parser(a_json);
        constexpr std::array topNames = {std::string_view("schemaVersion"), std::string_view("projectId"),
                                         std::string_view("engineVersion"), std::string_view("configuration"),
                                         std::string_view("startupScene"),  std::string_view("files")};
        if (!parser.parse(root) || !has_exact_members(root, topNames))
        {
            return Result<PackageManifest>::failure(
                manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                               "Package Manifest JSON or top-level member set is invalid"));
        }
        std::uint32_t schemaVersion = 0U;
        const JsonValue *schema = find_member(root, "schemaVersion");
        if (schema == nullptr || !parse_unsigned(*schema, schemaVersion))
        {
            return Result<PackageManifest>::failure(manifest_error(
                a_assertContext, PackageError::InvalidPackageManifest, "Package Manifest schema version is invalid"));
        }
        if (schemaVersion != k_packageManifestSchemaVersion)
        {
            return Result<PackageManifest>::failure(manifest_error(a_assertContext,
                                                                   PackageError::UnsupportedPackageManifestVersion,
                                                                   "Package Manifest schema version is unsupported"));
        }
        const JsonValue *projectId = find_member(root, "projectId");
        const JsonValue *engineVersion = find_member(root, "engineVersion");
        const JsonValue *configuration = find_member(root, "configuration");
        const JsonValue *startupScene = find_member(root, "startupScene");
        const JsonValue *files = find_member(root, "files");
        constexpr std::array startupNames = {std::string_view("sceneAssetId"), std::string_view("runtimeDataPath")};
        if (projectId == nullptr || projectId->kind != JsonKind::String || engineVersion == nullptr ||
            engineVersion->kind != JsonKind::String || configuration == nullptr ||
            configuration->kind != JsonKind::String || startupScene == nullptr ||
            !has_exact_members(*startupScene, startupNames) || files == nullptr || files->kind != JsonKind::Array ||
            files->elements.size() > k_maximumPackageFileEntries)
        {
            return Result<PackageManifest>::failure(manifest_error(
                a_assertContext, PackageError::InvalidPackageManifest, "Package Manifest value types are invalid"));
        }
        EngineVersion parsedEngineVersion{};
        const std::optional<BuildConfiguration> parsedConfiguration = parse_configuration(configuration->text);
        const JsonValue *sceneAssetId = find_member(*startupScene, "sceneAssetId");
        const JsonValue *runtimeDataPath = find_member(*startupScene, "runtimeDataPath");
        if (!parse_engine_version(engineVersion->text, parsedEngineVersion) || !parsedConfiguration.has_value() ||
            sceneAssetId == nullptr || sceneAssetId->kind != JsonKind::String || runtimeDataPath == nullptr ||
            runtimeDataPath->kind != JsonKind::String)
        {
            return Result<PackageManifest>::failure(
                manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                               "Package engine, configuration, or startup scene value is invalid"));
        }
        std::vector<PackageFileEntry> parsedFiles;
        parsedFiles.reserve(files->elements.size());
        std::string_view previousPath;
        constexpr std::array fileNames = {std::string_view("role"), std::string_view("path"),
                                          std::string_view("sizeBytes"), std::string_view("sha256")};
        for (const JsonValue &file : files->elements)
        {
            if (!has_exact_members(file, fileNames))
            {
                return Result<PackageManifest>::failure(manifest_error(
                    a_assertContext, PackageError::InvalidPackageManifest, "Package file entry members are invalid"));
            }
            const JsonValue *role = find_member(file, "role");
            const JsonValue *path = find_member(file, "path");
            const JsonValue *size = find_member(file, "sizeBytes");
            const JsonValue *hash = find_member(file, "sha256");
            std::uint64_t parsedSize = 0U;
            if (role == nullptr || role->kind != JsonKind::String || path == nullptr ||
                path->kind != JsonKind::String || size == nullptr || !parse_unsigned(*size, parsedSize) ||
                hash == nullptr || hash->kind != JsonKind::String)
            {
                return Result<PackageManifest>::failure(manifest_error(
                    a_assertContext, PackageError::InvalidPackageManifest, "Package file entry value is invalid"));
            }
            const std::optional<PackageFileRole> parsedRole = parse_role(role->text);
            if (!parsedRole.has_value() || (!previousPath.empty() && !(previousPath < path->text)))
            {
                return Result<PackageManifest>::failure(
                    manifest_error(a_assertContext, PackageError::InvalidPackageManifest,
                                   "Package file role or canonical path order is invalid"));
            }
            auto entry = PackageFileEntry::create(*parsedRole, path->text, parsedSize, hash->text, a_assertContext);
            if (!entry)
            {
                return Result<PackageManifest>::failure(std::move(*entry.try_error()));
            }
            parsedFiles.push_back(std::move(*entry.try_value()));
            previousPath = parsedFiles.back().relative_path();
        }
        return PackageManifest::create(projectId->text, parsedEngineVersion, *parsedConfiguration, sceneAssetId->text,
                                       runtimeDataPath->text, std::move(parsedFiles), a_assertContext);
    }
    catch (...)
    {
        terminate_manifest_exception(a_assertContext);
    }
}

Result<void> verify_package_manifest_files(std::string_view a_packageRoot, const PackageManifest &a_manifest,
                                           const AssertContext &a_assertContext) noexcept
{
    try
    {
        const std::filesystem::path root = native_path(a_packageRoot);
        std::error_code error;
        const std::filesystem::file_status rootStatus =
            std::filesystem::symlink_status(native_inspection_path(root), error);
        std::error_code indirectError;
        const bool isRootIndirect = is_indirect_path(root, indirectError);
        if (error || indirectError || !root.is_absolute() || !std::filesystem::is_directory(rootStatus) ||
            isRootIndirect)
        {
            return Result<void>::failure(manifest_error(a_assertContext, PackageError::InvalidPackagePath,
                                                        "Package Root is unavailable or indirect"));
        }
        for (const PackageFileEntry &expected : a_manifest.files())
        {
            const std::filesystem::path relative = native_path(expected.relative_path());
            std::filesystem::path current = root;
            for (const std::filesystem::path &segment : relative.parent_path())
            {
                current /= segment;
                error.clear();
                const std::filesystem::file_status status =
                    std::filesystem::symlink_status(native_inspection_path(current), error);
                indirectError.clear();
                const bool isIndirect = is_indirect_path(current, indirectError);
                if (isIndirect)
                {
                    return Result<void>::failure(manifest_error(a_assertContext, PackageError::InvalidPackagePath,
                                                                "Package entry traverses an indirect path"));
                }
                if (error || indirectError || !std::filesystem::is_directory(status))
                {
                    return Result<void>::failure(manifest_error(a_assertContext, PackageError::PackageFileMissing,
                                                                "Required Package parent directory is missing"));
                }
            }
            current = root / relative;
            error.clear();
            const std::filesystem::file_status fileStatus =
                std::filesystem::symlink_status(native_inspection_path(current), error);
            indirectError.clear();
            const bool isFileIndirect = is_indirect_path(current, indirectError);
            if (error || indirectError || isFileIndirect || !std::filesystem::is_regular_file(fileStatus))
            {
                return Result<void>::failure(manifest_error(a_assertContext, PackageError::PackageFileMissing,
                                                            "Required Package file is missing"));
            }
            std::ifstream input(native_inspection_path(current), std::ios::binary);
            if (!input)
            {
                return Result<void>::failure(manifest_error(a_assertContext, PackageError::PackageFileMissing,
                                                            "Required Package file could not be opened"));
            }
            package_private::Sha256 sha256;
            std::array<std::byte, k_hashBufferBytes> buffer{};
            std::uint64_t actualSize = 0U;
            for (;;)
            {
                input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize read = input.gcount();
                if (read > 0)
                {
                    const std::uint64_t count = static_cast<std::uint64_t>(read);
                    if (actualSize > k_maximumPackagedFileBytes - count ||
                        !sha256.update(std::span(buffer.data(), static_cast<std::size_t>(read))))
                    {
                        return Result<void>::failure(
                            manifest_error(a_assertContext, PackageError::PackageManifestResourceLimitExceeded,
                                           "Package file exceeds the supported verification size"));
                    }
                    actualSize += count;
                }
                if (input.eof())
                {
                    break;
                }
                if (!input)
                {
                    return Result<void>::failure(
                        manifest_error(a_assertContext, PackageError::PackageFileMismatch, "Package file read failed"));
                }
            }
            const std::string actualHash = digest_text(sha256.finish());
            if (actualSize != expected.byte_size() || actualHash != expected.sha256())
            {
                return Result<void>::failure(manifest_error(a_assertContext, PackageError::PackageFileMismatch,
                                                            "Package file size or SHA-256 differs from the Manifest"));
            }
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_manifest_exception(a_assertContext);
    }
}
} // namespace cue::package
