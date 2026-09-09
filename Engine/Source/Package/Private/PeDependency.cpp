#include <Cue/Package/Manifest.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Package/Error.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::uint16_t k_amd64Machine = 0x8664U;
constexpr std::uint16_t k_pe32PlusMagic = 0x020bU;
constexpr std::size_t k_sectionHeaderBytes = 40U;
constexpr std::size_t k_importDescriptorBytes = 20U;
constexpr std::size_t k_delayImportDescriptorBytes = 32U;
constexpr std::size_t k_exportDirectoryBytes = 40U;
constexpr std::size_t k_maximumSectionCount = 96U;
constexpr std::size_t k_maximumImportDescriptors = 512U;
constexpr std::size_t k_maximumImportThunkEntries = 65536U;
constexpr std::size_t k_maximumImportNameBytes = 1024U;

struct PeDirectory final
{
    std::uint32_t rva = 0U;
    std::uint32_t size = 0U;
};

struct PeLayout final
{
    std::span<const std::byte> bytes;
    std::size_t sectionTableOffset = 0U;
    std::uint16_t sectionCount = 0U;
    std::uint32_t sizeOfHeaders = 0U;
    PeDirectory exportDirectory;
    PeDirectory importDirectory;
    PeDirectory delayImportDirectory;
};

struct ParsedPeImage final
{
    std::vector<std::string> imports;
    bool hasExportForwarder = false;
};

/// @brief PE解析中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_pe_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Unexpected exception escaped PE dependency validation");
    std::abort();
}

/// @brief Little-endian 16-bit値を境界内だけ読み込む
[[nodiscard]] bool read_u16(std::span<const std::byte> a_bytes, std::size_t a_offset,
                            std::uint16_t &a_output) noexcept
{
    if (a_offset > a_bytes.size() || a_bytes.size() - a_offset < 2U)
    {
        return false;
    }
    a_output = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(a_bytes[a_offset])) |
               static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(a_bytes[a_offset + 1U]) << 8U);
    return true;
}

/// @brief Little-endian 32-bit値を境界内だけ読み込む
[[nodiscard]] bool read_u32(std::span<const std::byte> a_bytes, std::size_t a_offset,
                            std::uint32_t &a_output) noexcept
{
    if (a_offset > a_bytes.size() || a_bytes.size() - a_offset < 4U)
    {
        return false;
    }
    a_output = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(a_bytes[a_offset])) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(a_bytes[a_offset + 1U])) << 8U) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(a_bytes[a_offset + 2U])) << 16U) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(a_bytes[a_offset + 3U])) << 24U);
    return true;
}

/// @brief Little-endian 64-bit値を境界内だけ読み込む
[[nodiscard]] bool read_u64(std::span<const std::byte> a_bytes, std::size_t a_offset,
                            std::uint64_t &a_output) noexcept
{
    std::uint32_t lower = 0U;
    std::uint32_t upper = 0U;
    if (!read_u32(a_bytes, a_offset, lower) || !read_u32(a_bytes, a_offset + 4U, upper))
    {
        return false;
    }
    a_output = static_cast<std::uint64_t>(lower) | (static_cast<std::uint64_t>(upper) << 32U);
    return true;
}

/// @brief PE Optional HeaderのData Directory一件を読み込む
[[nodiscard]] bool read_directory(std::span<const std::byte> a_bytes, std::size_t a_optionalOffset,
                                  std::size_t a_index, PeDirectory &a_output) noexcept
{
    const std::size_t offset = a_optionalOffset + 112U + a_index * 8U;
    return read_u32(a_bytes, offset, a_output.rva) && read_u32(a_bytes, offset + 4U, a_output.size) &&
           ((a_output.rva == 0U) == (a_output.size == 0U));
}

