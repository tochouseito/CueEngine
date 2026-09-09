#include "RuntimePackage.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/GameModule/GameModuleAbi.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Math/Transform.h>
#include <Cue/Package/Error.h>
#include <Cue/Package/Manifest.h>
#include <Cue/Package/RuntimeData.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef CUE_RUNTIME_BUILD_CONFIGURATION
#error CUE_RUNTIME_BUILD_CONFIGURATION must identify the RuntimeHost build configuration
#endif

namespace
{
constexpr cue::EngineVersion k_engineVersion{1U, 0U, 0U};
constexpr std::size_t k_maximumRuntimeSystems = 256U;

/// @brief Runtime Package処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_package_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Unexpected exception escaped Runtime Package loading");
    std::abort();
}

/// @brief Runtime Package起動の失敗をPackage Domainへ分類する
[[nodiscard]] cue::Error package_error(const cue::AssertContext &a_assertContext,
                                       cue::package::PackageError a_code,
                                       std::string_view a_summary) noexcept
{
    return cue::package::make_package_error(a_assertContext, a_code, a_summary);
}

/// @brief Win32失敗をRuntime Package起動Errorへ変換する
[[nodiscard]] cue::Error windows_package_error(const cue::AssertContext &a_assertContext,
                                               cue::package::PackageError a_code, DWORD a_nativeCode,
                                               std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Package",
                                                 static_cast<std::int64_t>(a_code));
    cue::NativeError native = cue::NativeError::create(a_assertContext.fatal_handler(), "Win32", a_nativeCode);
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief Win32 Handleを単一所有する
class UniqueHandle final
{
  public:
    /// @brief 無効Handleを構築する
    UniqueHandle() noexcept = default;
    /// @brief Native Handleの所有権を取得する
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Handleの複製を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief Handleの複製代入を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    /// @brief Handle所有権を移動する
    UniqueHandle(UniqueHandle &&a_other) noexcept : m_handle(std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE))
    {
    }
    /// @brief 既存Handleを閉じて所有権を移動代入する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    /// @brief 所有Handleを閉じる
    ~UniqueHandle() noexcept
    {
        reset();
    }
    /// @brief 有効Handleか返す
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    /// @brief 借用Native Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

  private:
    /// @brief 所有Handleがあれば閉じて無効化する
    void reset() noexcept
    {
        if (is_valid())
        {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief Canonical Runtime JSONを順序、重複、末尾Data込みでFail-closedに読むCursor
class JsonCursor final
{
  public:
    /// @brief Cursor寿命中だけ入力Byte列を借用する
    explicit JsonCursor(std::string_view a_input) noexcept : m_input(a_input)
    {
    }

    /// @brief 空白を除いた次Tokenが指定文字なら消費する
    [[nodiscard]] bool consume(char a_value) noexcept
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset] != a_value)
        {
            return false;
        }
        ++m_offset;
        return true;
    }

    /// @brief 空白を除いた次Tokenが指定文字か返す
    [[nodiscard]] bool next_is(char a_value) noexcept
    {
        skip_whitespace();
        return m_offset < m_input.size() && m_input[m_offset] == a_value;
    }

    /// @brief Object Member名とColonを固定順で読む
    [[nodiscard]] bool member(std::string_view a_expected)
    {
        std::string name;
        return string(name) && name == a_expected && consume(':');
    }

    /// @brief JSON StringをASCII Escape検証付きで復号する
    [[nodiscard]] bool string(std::string &a_output)
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset++] != '"')
        {
            return false;
        }
        a_output.clear();
        while (m_offset < m_input.size())
        {
            const unsigned char value = static_cast<unsigned char>(m_input[m_offset++]);
            if (value == '"')
            {
                return a_output.size() <= cue::package::k_maximumPackageManifestStringBytes;
            }
            if (value < 0x20U)
            {
                return false;
            }
            if (value != '\\')
            {
                a_output.push_back(static_cast<char>(value));
            }
            else
            {
                if (m_offset >= m_input.size())
                {
                    return false;
                }
                const char escaped = m_input[m_offset++];
                switch (escaped)
                {
                case '"':
                case '\\':
                case '/':
                    a_output.push_back(escaped);
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

    /// @brief JSON unsigned整数を対象型へ完全変換する
    template <typename Value> [[nodiscard]] bool unsigned_number(Value &a_output) noexcept
    {
        skip_whitespace();
        const std::size_t begin = m_offset;
        while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
        {
            ++m_offset;
        }
        if (begin == m_offset || (m_offset - begin > 1U && m_input[begin] == '0'))
        {
            return false;
        }
        const auto converted = std::from_chars(m_input.data() + begin, m_input.data() + m_offset, a_output);
        return converted.ec == std::errc{} && converted.ptr == m_input.data() + m_offset;
    }

    /// @brief JSON有限floatを完全変換する
    [[nodiscard]] bool floating(float &a_output) noexcept
    {
        skip_whitespace();
        const std::size_t begin = m_offset;
        while (m_offset < m_input.size())
        {
            const char value = m_input[m_offset];
            if ((value >= '0' && value <= '9') || value == '-' || value == '+' || value == '.' || value == 'e' ||
                value == 'E')
            {
                ++m_offset;
                continue;
            }
            break;
        }
        if (begin == m_offset)
        {
            return false;
        }
        const auto converted = std::from_chars(m_input.data() + begin, m_input.data() + m_offset, a_output,
                                               std::chars_format::general);
        return converted.ec == std::errc{} && converted.ptr == m_input.data() + m_offset && std::isfinite(a_output);
    }

    /// @brief JSON Booleanを読む
    [[nodiscard]] bool boolean(bool &a_output) noexcept
    {
        skip_whitespace();
        if (m_input.substr(m_offset, 4U) == "true")
        {
            m_offset += 4U;
            a_output = true;
            return true;
        }
        if (m_input.substr(m_offset, 5U) == "false")
        {
            m_offset += 5U;
            a_output = false;
            return true;
        }
        return false;
    }

    /// @brief JSON nullを読む
    [[nodiscard]] bool null_value() noexcept
    {
        skip_whitespace();
        if (m_input.substr(m_offset, 4U) != "null")
        {
            return false;
        }
        m_offset += 4U;
        return true;
    }

    /// @brief 文書末尾まで空白以外がないか返す
    [[nodiscard]] bool finished() noexcept
    {
        skip_whitespace();
        return m_offset == m_input.size();
    }

  private:
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
};

/// @brief canonical major.minor.patch文字列をEngine Versionへ変換する
[[nodiscard]] bool parse_engine_version(std::string_view a_text, cue::EngineVersion &a_output) noexcept
{
    const std::size_t first = a_text.find('.');
    const std::size_t second = first == std::string_view::npos ? first : a_text.find('.', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos || a_text.find('.', second + 1U) !=
                                                                          std::string_view::npos)
    {
        return false;
    }
    const auto parsePart = [](std::string_view a_part, std::uint32_t &a_value) noexcept
    {
        if (a_part.empty() || (a_part.size() > 1U && a_part.front() == '0'))
        {
            return false;
        }
        const auto converted = std::from_chars(a_part.data(), a_part.data() + a_part.size(), a_value);
        return converted.ec == std::errc{} && converted.ptr == a_part.data() + a_part.size();
    };
    return parsePart(a_text.substr(0U, first), a_output.major) &&
           parsePart(a_text.substr(first + 1U, second - first - 1U), a_output.minor) &&
           parsePart(a_text.substr(second + 1U), a_output.patch);
}

/// @brief RuntimeHost Build ConfigurationをManifest表現へ変換する
[[nodiscard]] constexpr cue::BuildConfiguration host_configuration() noexcept
{
#if CUE_RUNTIME_BUILD_CONFIGURATION == 1
    return cue::BuildConfiguration::Debug;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 2
    return cue::BuildConfiguration::Development;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 3
    return cue::BuildConfiguration::Release;
#else
#error Unsupported CUE_RUNTIME_BUILD_CONFIGURATION value
#endif
}

/// @brief RuntimeHost Build ConfigurationをGame Module ABI値へ変換する
[[nodiscard]] constexpr std::uint32_t host_module_configuration() noexcept
{
#if CUE_RUNTIME_BUILD_CONFIGURATION == 1
    return CUE_GAME_MODULE_CONFIGURATION_DEBUG;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 2
    return CUE_GAME_MODULE_CONFIGURATION_DEVELOPMENT;
#else
    return CUE_GAME_MODULE_CONFIGURATION_RELEASE;
#endif
}

/// @brief Runtime Project Dataから起動に必要なIdentityとCompatibilityだけを所有する
struct RuntimeProjectInfo final
{
    std::string projectId;
    cue::EngineCompatibility compatibility;
    std::string startupSceneAssetId;
};

/// @brief Runtime Project Data v1をCanonical Member順とResource Limitへ検証する
[[nodiscard]] cue::Result<RuntimeProjectInfo> parse_runtime_project(
    std::string_view a_bytes, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        JsonCursor cursor(a_bytes);
        std::uint32_t schemaVersion = 0U;
        std::string minimumText;
        std::string maximumText;
        bool hasMaximum = false;
        RuntimeProjectInfo info{{}, {{}, std::nullopt}, {}};
        if (!cursor.consume('{') || !cursor.member("schemaVersion") || !cursor.unsigned_number(schemaVersion) ||
            schemaVersion != cue::package::k_runtimeProjectDataSchemaVersion || !cursor.consume(',') ||
            !cursor.member("projectId") || !cursor.string(info.projectId) || !cursor.consume(',') ||
            !cursor.member("engineCompatibility") || !cursor.consume('{') || !cursor.member("minimum") ||
            !cursor.string(minimumText) || !cursor.consume(',') || !cursor.member("maximumExclusive"))
        {
            return cue::Result<RuntimeProjectInfo>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Project Data members are invalid"));
        }
        if (cursor.next_is('"'))
        {
            hasMaximum = cursor.string(maximumText);
        }
        else if (!cursor.null_value())
        {
            return cue::Result<RuntimeProjectInfo>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Project maximum compatibility is invalid"));
        }
        if (!cursor.consume('}') || !cursor.consume(',') || !cursor.member("requiredCapabilities") ||
            !cursor.consume('[') || !cursor.consume(']') || !cursor.consume(',') ||
            !cursor.member("startupSceneAssetId") || !cursor.string(info.startupSceneAssetId) ||
            !cursor.consume('}') || !cursor.finished() ||
            !parse_engine_version(minimumText, info.compatibility.minimum))
        {
            return cue::Result<RuntimeProjectInfo>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Project Data is not a supported canonical v1 document"));
        }
        if (hasMaximum)
        {
            cue::EngineVersion maximum{};
            if (!parse_engine_version(maximumText, maximum) || maximum <= info.compatibility.minimum)
            {
                return cue::Result<RuntimeProjectInfo>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Project compatibility range is invalid"));
            }
            info.compatibility.maximumExclusive = maximum;
        }
        if (k_engineVersion < info.compatibility.minimum ||
            (info.compatibility.maximumExclusive.has_value() &&
             k_engineVersion >= *info.compatibility.maximumExclusive))
        {
            return cue::Result<RuntimeProjectInfo>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Project is incompatible with this Engine version"));
        }
        return cue::Result<RuntimeProjectInfo>::success(std::move(info));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Runtime Scene Readerが一ObjectをDocument構築まで所有する
struct ParsedRuntimeObject final
{
    cue::scene::ObjectId id;
    std::optional<cue::scene::ObjectId> parentId;
    bool isActive;
    cue::math::Transform transform;
};

/// @brief 固定要素数のfloat Arrayを読む
template <std::size_t Size>
[[nodiscard]] bool read_float_array(JsonCursor &a_cursor, std::array<float, Size> &a_values) noexcept
{
    if (!a_cursor.consume('['))
    {
        return false;
    }
    for (std::size_t index = 0U; index < Size; ++index)
    {
        if ((index > 0U && !a_cursor.consume(',')) || !a_cursor.floating(a_values[index]))
        {
            return false;
        }
    }
    return a_cursor.consume(']');
}

/// @brief Runtime Scene v1のCore ObjectをScene Snapshotへ復元する
[[nodiscard]] cue::Result<cue::scene::SceneSnapshot> parse_runtime_scene(
    std::string_view a_bytes, std::string_view a_expectedSceneId,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        JsonCursor cursor(a_bytes);
        std::uint32_t schemaVersion = 0U;
        std::string sceneIdText;
        if (!cursor.consume('{') || !cursor.member("schemaVersion") || !cursor.unsigned_number(schemaVersion) ||
            schemaVersion != cue::package::k_runtimeSceneDataSchemaVersion || !cursor.consume(',') ||
            !cursor.member("sceneAssetId") || !cursor.string(sceneIdText) || sceneIdText != a_expectedSceneId ||
            !cursor.consume(',') || !cursor.member("objects") || !cursor.consume('['))
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Scene identity or schema is invalid"));
        }
        std::vector<ParsedRuntimeObject> objects;
        while (!cursor.next_is(']'))
        {
            if (!objects.empty() && !cursor.consume(','))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene object separator is invalid"));
            }
            if (objects.size() >= cue::scene::k_maximumSceneObjectCount)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::RuntimeDataResourceLimitExceeded,
                    "Runtime Scene object count exceeds the supported limit"));
            }
            std::string objectIdText;
            std::string parentIdText;
            bool hasParent = false;
            bool isActive = false;
            std::array<float, 3U> translation{};
            std::array<float, 4U> rotation{};
            std::array<float, 3U> scale{};
            if (!cursor.consume('{') || !cursor.member("objectId") || !cursor.string(objectIdText) ||
                !cursor.consume(',') || !cursor.member("parentObjectId"))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene object identity is invalid"));
            }
            if (cursor.next_is('"'))
            {
                hasParent = cursor.string(parentIdText);
            }
            else if (!cursor.null_value())
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene parent identity is invalid"));
            }
            if (!cursor.consume(',') || !cursor.member("active") || !cursor.boolean(isActive) ||
                !cursor.consume(',') || !cursor.member("transform") || !cursor.consume('{') ||
                !cursor.member("translation") || !read_float_array(cursor, translation) || !cursor.consume(',') ||
                !cursor.member("rotation") || !read_float_array(cursor, rotation) || !cursor.consume(',') ||
                !cursor.member("scale") || !read_float_array(cursor, scale) || !cursor.consume('}') ||
                !cursor.consume(',') || !cursor.member("components") || !cursor.consume('['))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene object values are invalid"));
            }
            if (!cursor.consume(']'))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                    "RuntimeHost v1 does not yet instantiate custom Scene components"));
            }
            if (!cursor.consume('}'))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene object has unsupported members"));
            }
            auto objectId = cue::scene::ObjectId::parse(objectIdText, a_assertContext);
            auto parentId = hasParent ? cue::scene::ObjectId::parse(parentIdText, a_assertContext)
                                      : cue::Result<cue::scene::ObjectId>::failure(package_error(
                                            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                            "Runtime Scene object has no parent"));
            auto tolerance = cue::math::Tolerance::create(a_assertContext.fatal_handler(), 0.00001F, 0.00001F);
            auto transform = tolerance
                                 ? cue::math::Transform::create(
                                       a_assertContext.fatal_handler(),
                                       {translation[0], translation[1], translation[2]},
                                       {rotation[0], rotation[1], rotation[2], rotation[3]},
                                       {scale[0], scale[1], scale[2]}, *tolerance.try_value())
                                 : cue::Result<cue::math::Transform>::failure(std::move(*tolerance.try_error()));
            if (!objectId || (hasParent && !parentId) || !transform)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene contains an invalid Object identity or Transform"));
            }
            std::optional<cue::scene::ObjectId> parsedParent;
            if (hasParent)
            {
                parsedParent.emplace(std::move(*parentId.try_value()));
            }
            objects.push_back({std::move(*objectId.try_value()), std::move(parsedParent), isActive,
                               std::move(*transform.try_value())});
        }
        if (!cursor.consume(']') || !cursor.consume('}') || !cursor.finished())
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Scene has trailing or unsupported data"));
        }
        auto sceneId = cue::scene::SceneAssetId::parse(sceneIdText, a_assertContext);
        if (!sceneId)
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Scene identity is invalid"));
        }
        cue::scene::SceneDocument document =
            cue::scene::SceneDocument::create(std::move(*sceneId.try_value()), a_assertContext);
        for (const ParsedRuntimeObject &object : objects)
        {
            const cue::scene::IdentityText name = object.id.canonical_text();
            if (!document.add_object(object.id, std::string_view(name.data(), name.size()), object.isActive,
                                     std::nullopt, object.transform))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene object set is invalid"));
            }
        }
        for (const ParsedRuntimeObject &object : objects)
        {
            if (object.parentId.has_value() && !document.set_parent(object.id, object.parentId))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                    "Runtime Scene hierarchy is invalid"));
            }
        }
        return cue::scene::create_scene_snapshot(document, a_assertContext);
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Metadata v1のPackage起動互換性を検証する
[[nodiscard]] cue::Result<void> validate_module_metadata(
    std::string_view a_bytes, const cue::package::PackageManifest &a_manifest,
    const RuntimeProjectInfo &a_project, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        JsonCursor cursor(a_bytes);
        std::uint32_t schemaVersion = 0U;
        std::uint32_t abiVersion = 0U;
        std::uint64_t compilerVersion = 0U;
        std::uint64_t fullVersion = 0U;
        std::uint64_t build = 0U;
        std::uint32_t iteratorDebugLevel = 0U;
        std::string artifactId;
        std::string projectId;
        std::string minimumText;
        std::string maximumText;
        std::string configuration;
        std::string architecture;
        std::string compilerFamily;
        std::string runtimeLibrary;
        std::string moduleFile;
        std::string entrySymbol;
        bool hasMaximum = false;
        if (!cursor.consume('{') || !cursor.member("schemaVersion") || !cursor.unsigned_number(schemaVersion) ||
            schemaVersion != 1U || !cursor.consume(',') || !cursor.member("artifactId") ||
            !cursor.string(artifactId) || artifactId.empty() || !cursor.consume(',') ||
            !cursor.member("projectId") || !cursor.string(projectId) || !cursor.consume(',') ||
            !cursor.member("engineCompatibility") || !cursor.consume('{') || !cursor.member("minimum") ||
            !cursor.string(minimumText) || !cursor.consume(',') || !cursor.member("maximumExclusive"))
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata header is invalid"));
        }
        if (cursor.next_is('"'))
        {
            hasMaximum = cursor.string(maximumText);
        }
        else if (!cursor.null_value())
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata compatibility is invalid"));
        }
        if (!cursor.consume('}') || !cursor.consume(',') || !cursor.member("abiVersion") ||
            !cursor.unsigned_number(abiVersion) || !cursor.consume(',') || !cursor.member("configuration") ||
            !cursor.string(configuration) || !cursor.consume(',') || !cursor.member("architecture") ||
            !cursor.string(architecture) || !cursor.consume(',') || !cursor.member("compilerFamily") ||
            !cursor.string(compilerFamily) || !cursor.consume(',') || !cursor.member("msvcToolset") ||
            !cursor.consume('{') || !cursor.member("compilerVersion") ||
            !cursor.unsigned_number(compilerVersion) || !cursor.consume(',') || !cursor.member("fullVersion") ||
            !cursor.unsigned_number(fullVersion) || !cursor.consume(',') || !cursor.member("build") ||
            !cursor.unsigned_number(build) || !cursor.consume('}') || !cursor.consume(',') ||
            !cursor.member("runtimeLibrary") || !cursor.string(runtimeLibrary) || !cursor.consume(',') ||
            !cursor.member("iteratorDebugLevel") || !cursor.unsigned_number(iteratorDebugLevel) ||
            !cursor.consume(',') || !cursor.member("moduleFile") || !cursor.string(moduleFile) ||
            !cursor.consume(',') || !cursor.member("entrySymbol") || !cursor.string(entrySymbol) ||
            !cursor.consume('}') || !cursor.finished())
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata body is invalid"));
        }
        cue::EngineVersion minimum{};
        cue::EngineVersion maximum{};
        const bool compatibilityMatches = parse_engine_version(minimumText, minimum) &&
                                          minimum == a_project.compatibility.minimum &&
                                          (hasMaximum == a_project.compatibility.maximumExclusive.has_value()) &&
                                          (!hasMaximum || (parse_engine_version(maximumText, maximum) &&
                                                           maximum == *a_project.compatibility.maximumExclusive));
        const std::string_view expectedConfiguration =
            a_manifest.configuration() == cue::BuildConfiguration::Debug
                ? "Debug"
                : (a_manifest.configuration() == cue::BuildConfiguration::Development ? "Development" : "Release");
        const bool debug = a_manifest.configuration() == cue::BuildConfiguration::Debug;
        // _MSC_FULL_VERと_MSC_BUILDはProvenanceとして保持し、同一_MSC_VER内のServicing更新は許容する。
        if (projectId != a_manifest.project_id() || projectId != a_project.projectId || !compatibilityMatches ||
            abiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || configuration != expectedConfiguration ||
            architecture != "x64" || compilerFamily != "msvc" || compilerVersion != _MSC_VER ||
            runtimeLibrary != (debug ? "DebugDll" : "Dll") || iteratorDebugLevel != (debug ? 2U : 0U) ||
            moduleFile != "CueGameModule.dll" || entrySymbol != "cue_game_module_query")
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata is incompatible with the Package"));
        }
        return cue::Result<void>::success();
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Executable Moduleの完全Pathを切捨てなしで取得する
[[nodiscard]] cue::Result<std::filesystem::path> executable_path(
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<wchar_t> buffer(512U, L'\0');
        for (;;)
        {
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0U)
            {
                return cue::Result<std::filesystem::path>::failure(windows_package_error(
                    a_assertContext, cue::package::PackageError::InvalidPackagePath, GetLastError(),
                    "RuntimeHost executable path could not be resolved"));
            }
            if (length < buffer.size() - 1U)
            {
                return cue::Result<std::filesystem::path>::success(
                    std::filesystem::path(std::wstring_view(buffer.data(), length)));
            }
            if (buffer.size() >= 32768U)
            {
                return cue::Result<std::filesystem::path>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidPackagePath,
                    "RuntimeHost executable path exceeds the Windows limit"));
            }
            buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32768U));
        }
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief FilesystemRootからManifestに列挙された一Fileだけを読んでByte Identityを再検証する
[[nodiscard]] cue::Result<std::vector<std::byte>> read_manifest_file(
    cue::FilesystemRoot &a_filesystem, const cue::package::PackageFileEntry &a_entry,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto path = cue::RelativePath::parse(a_entry.relative_path(), a_assertContext);
    if (!path)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*path.try_error()));
    }
    auto bytes = a_filesystem.read_file(*path.try_value(), static_cast<std::size_t>(a_entry.byte_size()));
    if (!bytes)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*bytes.try_error()));
    }
    auto verified = cue::package::verify_package_file_bytes(a_entry, *bytes.try_value(), a_assertContext);
    if (!verified)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*verified.try_error()));
    }
    return bytes;
}

/// @brief Manifest内の必須Role一件を返す
[[nodiscard]] const cue::package::PackageFileEntry *find_role(
    const cue::package::PackageManifest &a_manifest, cue::package::PackageFileRole a_role) noexcept
{
    const auto found = std::find_if(a_manifest.files().begin(), a_manifest.files().end(),
                                    [a_role](const cue::package::PackageFileEntry &a_entry) noexcept
                                    { return a_entry.role() == a_role; });
    return found == a_manifest.files().end() ? nullptr : &*found;
}

/// @brief byte列をUTF-8検証済みParser入力Viewへ変換する
[[nodiscard]] std::string_view text_view(const std::vector<std::byte> &a_bytes) noexcept
{
    return {reinterpret_cast<const char *>(a_bytes.data()), a_bytes.size()};
}

/// @brief ABI UUIDをlowercase canonical Textへ変換する
[[nodiscard]] std::string uuid_text(const CueGameUuidV1 &a_uuid)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(36U);
    for (std::size_t index = 0U; index < 16U; ++index)
    {
        if (index == 4U || index == 6U || index == 8U || index == 10U)
        {
            output.push_back('-');
        }
        output.push_back(digits[(a_uuid.bytes[index] >> 4U) & 0x0fU]);
        output.push_back(digits[a_uuid.bytes[index] & 0x0fU]);
    }
    return output;
}