/// @brief x64 PE HeaderとSection Tableの境界を検証してLayoutを返す
[[nodiscard]] bool parse_layout(std::span<const std::byte> a_bytes, PeLayout &a_output) noexcept
{
    std::uint16_t dosSignature = 0U;
    std::uint32_t peOffset32 = 0U;
    if (!read_u16(a_bytes, 0U, dosSignature) || dosSignature != 0x5a4dU ||
        !read_u32(a_bytes, 0x3cU, peOffset32))
    {
        return false;
    }
    const std::size_t peOffset = peOffset32;
    std::uint32_t peSignature = 0U;
    std::uint16_t machine = 0U;
    std::uint16_t sectionCount = 0U;
    std::uint16_t optionalSize = 0U;
    if (!read_u32(a_bytes, peOffset, peSignature) || peSignature != 0x00004550U ||
        !read_u16(a_bytes, peOffset + 4U, machine) || machine != k_amd64Machine ||
        !read_u16(a_bytes, peOffset + 6U, sectionCount) || sectionCount == 0U ||
        sectionCount > k_maximumSectionCount ||
        !read_u16(a_bytes, peOffset + 20U, optionalSize) || optionalSize < 224U)
    {
        return false;
    }
    const std::size_t optionalOffset = peOffset + 24U;
    std::uint16_t magic = 0U;
    std::uint32_t sizeOfHeaders = 0U;
    std::uint32_t directoryCount = 0U;
    if (!read_u16(a_bytes, optionalOffset, magic) || magic != k_pe32PlusMagic ||
        !read_u32(a_bytes, optionalOffset + 60U, sizeOfHeaders) || sizeOfHeaders > a_bytes.size() ||
        !read_u32(a_bytes, optionalOffset + 108U, directoryCount) || directoryCount < 14U)
    {
        return false;
    }
    const std::size_t sectionTableOffset = optionalOffset + optionalSize;
    if (sectionTableOffset > a_bytes.size() ||
        sectionCount > (a_bytes.size() - sectionTableOffset) / k_sectionHeaderBytes)
    {
        return false;
    }
    PeDirectory exportDirectory;
    PeDirectory importDirectory;
    PeDirectory delayImportDirectory;
    if (!read_directory(a_bytes, optionalOffset, 0U, exportDirectory) ||
        !read_directory(a_bytes, optionalOffset, 1U, importDirectory) ||
        !read_directory(a_bytes, optionalOffset, 13U, delayImportDirectory))
    {
        return false;
    }
    a_output = {a_bytes, sectionTableOffset, sectionCount, sizeOfHeaders, exportDirectory, importDirectory,
                delayImportDirectory};
    return true;
}

/// @brief RVA範囲をFile Offsetへ変換しVirtual-only領域とOverflowを拒否する
[[nodiscard]] std::optional<std::size_t> rva_to_offset(const PeLayout &a_layout, std::uint32_t a_rva,
                                                       std::size_t a_requiredBytes) noexcept
{
    if (a_rva < a_layout.sizeOfHeaders && a_rva <= a_layout.bytes.size() &&
        a_requiredBytes <= a_layout.bytes.size() - a_rva)
    {
        return static_cast<std::size_t>(a_rva);
    }
    for (std::size_t index = 0U; index < a_layout.sectionCount; ++index)
    {
        const std::size_t section = a_layout.sectionTableOffset + index * k_sectionHeaderBytes;
        std::uint32_t virtualAddress = 0U;
        std::uint32_t rawSize = 0U;
        std::uint32_t rawOffset = 0U;
        if (!read_u32(a_layout.bytes, section + 12U, virtualAddress) ||
            !read_u32(a_layout.bytes, section + 16U, rawSize) ||
            !read_u32(a_layout.bytes, section + 20U, rawOffset))
        {
            return std::nullopt;
        }
        if (a_rva < virtualAddress)
        {
            continue;
        }
        const std::uint64_t delta = static_cast<std::uint64_t>(a_rva) - virtualAddress;
        if (delta > rawSize || a_requiredBytes > static_cast<std::uint64_t>(rawSize) - delta)
        {
            continue;
        }
        const std::uint64_t fileOffset = static_cast<std::uint64_t>(rawOffset) + delta;
        if (fileOffset <= a_layout.bytes.size() && a_requiredBytes <= a_layout.bytes.size() - fileOffset)
        {
            return static_cast<std::size_t>(fileOffset);
        }
    }
    return std::nullopt;
}