/// @brief Module Diagnosticへ呼出中有効な固定Messageを設定する
void set_module_diagnostic(CueGameModuleDiagnosticV1 *a_diagnostic, CueGameModuleResult a_code,
                           const char *a_message) noexcept
{
    if (a_diagnostic == nullptr || a_diagnostic->structSize < sizeof(CueGameModuleDiagnosticV1) ||
        a_diagnostic->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1)
    {
        return;
    }
    a_diagnostic->code = a_code;
    a_diagnostic->reserved = 0U;
    a_diagnostic->message = {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, a_message,
                             a_message == nullptr ? 0U : static_cast<std::uint64_t>(std::char_traits<char>::length(a_message))};
}

/// @brief Module登録中に収集する一System Definition
struct PendingSystem final
{
    cue::game_core::RuntimeSystemDescriptor descriptor;
    CueGameSystemCreateV1 createState;
    CueGameSystemDestroyV1 destroyState;
    CueGameSystemStartV1 start;
    CueGameSystemUpdateCallbackV1 update;
    CueGameSystemStopV1 stop;
};

enum class RegistrationStage : std::uint8_t
{
    Schemas,
    Components,
    Systems
};

/// @brief Game Module ABI SinkへOwner Thread限定の登録状態を渡す
struct RegistrationContext final
{
    RegistrationStage stage = RegistrationStage::Schemas;
    std::vector<PendingSystem> systems;
};

/// @brief M16未対応のGame Schema登録をFail-closedに拒否する
CueGameModuleResult CUE_GAME_MODULE_CALL reject_schema_registration(
    void *a_context, const CueGameSchemaDescriptorV1 *, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_context == nullptr || static_cast<RegistrationContext *>(a_context)->stage != RegistrationStage::Schemas)
    {
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                              "Schema registration occurred outside its stage");
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED,
                          "RuntimeHost v1 does not yet accept custom Schema registration");
    return CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED;
}

/// @brief M16未対応のGame Component登録をFail-closedに拒否する
CueGameModuleResult CUE_GAME_MODULE_CALL reject_component_registration(
    void *a_context, const CueGameComponentDescriptorV1 *, CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    if (a_context == nullptr || static_cast<RegistrationContext *>(a_context)->stage != RegistrationStage::Components)
    {
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                              "Component registration occurred outside its stage");
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED,
                          "RuntimeHost v1 does not yet accept custom Component registration");
    return CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED;
}

/// @brief ABI UTF-8 Viewを所有文字列へ検証Copyする
[[nodiscard]] bool copy_utf8_view(const CueGameUtf8ViewV1 &a_view, std::string &a_output)
{
    if (a_view.structSize < sizeof(CueGameUtf8ViewV1) || a_view.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
        a_view.size > cue::package::k_maximumPackageManifestStringBytes ||
        (a_view.size != 0U && a_view.data == nullptr))
    {
        return false;
    }
    a_output.assign(a_view.data == nullptr ? "" : a_view.data, static_cast<std::size_t>(a_view.size));
    return !a_output.empty();
}