/// @brief ASCII DLL Import名をRVAから上限付きで読み込む
[[nodiscard]] bool read_import_name(const PeLayout &a_layout, std::uint32_t a_rva, std::string &a_output)
{
    a_output.clear();
    for (std::size_t index = 0U; index <= k_maximumImportNameBytes; ++index)
    {
        if (a_rva > (std::numeric_limits<std::uint32_t>::max)() - index)
        {
            return false;
        }
        const auto offset = rva_to_offset(a_layout, a_rva + static_cast<std::uint32_t>(index), 1U);
        if (!offset)
        {
            return false;
        }
        const unsigned char value = std::to_integer<unsigned char>(a_layout.bytes[*offset]);
        if (value == 0U)
        {
            return !a_output.empty();
        }
        if (value < 0x21U || value > 0x7eU || value == '/' || value == '\\' || value == ':')
        {
            return false;
        }
        a_output.push_back(static_cast<char>(value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value));
    }
    return false;
}

/// @brief PE32+ Import Thunk TableがImage内でNUL終端されることを検証する
[[nodiscard]] bool validate_import_thunk_table(const PeLayout &a_layout, std::uint32_t a_tableRva) noexcept
{
    if (a_tableRva == 0U)
    {
        return false;
    }
    for (std::size_t index = 0U; index < k_maximumImportThunkEntries; ++index)
    {
        const std::uint64_t delta = index * 8ULL;
        if (delta > (std::numeric_limits<std::uint32_t>::max)() - a_tableRva)
        {
            return false;
        }
        const auto offset = rva_to_offset(a_layout, a_tableRva + static_cast<std::uint32_t>(delta), 8U);
        std::uint64_t thunk = 0U;
        if (!offset || !read_u64(a_layout.bytes, *offset, thunk))
        {
            return false;
        }
        if (thunk == 0U)
        {
            return true;
        }
    }
    return false;
}

/// @brief 通常Import DirectoryのDLL名を列挙する
[[nodiscard]] bool append_import_directory(const PeLayout &a_layout, std::vector<std::string> &a_imports)
{
    if (a_layout.importDirectory.rva == 0U)
    {
        return true;
    }
    for (std::size_t index = 0U; index < k_maximumImportDescriptors; ++index)
    {
        const std::uint64_t delta = index * k_importDescriptorBytes;
        if (delta + k_importDescriptorBytes > a_layout.importDirectory.size ||
            delta > (std::numeric_limits<std::uint32_t>::max)() - a_layout.importDirectory.rva)
        {
            return false;
        }
        const auto offset = rva_to_offset(a_layout, a_layout.importDirectory.rva + static_cast<std::uint32_t>(delta),
                                          k_importDescriptorBytes);
        if (!offset)
        {
            return false;
        }
        std::array<std::uint32_t, 5U> fields{};
        for (std::size_t field = 0U; field < fields.size(); ++field)
        {
            if (!read_u32(a_layout.bytes, *offset + field * 4U, fields[field]))
            {
                return false;
            }
        }
        if (std::ranges::all_of(fields, [](std::uint32_t a_value) noexcept { return a_value == 0U; }))
        {
            return true;
        }
        const std::uint32_t lookupTableRva = fields[0] != 0U ? fields[0] : fields[4];
        std::string name;
        if (fields[3] == 0U || fields[4] == 0U || !validate_import_thunk_table(a_layout, lookupTableRva) ||
            (lookupTableRva != fields[4] && !validate_import_thunk_table(a_layout, fields[4])) ||
            !read_import_name(a_layout, fields[3], name))
        {
            return false;
        }
        a_imports.push_back(std::move(name));
    }
    return false;
}