/// @brief Game Module System DescriptorとCallbackをHost所有定義へCopy登録する
CueGameModuleResult CUE_GAME_MODULE_CALL register_system(
    void *a_context, const CueGameSystemDescriptorV1 *a_descriptor,
    CueGameModuleDiagnosticV1 *a_diagnostic) noexcept
{
    auto *context = static_cast<RegistrationContext *>(a_context);
    if (context == nullptr || context->stage != RegistrationStage::Systems || a_descriptor == nullptr ||
        a_descriptor->structSize < sizeof(CueGameSystemDescriptorV1) ||
        a_descriptor->version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
        a_descriptor->dependencyCount > k_maximumRuntimeSystems ||
        (a_descriptor->dependencyCount != 0U && a_descriptor->dependencies == nullptr) ||
        a_descriptor->createState == nullptr || a_descriptor->destroyState == nullptr || a_descriptor->start == nullptr ||
        a_descriptor->update == nullptr || a_descriptor->stop == nullptr || context->systems.size() >= k_maximumRuntimeSystems)
    {
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                              "Runtime System descriptor is invalid");
        return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
    }
    try
    {
        PendingSystem pending{{}, a_descriptor->createState, a_descriptor->destroyState, a_descriptor->start,
                              a_descriptor->update, a_descriptor->stop};
        if (!copy_utf8_view(a_descriptor->stableId, pending.descriptor.id))
        {
            set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                  "Runtime System stable ID is invalid");
            return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
        }
        switch (a_descriptor->phase)
        {
        case CUE_GAME_MODULE_SYSTEM_PHASE_PRE_UPDATE:
            pending.descriptor.phase = cue::game_core::RuntimeUpdatePhase::PreUpdate;
            break;
        case CUE_GAME_MODULE_SYSTEM_PHASE_UPDATE:
            pending.descriptor.phase = cue::game_core::RuntimeUpdatePhase::Update;
            break;
        case CUE_GAME_MODULE_SYSTEM_PHASE_POST_UPDATE:
            pending.descriptor.phase = cue::game_core::RuntimeUpdatePhase::PostUpdate;
            break;
        default:
            set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                  "Runtime System phase is invalid");
            return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
        }
        pending.descriptor.order = a_descriptor->order;
        for (std::size_t index = 0U; index < a_descriptor->dependencyCount; ++index)
        {
            std::string dependency;
            if (!copy_utf8_view(a_descriptor->dependencies[index], dependency))
            {
                set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT,
                                      "Runtime System dependency is invalid");
                return CUE_GAME_MODULE_RESULT_INVALID_ARGUMENT;
            }
            pending.descriptor.dependencies.push_back(std::move(dependency));
        }
        context->systems.push_back(std::move(pending));
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_SUCCESS, nullptr);
        return CUE_GAME_MODULE_RESULT_SUCCESS;
    }
    catch (const std::bad_alloc &)
    {
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY,
                              "Runtime System registration allocation failed");
        return CUE_GAME_MODULE_RESULT_OUT_OF_MEMORY;
    }
    catch (...)
    {
        set_module_diagnostic(a_diagnostic, CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED,
                              "Runtime System registration failed unexpectedly");
        return CUE_GAME_MODULE_RESULT_REGISTRATION_FAILED;
    }
}

/// @brief Game Module登録Callbackの失敗をRuntime Errorへ変換する
[[nodiscard]] cue::Result<void> call_registration(
    CueGameModuleRegisterV1 a_callback, CueGameModuleHandle a_module, RegistrationContext &a_context,
    RegistrationStage a_stage, const cue::AssertContext &a_assertContext) noexcept
{
    a_context.stage = a_stage;
    CueGameRegistrationSinkV1 sink{sizeof(CueGameRegistrationSinkV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                   &a_context, &reject_schema_registration, &reject_component_registration,
                                   &register_system, {0U, 0U, 0U, 0U}};
    CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                         0U, 0U,
                                         {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, 0U}};
    const CueGameModuleResult result = a_callback(a_module, &sink, &diagnostic);
    if (result != CUE_GAME_MODULE_RESULT_SUCCESS)
    {
        return cue::Result<void>::failure(cue::runtime::make_runtime_error(
            a_assertContext, cue::runtime::RuntimeError::InvalidApplicationConfiguration,
            "Game Module registration failed"));
    }
    return cue::Result<void>::success();
}

/// @brief DLL所有System StateをRuntimeSystemへ適合する
class DllRuntimeSystem final : public cue::game_core::RuntimeSystem
{
  public:
    /// @brief Callback、Module、Stateを一回のSession Systemへ束ねる
    DllRuntimeSystem(CueGameModuleHandle a_module, CueGameSystemState a_state, PendingSystem a_definition,
                     const cue::AssertContext &a_assertContext) noexcept
        : m_module(a_module), m_state(a_state), m_definition(std::move(a_definition)),
          m_assertContext(&a_assertContext)
    {
    }
    /// @brief DLL Stateを生成元Callbackで一度だけ破棄する
    ~DllRuntimeSystem() noexcept override
    {
        if (m_state != nullptr)
        {
            m_definition.destroyState(m_state);
        }
    }

    /// @brief DLL SystemのStart Callbackを呼ぶ
    [[nodiscard]] cue::Result<void> start(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                             0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr,
                                              0U}};
        if (m_definition.start(m_state, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS)
        {
            return cue::Result<void>::failure(cue::runtime::make_runtime_error(
                *m_assertContext, cue::runtime::RuntimeError::ApplicationSessionStartFailed,
                "Game Module System start failed"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Portable Frame TimingをDLL ABIへCopyしてUpdate Callbackを呼ぶ
    [[nodiscard]] cue::Result<void> update(
        const cue::game_core::RuntimeSystemUpdateContext &a_context) noexcept override
    {
        const cue::game_core::FrameTiming &timing = a_context.timing.timing();
        CueGameSystemUpdateV1 update{sizeof(CueGameSystemUpdateV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                     timing.frameIndex, timing.simulationDeltaNanoseconds,
                                     timing.simulationTimeNanoseconds};
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                             0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr,
                                              0U}};
        if (m_definition.update(m_state, &update, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS)
        {
            return cue::Result<void>::failure(cue::runtime::make_runtime_error(
                *m_assertContext, cue::runtime::RuntimeError::ApplicationSessionUpdateFailed,
                "Game Module System update failed"));
        }
        return cue::Result<void>::success();
    }

    /// @brief DLL SystemのStop Callbackを呼ぶ
    [[nodiscard]] cue::Result<void> stop(cue::game_core::RuntimeSystemContext &) noexcept override
    {
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                             0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr,
                                              0U}};
        if (m_definition.stop(m_state, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS)
        {
            return cue::Result<void>::failure(cue::runtime::make_runtime_error(
                *m_assertContext, cue::runtime::RuntimeError::ApplicationSessionCleanupFailed,
                "Game Module System stop failed"));
        }
        return cue::Result<void>::success();
    }

  private:
    CueGameModuleHandle m_module;
    CueGameSystemState m_state;
    PendingSystem m_definition;
    const cue::AssertContext *m_assertContext;
};

/// @brief Pending DLL System群からSession所有登録を構築する
[[nodiscard]] cue::Result<std::vector<cue::runtime::RuntimeSystemRegistration>> create_systems(
    CueGameModuleHandle a_module, std::vector<PendingSystem> a_pending,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<cue::runtime::RuntimeSystemRegistration> systems;
        systems.reserve(a_pending.size());
        for (PendingSystem &pending : a_pending)
        {
            CueGameSystemState state = nullptr;
            CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1),
                                                 CUE_GAME_MODULE_STRUCTURE_VERSION_1, 0U, 0U,
                                                 {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                                  nullptr, 0U}};
            if (pending.createState(a_module, &state, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS || state == nullptr)
            {
                return cue::Result<std::vector<cue::runtime::RuntimeSystemRegistration>>::failure(
                    cue::runtime::make_runtime_error(a_assertContext,
                                                     cue::runtime::RuntimeError::InvalidApplicationConfiguration,
                                                     "Game Module System state creation failed"));
            }
            cue::game_core::RuntimeSystemDescriptor descriptor = pending.descriptor;
            auto system = std::make_unique<DllRuntimeSystem>(a_module, state, std::move(pending), a_assertContext);
            systems.push_back({std::move(descriptor), std::move(system)});
        }
        return cue::Result<std::vector<cue::runtime::RuntimeSystemRegistration>>::success(std::move(systems));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Win32 DLLと関連Guardを正しい順序で所有するRuntime Package Module
class WindowsRuntimePackageModule final : public cue::runtime_host::RuntimePackageModule
{
  public:
    /// @brief DLLとProject Scope Handle、検索Directory Cookie、固定Handleを所有する
    WindowsRuntimePackageModule(HMODULE a_library, CueGameModuleHandle a_module, const CueGameModuleApiV1 &a_api,
                                DLL_DIRECTORY_COOKIE a_runtimeCookie, UniqueHandle a_rootGuard,
                                UniqueHandle a_gameGuard, UniqueHandle a_runtimeGuard, UniqueHandle a_moduleGuard) noexcept
        : m_library(a_library), m_module(a_module), m_api(&a_api), m_runtimeCookie(a_runtimeCookie),
          m_rootGuard(std::move(a_rootGuard)), m_gameGuard(std::move(a_gameGuard)),
          m_runtimeGuard(std::move(a_runtimeGuard)), m_moduleGuard(std::move(a_moduleGuard))
    {
    }
    /// @brief System State破棄後にModule、DLL、検索Directoryを逆順Cleanupする
    ~WindowsRuntimePackageModule() noexcept override
    {
        if (m_module != nullptr)
        {
            m_api->destroyModule(m_module);
        }
        if (m_library != nullptr)
        {
            FreeLibrary(m_library);
        }
        if (m_runtimeCookie != nullptr)
        {
            RemoveDllDirectory(m_runtimeCookie);
        }
    }

  private:
    HMODULE m_library;
    CueGameModuleHandle m_module;
    const CueGameModuleApiV1 *m_api;
    DLL_DIRECTORY_COOKIE m_runtimeCookie;
    UniqueHandle m_rootGuard;
    UniqueHandle m_gameGuard;
    UniqueHandle m_runtimeGuard;
    UniqueHandle m_moduleGuard;
};

/// @brief DirectoryをReparse追跡なし、Delete共有なしで固定する
[[nodiscard]] cue::Result<UniqueHandle> guard_directory(
    const std::filesystem::path &a_path, const cue::AssertContext &a_assertContext) noexcept
{
    UniqueHandle handle(CreateFileW(a_path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<UniqueHandle>::failure(windows_package_error(
            a_assertContext, cue::package::PackageError::InvalidPackagePath, GetLastError(),
            "Runtime Package directory could not be fixed"));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<UniqueHandle>::failure(package_error(
            a_assertContext, cue::package::PackageError::InvalidPackagePath,
            "Runtime Package directory is not a regular non-reparse directory"));
    }
    return cue::Result<UniqueHandle>::success(std::move(handle));
}

/// @brief ASCII英字だけをlowercaseへ変換してWindows File名比較Keyを返す
[[nodiscard]] std::wstring ascii_case_key(std::wstring_view a_value)
{
    std::wstring key(a_value);
    for (wchar_t &character : key)
    {
        if (character >= L'A' && character <= L'Z')
        {
            character = static_cast<wchar_t>(character + (L'a' - L'A'));
        }
    }
    return key;
}

/// @brief Runtime Directoryの通常File集合がManifestの依存Entryと完全一致するか検証する
[[nodiscard]] cue::Result<bool> validate_runtime_dependency_inventory(
    const std::filesystem::path &a_root, const cue::package::PackageManifest &a_manifest,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<std::wstring> expected;
        for (const cue::package::PackageFileEntry &entry : a_manifest.files())
        {
            if (entry.role() != cue::package::PackageFileRole::RuntimeDependency)
            {
                continue;
            }
            constexpr std::string_view prefix = "Runtime/";
            const std::string_view path = entry.relative_path();
            if (!path.starts_with(prefix) || path.size() == prefix.size())
            {
                return cue::Result<bool>::failure(package_error(
                    a_assertContext, cue::package::PackageError::InvalidPackageManifest,
                    "Runtime dependency path is outside the direct Runtime directory"));
            }
            std::wstring name;
            name.reserve(path.size() - prefix.size());
            for (const char character : path.substr(prefix.size()))
            {
                name.push_back(static_cast<unsigned char>(character));
            }
            expected.push_back(ascii_case_key(name));
        }
        std::sort(expected.begin(), expected.end());

        const std::filesystem::path runtimePath = a_root / "Runtime";
        const DWORD runtimeAttributes = GetFileAttributesW(runtimePath.c_str());
        if (runtimeAttributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD code = GetLastError();
            if ((code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) && expected.empty())
            {
                return cue::Result<bool>::success(false);
            }
            return cue::Result<bool>::failure(windows_package_error(
                a_assertContext, cue::package::PackageError::PackageFileMissing, code,
                "Runtime dependency directory could not be inspected"));
        }
        if ((runtimeAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (runtimeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return cue::Result<bool>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidPackagePath,
                "Runtime dependency path is not a regular non-reparse directory"));
        }

        std::vector<std::wstring> actual;
        std::error_code iteratorError;
        std::filesystem::directory_iterator iterator(runtimePath, iteratorError);
        const std::filesystem::directory_iterator end;
        while (!iteratorError && iterator != end)
        {
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                return cue::Result<bool>::failure(package_error(
                    a_assertContext, cue::package::PackageError::PackageFileMismatch,
                    "Runtime directory contains a non-regular or reparse entry"));
            }
            actual.push_back(ascii_case_key(iterator->path().filename().wstring()));
            iterator.increment(iteratorError);
        }
        if (iteratorError)
        {
            return cue::Result<bool>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidPackagePath,
                "Runtime dependency directory could not be enumerated"));
        }
        std::sort(actual.begin(), actual.end());
        if (actual != expected)
        {
            return cue::Result<bool>::failure(package_error(
                a_assertContext, cue::package::PackageError::PackageFileMismatch,
                "Runtime directory inventory differs from the Manifest"));
        }
        return cue::Result<bool>::success(!expected.empty());
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Game Module FileをWrite／Delete共有なしで固定する
[[nodiscard]] cue::Result<UniqueHandle> guard_module_file(
    const std::filesystem::path &a_path, const cue::AssertContext &a_assertContext) noexcept
{
    UniqueHandle handle(CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<UniqueHandle>::failure(windows_package_error(
            a_assertContext, cue::package::PackageError::PackageFileMissing, GetLastError(),
            "Game Module file could not be fixed"));
    }
    return cue::Result<UniqueHandle>::success(std::move(handle));
}
} // namespace