/// @brief Delay-load Import DirectoryのDLL名を列挙する
[[nodiscard]] bool append_delay_import_directory(const PeLayout &a_layout, std::vector<std::string> &a_imports)
{
    if (a_layout.delayImportDirectory.rva == 0U)
    {
        return true;
    }
    for (std::size_t index = 0U; index < k_maximumImportDescriptors; ++index)
    {
        const std::uint64_t delta = index * k_delayImportDescriptorBytes;
        if (delta + k_delayImportDescriptorBytes > a_layout.delayImportDirectory.size ||
            delta > (std::numeric_limits<std::uint32_t>::max)() - a_layout.delayImportDirectory.rva)
        {
            return false;
        }
        const auto offset = rva_to_offset(
            a_layout, a_layout.delayImportDirectory.rva + static_cast<std::uint32_t>(delta),
            k_delayImportDescriptorBytes);
        if (!offset)
        {
            return false;
        }
        std::array<std::uint32_t, 8U> fields{};
        for (std::size_t field = 0U; field < fields.size(); ++field)
        {
            if (!read_u32(a_layout.bytes, *offset + field * 4U, fields[field]))
            {
                return false;
            }
        }
        if (std::ranges::all_of(fields, [](std::uint32_t a_value) noexcept { return a_value == 0U; }))
        {
            return true;
        }
        std::string name;
        if (fields[0] != 1U || fields[1] == 0U || fields[2] == 0U ||
            !rva_to_offset(a_layout, fields[2], 8U) || fields[3] == 0U || fields[4] == 0U ||
            !validate_import_thunk_table(a_layout, fields[3]) ||
            !validate_import_thunk_table(a_layout, fields[4]) ||
            (fields[5] != 0U && !validate_import_thunk_table(a_layout, fields[5])) ||
            (fields[6] != 0U && !validate_import_thunk_table(a_layout, fields[6])) ||
            !read_import_name(a_layout, fields[1], name))
        {
            return false;
        }
        a_imports.push_back(std::move(name));
    }
    return false;
}

/// @brief Export Address Table内にForwarder RVAが存在するか検査する
[[nodiscard]] bool inspect_export_forwarders(const PeLayout &a_layout, bool &a_hasForwarder) noexcept
{
    a_hasForwarder = false;
    if (a_layout.exportDirectory.rva == 0U)
    {
        return true;
    }
    if (a_layout.exportDirectory.size < k_exportDirectoryBytes)
    {
        return false;
    }
    const auto directoryOffset = rva_to_offset(a_layout, a_layout.exportDirectory.rva, k_exportDirectoryBytes);
    std::uint32_t functionCount = 0U;
    std::uint32_t functionTableRva = 0U;
    if (!directoryOffset || !read_u32(a_layout.bytes, *directoryOffset + 20U, functionCount) ||
        !read_u32(a_layout.bytes, *directoryOffset + 28U, functionTableRva) ||
        functionCount > 65536U)
    {
        return false;
    }
    if (functionCount == 0U)
    {
        return true;
    }
    const auto functionTableOffset = rva_to_offset(a_layout, functionTableRva, functionCount * 4ULL);
    if (!functionTableOffset)
    {
        return false;
    }
    const std::uint64_t directoryEnd = static_cast<std::uint64_t>(a_layout.exportDirectory.rva) +
                                       static_cast<std::uint64_t>(a_layout.exportDirectory.size);
    if (directoryEnd > (std::numeric_limits<std::uint32_t>::max)() + 1ULL)
    {
        return false;
    }
    for (std::size_t index = 0U; index < functionCount; ++index)
    {
        std::uint32_t functionRva = 0U;
        if (!read_u32(a_layout.bytes, *functionTableOffset + index * 4U, functionRva))
        {
            return false;
        }
        if (functionRva >= a_layout.exportDirectory.rva && functionRva < directoryEnd)
        {
            a_hasForwarder = true;
            return true;
        }
    }
    return true;
}

/// @brief x64 PEから通常／Delay ImportとForwarder有無を抽出する
[[nodiscard]] bool parse_pe_image(std::span<const std::byte> a_bytes, ParsedPeImage &a_output)
{
    PeLayout layout;
    if (!parse_layout(a_bytes, layout) || !append_import_directory(layout, a_output.imports) ||
        !append_delay_import_directory(layout, a_output.imports) ||
        !inspect_export_forwarders(layout, a_output.hasExportForwarder))
    {
        return false;
    }
    std::sort(a_output.imports.begin(), a_output.imports.end());
    a_output.imports.erase(std::unique(a_output.imports.begin(), a_output.imports.end()), a_output.imports.end());
    return true;
}

/// @brief File名をASCII case-insensitive比較Keyへ変換する
[[nodiscard]] std::string ascii_lower(std::string_view a_value)
{
    std::string output(a_value);
    for (char &value : output)
    {
        if (value >= 'A' && value <= 'Z')
        {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return output;
}

/// @brief Runtime Dependencyへ許可する単一DLL File名か返す
[[nodiscard]] bool is_dll_file_name(std::string_view a_value) noexcept
{
    if (a_value.size() <= 4U || a_value.find_first_of("/\\:") != std::string_view::npos)
    {
        return false;
    }
    const std::size_t suffix = a_value.size() - 4U;
    return (a_value[suffix] == '.') && (a_value[suffix + 1U] == 'd' || a_value[suffix + 1U] == 'D') &&
           (a_value[suffix + 2U] == 'l' || a_value[suffix + 2U] == 'L') &&
           (a_value[suffix + 3U] == 'l' || a_value[suffix + 3U] == 'L');
}

/// @brief Version付きWindows System Import Allowlistに含まれるか返す
[[nodiscard]] bool is_system_import(std::string_view a_name) noexcept
{
    constexpr std::array allowlist = {
        std::string_view("bcrypt.dll"),
        std::string_view("d3d12.dll"),
        std::string_view("dxgi.dll"),
        std::string_view("kernel32.dll"),
        std::string_view("ole32.dll"),
        std::string_view("shell32.dll"),
        std::string_view("user32.dll"),
        std::string_view("api-ms-win-crt-filesystem-l1-1-0.dll"),
        std::string_view("api-ms-win-crt-heap-l1-1-0.dll"),
        std::string_view("api-ms-win-crt-locale-l1-1-0.dll"),
        std::string_view("api-ms-win-crt-math-l1-1-0.dll"),
        std::string_view("api-ms-win-crt-runtime-l1-1-0.dll"),
        std::string_view("api-ms-win-crt-stdio-l1-1-0.dll"),
        std::string_view("api-ms-win-crt-string-l1-1-0.dll")};
    return std::ranges::find(allowlist, a_name) != allowlist.end();
}

/// @brief 選択Build ConfigurationのMSVC Runtime Allowlistに含まれるか返す
[[nodiscard]] bool is_configuration_msvc_import(cue::BuildConfiguration a_configuration,
                                                 std::string_view a_name) noexcept
{
    constexpr std::array nonDebug = {std::string_view("msvcp140.dll"), std::string_view("vcruntime140.dll"),
                                     std::string_view("vcruntime140_1.dll")};
    constexpr std::array debug = {std::string_view("msvcp140d.dll"), std::string_view("vcruntime140d.dll"),
                                  std::string_view("vcruntime140_1d.dll"), std::string_view("ucrtbased.dll")};
    const bool isDebug = a_configuration == cue::BuildConfiguration::Debug;
    return isDebug ? std::ranges::find(debug, a_name) != debug.end()
                   : std::ranges::find(nonDebug, a_name) != nonDebug.end();
}

/// @brief いずれかの構成専用MSVC Runtime名か返す
[[nodiscard]] bool is_known_msvc_import(std::string_view a_name) noexcept
{
    constexpr std::array names = {std::string_view("msvcp140.dll"),      std::string_view("vcruntime140.dll"),
                                  std::string_view("vcruntime140_1.dll"), std::string_view("msvcp140d.dll"),
                                  std::string_view("vcruntime140d.dll"),  std::string_view("vcruntime140_1d.dll"),
                                  std::string_view("ucrtbased.dll")};
    return std::ranges::find(names, a_name) != names.end();
}

/// @brief Configuration列挙値が公開3構成のいずれかか返す
[[nodiscard]] bool is_valid_configuration(cue::BuildConfiguration a_configuration) noexcept
{
    return a_configuration == cue::BuildConfiguration::Debug ||
           a_configuration == cue::BuildConfiguration::Development ||
           a_configuration == cue::BuildConfiguration::Release;
}
} // namespace

namespace cue::package
{
Result<void> validate_runtime_dependency_closure(BuildConfiguration a_configuration,
                                                 RuntimePeImageView a_runtimeHost,
                                                 RuntimePeImageView a_gameModule,
                                                 std::span<const RuntimePeImageView> a_appLocalDependencies,
                                                 const AssertContext &a_assertContext) noexcept
{
    if (!is_valid_configuration(a_configuration) || ascii_lower(a_runtimeHost.fileName) != "cueruntimehost.exe" ||
        ascii_lower(a_gameModule.fileName) != "cuegamemodule.dll" ||
        a_appLocalDependencies.size() > k_maximumPackageFileEntries - k_requiredPackageFileEntries)
    {
        return Result<void>::failure(make_package_error(
            a_assertContext, PackageError::RuntimeDependencyViolation,
            "Runtime dependency closure input identity or configuration is invalid"));
    }
    try
    {
        ParsedPeImage host;
        ParsedPeImage game;
        if (!parse_pe_image(a_runtimeHost.bytes, host) || !parse_pe_image(a_gameModule.bytes, game))
        {
            return Result<void>::failure(make_package_error(
                a_assertContext, PackageError::InvalidPortableExecutable,
                "Runtime Host or Game Module is not a bounded x64 PE image"));
        }
        if (host.hasExportForwarder || game.hasExportForwarder)
        {
            return Result<void>::failure(make_package_error(
                a_assertContext, PackageError::RuntimeDependencyViolation,
                "Runtime Host or Game Module contains an unsupported export forwarder"));
        }
        for (const std::string &name : host.imports)
        {
            if (!is_system_import(name) && !is_configuration_msvc_import(a_configuration, name))
            {
                return Result<void>::failure(make_package_error(
                    a_assertContext, PackageError::RuntimeDependencyViolation,
                    "Runtime Host imports a DLL outside the fixed system and MSVC allowlists"));
            }
        }

        std::vector<std::string> dependencyNames;
        std::vector<ParsedPeImage> parsedDependencies;
        dependencyNames.reserve(a_appLocalDependencies.size());
        parsedDependencies.reserve(a_appLocalDependencies.size());
        for (const RuntimePeImageView dependency : a_appLocalDependencies)
        {
            const std::string name = ascii_lower(dependency.fileName);
            if (!is_dll_file_name(dependency.fileName) ||
                std::ranges::find(dependencyNames, name) != dependencyNames.end())
            {
                return Result<void>::failure(make_package_error(
                    a_assertContext, PackageError::RuntimeDependencyViolation,
                    "App-local Runtime dependency names are invalid or case aliases"));
            }
            ParsedPeImage parsed;
            if (!parse_pe_image(dependency.bytes, parsed))
            {
                return Result<void>::failure(make_package_error(
                    a_assertContext, PackageError::InvalidPortableExecutable,
                    "App-local Runtime dependency is not a bounded x64 PE image"));
            }
            if (parsed.hasExportForwarder)
            {
                return Result<void>::failure(make_package_error(
                    a_assertContext, PackageError::RuntimeDependencyViolation,
                    "App-local Runtime dependency contains an unsupported export forwarder"));
            }
            dependencyNames.push_back(name);
            parsedDependencies.push_back(std::move(parsed));
        }

        std::vector<bool> reached(a_appLocalDependencies.size(), false);
        std::vector<std::size_t> pending;
        pending.reserve(a_appLocalDependencies.size());
        /// @brief 一つのPE Import集合をAllowlistまたは登録Dependencyへ解決する
        const auto resolveImports = [&](const std::vector<std::string> &a_imports) noexcept
        {
            for (const std::string &name : a_imports)
            {
                if (is_system_import(name) || is_configuration_msvc_import(a_configuration, name))
                {
                    continue;
                }
                if (is_known_msvc_import(name))
                {
                    return false;
                }
                const auto dependency = std::ranges::find(dependencyNames, name);
                if (dependency == dependencyNames.end())
                {
                    return false;
                }
                const std::size_t index = static_cast<std::size_t>(dependency - dependencyNames.begin());
                if (!reached[index])
                {
                    reached[index] = true;
                    pending.push_back(index);
                }
            }
            return true;
        };
        if (!resolveImports(game.imports))
        {
            return Result<void>::failure(make_package_error(
                a_assertContext, PackageError::RuntimeDependencyViolation,
                "Game Module has an unregistered or configuration-mismatched import"));
        }
        for (std::size_t cursor = 0U; cursor < pending.size(); ++cursor)
        {
            if (!resolveImports(parsedDependencies[pending[cursor]].imports))
            {
                return Result<void>::failure(make_package_error(
                    a_assertContext, PackageError::RuntimeDependencyViolation,
                    "App-local Runtime dependency closure contains an unresolved import"));
            }
        }
        if (std::ranges::find(reached, false) != reached.end())
        {
            return Result<void>::failure(make_package_error(
                a_assertContext, PackageError::RuntimeDependencyViolation,
                "A registered App-local Runtime dependency is unreachable from the Game Module"));
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_pe_exception(a_assertContext);
    }
}
} // namespace cue::package