namespace cue::runtime_host
{
LoadedRuntimePackage::LoadedRuntimePackage(std::string a_packageRoot, std::string a_projectId,
                                           std::unique_ptr<RuntimePackageModule> a_module,
                                           std::unique_ptr<schema::SchemaRegistry> a_schemaRegistry,
                                           scene::SceneSnapshot a_startupScene,
                                           std::vector<runtime::RuntimeSystemRegistration> a_systems) noexcept
    : m_packageRoot(std::move(a_packageRoot)), m_projectId(std::move(a_projectId)), m_module(std::move(a_module)),
      m_schemaRegistry(std::move(a_schemaRegistry)), m_startupScene(std::move(a_startupScene)),
      m_systems(std::move(a_systems))
{
}

LoadedRuntimePackage::~LoadedRuntimePackage() noexcept = default;

std::string_view LoadedRuntimePackage::package_root() const noexcept
{
    return m_packageRoot;
}

std::string_view LoadedRuntimePackage::project_id() const noexcept
{
    return m_projectId;
}

const schema::SchemaRegistry &LoadedRuntimePackage::schema_registry() const noexcept
{
    return *m_schemaRegistry;
}

const scene::SceneSnapshot &LoadedRuntimePackage::startup_scene() const noexcept
{
    return m_startupScene;
}

std::vector<runtime::RuntimeSystemRegistration> LoadedRuntimePackage::take_systems() noexcept
{
    return std::move(m_systems);
}

std::unique_ptr<RuntimePackageModule> LoadedRuntimePackage::take_module() noexcept
{
    return std::move(m_module);
}

std::unique_ptr<schema::SchemaRegistry> LoadedRuntimePackage::take_schema_registry() noexcept
{
    return std::move(m_schemaRegistry);
}

scene::SceneSnapshot LoadedRuntimePackage::take_startup_scene() noexcept
{
    return std::move(m_startupScene);
}

Result<LoadedRuntimePackage> load_runtime_package(schema::SchemaRegistryIdentitySource &a_identitySource,
                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) == FALSE)
        {
            return Result<LoadedRuntimePackage>::failure(windows_package_error(
                a_assertContext, package::PackageError::InvalidPackagePath, GetLastError(),
                "RuntimeHost could not restrict the process DLL search policy"));
        }
        auto executable = executable_path(a_assertContext);
        if (!executable)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*executable.try_error()));
        }
        const std::filesystem::path root = executable.try_value()->parent_path();
        auto filesystem = create_windows_filesystem_root(root.generic_string(), a_assertContext);
        if (!filesystem)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*filesystem.try_error()));
        }
        auto manifestPath = RelativePath::parse("CuePackage.json", a_assertContext);
        if (!manifestPath)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*manifestPath.try_error()));
        }
        auto manifestBytes = filesystem.try_value()->get()->read_file(*manifestPath.try_value(),
                                                                       package::k_maximumPackageManifestBytes);
        if (!manifestBytes)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*manifestBytes.try_error()));
        }
        auto manifest = package::parse_package_manifest(text_view(*manifestBytes.try_value()), a_assertContext);
        if (!manifest)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*manifest.try_error()));
        }
        if (manifest.try_value()->engine_version() != k_engineVersion ||
            manifest.try_value()->configuration() != host_configuration())
        {
            return Result<LoadedRuntimePackage>::failure(package_error(
                a_assertContext, package::PackageError::InvalidPackageManifest,
                "Package Engine version or Build Configuration differs from RuntimeHost"));
        }
        auto inventory = package::verify_package_manifest_files(root.generic_string(), *manifest.try_value(),
                                                                 a_assertContext);
        if (!inventory)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*inventory.try_error()));
        }

        const package::PackageFileEntry *projectEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::ProjectRuntimeData);
        const package::PackageFileEntry *sceneEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::StartupSceneRuntimeData);
        const package::PackageFileEntry *metadataEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::GameModuleMetadata);
        const package::PackageFileEntry *moduleEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::GameModule);
        if (projectEntry == nullptr || sceneEntry == nullptr || metadataEntry == nullptr || moduleEntry == nullptr)
        {
            return Result<LoadedRuntimePackage>::failure(package_error(
                a_assertContext, package::PackageError::InvalidPackageManifest,
                "Package is missing a required Runtime role"));
        }
        auto projectBytes = read_manifest_file(**filesystem.try_value(), *projectEntry, a_assertContext);
        auto sceneBytes = read_manifest_file(**filesystem.try_value(), *sceneEntry, a_assertContext);
        auto metadataBytes = read_manifest_file(**filesystem.try_value(), *metadataEntry, a_assertContext);
        if (!projectBytes || !sceneBytes || !metadataBytes)
        {
            Error error = !projectBytes ? std::move(*projectBytes.try_error())
                                       : (!sceneBytes ? std::move(*sceneBytes.try_error())
                                                      : std::move(*metadataBytes.try_error()));
            return Result<LoadedRuntimePackage>::failure(std::move(error));
        }
        auto project = parse_runtime_project(text_view(*projectBytes.try_value()), a_assertContext);
        if (!project || project.try_value()->projectId != manifest.try_value()->project_id() ||
            project.try_value()->startupSceneAssetId != manifest.try_value()->startup_scene_asset_id())
        {
            return Result<LoadedRuntimePackage>::failure(
                project ? package_error(a_assertContext, package::PackageError::InvalidRuntimeData,
                                        "Runtime Project identity differs from the Manifest")
                        : std::move(*project.try_error()));
        }
        auto metadata = validate_module_metadata(text_view(*metadataBytes.try_value()), *manifest.try_value(),
                                                 *project.try_value(), a_assertContext);
        if (!metadata)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*metadata.try_error()));
        }
        auto startupScene = parse_runtime_scene(text_view(*sceneBytes.try_value()),
                                                manifest.try_value()->startup_scene_asset_id(), a_assertContext);
        if (!startupScene)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*startupScene.try_error()));
        }

        auto rootGuard = guard_directory(root, a_assertContext);
        auto gameGuard = guard_directory(root / "Game", a_assertContext);
        if (!rootGuard || !gameGuard)
        {
            return Result<LoadedRuntimePackage>::failure(
                !rootGuard ? std::move(*rootGuard.try_error()) : std::move(*gameGuard.try_error()));
        }
        UniqueHandle runtimeGuard;
        DLL_DIRECTORY_COOKIE runtimeCookie = nullptr;
        auto runtimeInventory =
            validate_runtime_dependency_inventory(root, *manifest.try_value(), a_assertContext);
        if (!runtimeInventory)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*runtimeInventory.try_error()));
        }
        const bool hasRuntimeDependencies = *runtimeInventory.try_value();
        if (hasRuntimeDependencies)
        {
            auto guarded = guard_directory(root / "Runtime", a_assertContext);
            if (!guarded)
            {
                return Result<LoadedRuntimePackage>::failure(std::move(*guarded.try_error()));
            }
            runtimeGuard = std::move(*guarded.try_value());
            runtimeCookie = AddDllDirectory((root / "Runtime").c_str());
            if (runtimeCookie == nullptr)
            {
                return Result<LoadedRuntimePackage>::failure(windows_package_error(
                    a_assertContext, package::PackageError::InvalidPackagePath, GetLastError(),
                    "Runtime dependency directory could not be registered"));
            }
        }
        const std::filesystem::path modulePath = root / std::filesystem::path(moduleEntry->relative_path());
        auto moduleGuard = guard_module_file(modulePath, a_assertContext);
        if (!moduleGuard)
        {
            if (runtimeCookie != nullptr)
            {
                RemoveDllDirectory(runtimeCookie);
            }
            return Result<LoadedRuntimePackage>::failure(std::move(*moduleGuard.try_error()));
        }
        HMODULE library = LoadLibraryExW(
            modulePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (library == nullptr)
        {
            const DWORD code = GetLastError();
            if (runtimeCookie != nullptr)
            {
                RemoveDllDirectory(runtimeCookie);
            }
            return Result<LoadedRuntimePackage>::failure(windows_package_error(
                a_assertContext, package::PackageError::InvalidRuntimeData, code,
                "Game Module could not be loaded from the Manifest path"));
        }
        using Query = CueGameModuleResult(CUE_GAME_MODULE_CALL *)(std::uint32_t, CueGameModuleQueryOutputV1 *,
                                                                  CueGameModuleDiagnosticV1 *) noexcept;
        const FARPROC queryAddress = GetProcAddress(library, "cue_game_module_query");
        if (queryAddress == nullptr)
        {
            FreeLibrary(library);
            if (runtimeCookie != nullptr)
            {
                RemoveDllDirectory(runtimeCookie);
            }
            return Result<LoadedRuntimePackage>::failure(windows_package_error(
                a_assertContext, package::PackageError::InvalidRuntimeData, ERROR_PROC_NOT_FOUND,
                "Game Module entry symbol is missing"));
        }
        const Query query = std::bit_cast<Query>(queryAddress);
        CueGameModuleQueryOutputV1 queryOutput{sizeof(CueGameModuleQueryOutputV1),
                                               CUE_GAME_MODULE_STRUCTURE_VERSION_1, nullptr, {0U, 0U}};
        CueGameModuleDiagnosticV1 diagnostic{sizeof(CueGameModuleDiagnosticV1),
                                             CUE_GAME_MODULE_STRUCTURE_VERSION_1, 0U, 0U,
                                             {sizeof(CueGameUtf8ViewV1), CUE_GAME_MODULE_STRUCTURE_VERSION_1,
                                              nullptr, 0U}};
        if (query(CUE_GAME_MODULE_ABI_VERSION_1, &queryOutput, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS ||
            queryOutput.api == nullptr)
        {
            FreeLibrary(library);
            if (runtimeCookie != nullptr)
            {
                RemoveDllDirectory(runtimeCookie);
            }
            return Result<LoadedRuntimePackage>::failure(package_error(
                a_assertContext, package::PackageError::InvalidRuntimeData,
                "Game Module rejected the RuntimeHost ABI"));
        }
        const CueGameModuleApiV1 &api = *queryOutput.api;
        if (api.structSize < sizeof(CueGameModuleApiV1) || api.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
            api.abiVersion != CUE_GAME_MODULE_ABI_VERSION_1 || api.configuration != host_module_configuration() ||
            api.architecture != CUE_GAME_MODULE_ARCHITECTURE_X64 || api.reserved != 0U ||
            api.projectId.structSize < sizeof(CueGameUuidV1) ||
            api.projectId.version != CUE_GAME_MODULE_STRUCTURE_VERSION_1 ||
            uuid_text(api.projectId) != manifest.try_value()->project_id() || api.createModule == nullptr ||
            api.registerSchemas == nullptr || api.registerComponents == nullptr || api.registerSystems == nullptr ||
            api.destroyModule == nullptr)
        {
            FreeLibrary(library);
            if (runtimeCookie != nullptr)
            {
                RemoveDllDirectory(runtimeCookie);
            }
            return Result<LoadedRuntimePackage>::failure(package_error(
                a_assertContext, package::PackageError::InvalidRuntimeData,
                "Game Module API identity or lifecycle is incompatible"));
        }
        CueGameModuleHandle moduleHandle = nullptr;
        if (api.createModule(&moduleHandle, &diagnostic) != CUE_GAME_MODULE_RESULT_SUCCESS || moduleHandle == nullptr)
        {
            FreeLibrary(library);
            if (runtimeCookie != nullptr)
            {
                RemoveDllDirectory(runtimeCookie);
            }
            return Result<LoadedRuntimePackage>::failure(cue::runtime::make_runtime_error(
                a_assertContext, cue::runtime::RuntimeError::InvalidApplicationConfiguration,
                "Game Module Project Scope creation failed"));
        }
        auto module = std::make_unique<WindowsRuntimePackageModule>(
            library, moduleHandle, api, runtimeCookie, std::move(*rootGuard.try_value()),
            std::move(*gameGuard.try_value()), std::move(runtimeGuard), std::move(*moduleGuard.try_value()));
        RegistrationContext registration;
        auto schemas = call_registration(api.registerSchemas, moduleHandle, registration, RegistrationStage::Schemas,
                                         a_assertContext);
        auto components = schemas ? call_registration(api.registerComponents, moduleHandle, registration,
                                                       RegistrationStage::Components, a_assertContext)
                                  : Result<void>::failure(std::move(*schemas.try_error()));
        auto registeredSystems = components ? call_registration(api.registerSystems, moduleHandle, registration,
                                                                 RegistrationStage::Systems, a_assertContext)
                                            : Result<void>::failure(std::move(*components.try_error()));
        if (!registeredSystems)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*registeredSystems.try_error()));
        }
        schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
        auto coreSchemas = runtime::add_runtime_schema_types(builder, a_assertContext);
        if (!coreSchemas)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*coreSchemas.try_error()));
        }
        auto registry = builder.seal();
        if (!registry)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*registry.try_error()));
        }
        auto systems = create_systems(moduleHandle, std::move(registration.systems), a_assertContext);
        if (!systems)
        {
            return Result<LoadedRuntimePackage>::failure(std::move(*systems.try_error()));
        }
        return Result<LoadedRuntimePackage>::success(LoadedRuntimePackage(
            root.generic_string(), std::string(manifest.try_value()->project_id()), std::move(module),
            std::move(*registry.try_value()), std::move(*startupScene.try_value()),
            std::move(*systems.try_value())));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}
} // namespace cue::runtime_host
