#include <Cue/Build/Windows/WindowsProductSecurity.h>

#include "WindowsProductSecurityInternal.h"

#include <Cue/Build/Windows/WindowsArtifactPublisher.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>

#include <Windows.h>

#include <Softpub.h>
#include <Wincrypt.h>
#include <Wintrust.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::uint16_t k_requiredDllCharacteristics =
    IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA | IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
    IMAGE_DLLCHARACTERISTICS_NX_COMPAT | IMAGE_DLLCHARACTERISTICS_GUARD_CF;
constexpr std::uint16_t k_requiredDependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32;
constexpr std::uint32_t k_cetCompatible = IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT;
constexpr DWORD k_writableDataSection = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
constexpr DWORD k_readOnlyDataSection = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
constexpr DWORD k_executableCodeSection = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
constexpr std::uint64_t k_maximumProductBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_maximumImportNameBytes = 256U;
constexpr std::size_t k_maximumImportDescriptors = 256U;
constexpr std::size_t k_maximumImportsPerLibrary = 65536U;

constexpr std::array<std::string_view, 15U> k_allowedImports = {"api-ms-win-crt-heap-l1-1-0.dll",
                                                                "api-ms-win-crt-locale-l1-1-0.dll",
                                                                "api-ms-win-crt-math-l1-1-0.dll",
                                                                "api-ms-win-crt-runtime-l1-1-0.dll",
                                                                "api-ms-win-crt-stdio-l1-1-0.dll",
                                                                "api-ms-win-crt-string-l1-1-0.dll",
                                                                "bcrypt.dll",
                                                                "d3d12.dll",
                                                                "dxgi.dll",
                                                                "kernel32.dll",
                                                                "msvcp140.dll",
                                                                "ucrtbase.dll",
                                                                "user32.dll",
                                                                "vcruntime140.dll",
                                                                "vcruntime140_1.dll"};

constexpr std::array<std::string_view, 8U> k_forbiddenLoaderImports = {
    "FreeLibrary",    "FreeLibraryAndExitThread", "GetProcAddress", "LoadLibraryA",
    "LoadLibraryExA", "LoadLibraryExW",           "LoadLibraryW",   "LoadPackagedLibrary"};

/// @brief PE Header検証後に記録する正規化済みDirect Import一覧
struct PeSecurityEvidence final
{
    std::vector<std::string> importedLibraries;
};

/// @brief RVAに対応するFile Offsetと同一領域内の連続Byte数を保持する
struct RvaFileRange final
{
    std::size_t offset;
    std::size_t size;
};

/// @brief Image内VAに対応するFile OffsetとSection属性を保持する
struct MappedImageFileRange final
{
    std::size_t offset;
    DWORD sectionCharacteristics;
};

/// @brief Product Security検証中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_security_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Windows shipping product security validation failed unexpectedly");
    std::abort();
}

/// @brief Windows Shipping Product固有の回復可能Errorを構築する
[[nodiscard]] cue::Error make_error(const cue::AssertContext &a_assertContext, cue::WindowsBuildArtifactError a_code,
                                    std::string_view a_summary) noexcept
{
    cue::ErrorCode code = cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Windows.Artifact",
                                                 static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief Windows Error Codeを含む回復可能Errorを構築する
[[nodiscard]] cue::Error make_windows_error(const cue::AssertContext &a_assertContext,
                                            cue::WindowsBuildArtifactError a_code, DWORD a_windowsCode,
                                            std::string_view a_summary)
{
    std::string message(a_summary);
    message.append(" (Win32=");
    message.append(std::to_string(a_windowsCode));
    message.push_back(')');
    return make_error(a_assertContext, a_code, message);
}

/// @brief Kernel Handleを一意所有する
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
    /// @brief 既存HandleをCloseして所有権を移動代入する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    /// @brief 所有HandleをCloseする
    ~UniqueHandle()
    {
        reset();
    }
    /// @brief Native Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }
    /// @brief 有効なNative Handleを所有しているか返す
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

  private:
    /// @brief 所有HandleがあればCloseして無効化する
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

/// @brief Read-only File Mapping Viewを一意所有する
class UniqueView final
{
  public:
    /// @brief 空Viewを構築する
    UniqueView() noexcept = default;
    /// @brief Mapping Viewの所有権を取得する
    explicit UniqueView(const void *a_view) noexcept : m_view(a_view)
    {
    }
    /// @brief Viewの複製を禁止する
    UniqueView(const UniqueView &) = delete;
    /// @brief Viewの複製代入を禁止する
    UniqueView &operator=(const UniqueView &) = delete;
    /// @brief View所有権を移動する
    UniqueView(UniqueView &&a_other) noexcept : m_view(std::exchange(a_other.m_view, nullptr))
    {
    }
    /// @brief 既存Viewを解放して所有権を移動代入する
    UniqueView &operator=(UniqueView &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_view = std::exchange(a_other.m_view, nullptr);
        }
        return *this;
    }
    /// @brief 所有Viewを解放する
    ~UniqueView()
    {
        reset();
    }
    /// @brief Mapping先頭を返す
    [[nodiscard]] const void *get() const noexcept
    {
        return m_view;
    }

  private:
    /// @brief 所有Viewがあれば解放する
    void reset() noexcept
    {
        if (m_view != nullptr)
        {
            UnmapViewOfFile(m_view);
        }
        m_view = nullptr;
    }

    const void *m_view = nullptr;
};

/// @brief Product File HandleとRead-only Mappingを同じ寿命で保持する
class MappedProduct final
{
  public:
    /// @brief Handle、Mapping、View、Size、Native Pathを所有する
    MappedProduct(UniqueHandle a_file, UniqueHandle a_mapping, UniqueView a_view, std::size_t a_size,
                  std::wstring a_nativePath) noexcept
        : m_file(std::move(a_file)), m_mapping(std::move(a_mapping)), m_view(std::move(a_view)), m_size(a_size),
          m_nativePath(std::move(a_nativePath))
    {
    }
    /// @brief Mappingの複製を禁止する
    MappedProduct(const MappedProduct &) = delete;
    /// @brief Mappingの複製代入を禁止する
    MappedProduct &operator=(const MappedProduct &) = delete;
    /// @brief Mapping所有権を移動する
    MappedProduct(MappedProduct &&) noexcept = default;
    /// @brief Mapping所有権を移動代入する
    MappedProduct &operator=(MappedProduct &&) noexcept = default;
    /// @brief View、Mapping、File Handleを順に解放する
    ~MappedProduct() = default;
    /// @brief Product Byte列を返す
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return {static_cast<const std::byte *>(m_view.get()), m_size};
    }
    /// @brief Product File Handleを返す
    [[nodiscard]] HANDLE file_handle() const noexcept
    {
        return m_file.get();
    }
    /// @brief WinTrustへ渡すExtended-length Pathを返す
    [[nodiscard]] const std::wstring &native_path() const noexcept
    {
        return m_nativePath;
    }

  private:
    UniqueHandle m_file;
    UniqueHandle m_mapping;
    UniqueView m_view;
    std::size_t m_size;
    std::wstring m_nativePath;
};

/// @brief Absolute Windows PathへExtended-length Prefixを付与する
[[nodiscard]] std::filesystem::path native_path(const std::filesystem::path &a_path)
{
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring &value = preferred.native();
    if (value.starts_with(L"\\\\?\\"))
    {
        return preferred;
    }
    if (value.starts_with(L"\\\\"))
    {
        std::wstring extended = L"\\\\?\\UNC\\";
        extended.append(value.substr(2U));
        return std::filesystem::path(std::move(extended));
    }
    std::wstring extended = L"\\\\?\\";
    extended.append(value);
    return std::filesystem::path(std::move(extended));
}

/// @brief Absolute Product FileをWrite／Delete共有なしで開きRead-only Mappingする
[[nodiscard]] cue::Result<MappedProduct> open_product(std::string_view a_path,
                                                      const cue::AssertContext &a_assertContext)
{
    std::filesystem::path path(std::u8string_view(reinterpret_cast<const char8_t *>(a_path.data()), a_path.size()));
    path = path.lexically_normal();
    if (!path.is_absolute())
    {
        return cue::Result<MappedProduct>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product security validation requires an absolute path"));
    }
    std::filesystem::path inspected = native_path(path);
    UniqueHandle file(CreateFileW(inspected.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.is_valid())
    {
        return cue::Result<MappedProduct>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation, GetLastError(),
                               "Shipping Product could not be opened for security validation"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file.get(), &information) == FALSE ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
    {
        return cue::Result<MappedProduct>::failure(make_error(a_assertContext,
                                                              cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                              "Shipping Product must be a regular non-reparse file"));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > k_maximumProductBytes ||
        static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max())
    {
        return cue::Result<MappedProduct>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product size is invalid or exceeds the 512 MiB policy limit"));
    }
    UniqueHandle mapping(CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0U, 0U, nullptr));
    if (!mapping.is_valid())
    {
        return cue::Result<MappedProduct>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation, GetLastError(),
                               "Shipping Product mapping could not be created"));
    }
    UniqueView view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0U, 0U, 0U));
    if (view.get() == nullptr)
    {
        return cue::Result<MappedProduct>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation, GetLastError(),
                               "Shipping Product mapping could not be read"));
    }
    return cue::Result<MappedProduct>::success(MappedProduct(std::move(file), std::move(mapping), std::move(view),
                                                             static_cast<std::size_t>(size.QuadPart),
                                                             std::move(inspected.native())));
}

/// @brief 範囲検証後にPE構造体をAlignment非依存でCopyする
template <typename Value>
[[nodiscard]] std::optional<Value> read_value(std::span<const std::byte> a_bytes, std::size_t a_offset) noexcept
{
    if (a_offset > a_bytes.size() || sizeof(Value) > a_bytes.size() - a_offset)
    {
        return std::nullopt;
    }
    Value value{};
    std::memcpy(&value, a_bytes.data() + a_offset, sizeof(Value));
    return value;
}

/// @brief PE RVAをHeaderまたはSection内に限定した連続File範囲へ変換する
[[nodiscard]] std::optional<RvaFileRange> rva_to_file_range(std::uint32_t a_rva,
                                                            const IMAGE_OPTIONAL_HEADER64 &a_optional,
                                                            std::span<const IMAGE_SECTION_HEADER> a_sections,
                                                            std::size_t a_fileSize) noexcept
{
    const std::uint64_t rva = a_rva;
    if (rva < a_optional.SizeOfHeaders)
    {
        const std::uint64_t headerEnd = std::min<std::uint64_t>(a_optional.SizeOfHeaders, a_fileSize);
        if (rva < headerEnd)
        {
            return RvaFileRange{static_cast<std::size_t>(rva), static_cast<std::size_t>(headerEnd - rva)};
        }
        return std::nullopt;
    }
    for (const IMAGE_SECTION_HEADER &section : a_sections)
    {
        const std::uint64_t start = section.VirtualAddress;
        const std::uint64_t extent = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva < start || rva - start >= extent)
        {
            continue;
        }
        const std::uint64_t delta = rva - start;
        if (delta >= section.SizeOfRawData)
        {
            return std::nullopt;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(section.PointerToRawData) + delta;
        if (offset >= a_fileSize)
        {
            return std::nullopt;
        }
        const std::uint64_t rawAvailable = section.SizeOfRawData - delta;
        const std::uint64_t fileAvailable = a_fileSize - offset;
        return RvaFileRange{static_cast<std::size_t>(offset),
                            static_cast<std::size_t>(std::min(rawAvailable, fileAvailable))};
    }
    return std::nullopt;
}

/// @brief PE RVA範囲を検証済みFile Offsetへ変換する
[[nodiscard]] std::optional<std::size_t> rva_to_offset(std::uint32_t a_rva, std::size_t a_size,
                                                       const IMAGE_OPTIONAL_HEADER64 &a_optional,
                                                       std::span<const IMAGE_SECTION_HEADER> a_sections,
                                                       std::size_t a_fileSize) noexcept
{
    const std::optional<RvaFileRange> range = rva_to_file_range(a_rva, a_optional, a_sections, a_fileSize);
    return range && a_size <= range->size ? std::optional<std::size_t>(range->offset) : std::nullopt;
}

/// @brief PE内VAをImage範囲とFile-backed Sectionに限定したFile位置へ変換する
[[nodiscard]] std::optional<MappedImageFileRange> mapped_image_va_range(
    ULONGLONG a_va, std::size_t a_size, const IMAGE_OPTIONAL_HEADER64 &a_optional,
    std::span<const IMAGE_SECTION_HEADER> a_sections, std::size_t a_fileSize) noexcept
{
    if (a_va < a_optional.ImageBase)
    {
        return std::nullopt;
    }
    const std::uint64_t rva = a_va - a_optional.ImageBase;
    if (rva > std::numeric_limits<std::uint32_t>::max() || rva >= a_optional.SizeOfImage ||
        a_size > static_cast<std::uint64_t>(a_optional.SizeOfImage) - rva)
    {
        return std::nullopt;
    }
    for (const IMAGE_SECTION_HEADER &section : a_sections)
    {
        const std::uint64_t sectionRva = section.VirtualAddress;
        const std::uint64_t mappedSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva < sectionRva || rva - sectionRva >= mappedSize)
        {
            continue;
        }
        const std::uint64_t delta = rva - sectionRva;
        if (delta > section.SizeOfRawData || a_size > static_cast<std::uint64_t>(section.SizeOfRawData) - delta)
        {
            return std::nullopt;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(section.PointerToRawData) + delta;
        if (offset > a_fileSize || a_size > static_cast<std::uint64_t>(a_fileSize) - offset)
        {
            return std::nullopt;
        }
        return MappedImageFileRange{static_cast<std::size_t>(offset), section.Characteristics};
    }
    return std::nullopt;
}

/// @brief Mapped Sectionが要求属性を全て持ち禁止属性を一つも持たないか判定する
[[nodiscard]] bool has_section_characteristics(const MappedImageFileRange &a_range, DWORD a_required,
                                               DWORD a_forbidden) noexcept
{
    return (a_range.sectionCharacteristics & a_required) == a_required &&
           (a_range.sectionCharacteristics & a_forbidden) == 0U;
}

/// @brief Base Relocation Directoryの範囲、Block、x64 Relocation Entryを検証する
[[nodiscard]] cue::Result<std::vector<std::uint32_t>> validate_base_relocations(
    std::span<const std::byte> a_bytes, const IMAGE_OPTIONAL_HEADER64 &a_optional,
    std::span<const IMAGE_SECTION_HEADER> a_sections, const cue::AssertContext &a_assertContext) noexcept
{
    const IMAGE_DATA_DIRECTORY &directory = a_optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0U || directory.Size < sizeof(IMAGE_BASE_RELOCATION))
    {
        return cue::Result<std::vector<std::uint32_t>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product base relocation directory is missing"));
    }
    const std::optional<std::size_t> directoryOffset =
        rva_to_offset(directory.VirtualAddress, directory.Size, a_optional, a_sections, a_bytes.size());
    if (!directoryOffset)
    {
        return cue::Result<std::vector<std::uint32_t>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product base relocation directory is outside the PE image"));
    }

    std::size_t cursor = 0U;
    std::vector<std::uint32_t> relocatedImagePointers;
    while (cursor < directory.Size)
    {
        const std::size_t remaining = directory.Size - cursor;
        const std::optional<IMAGE_BASE_RELOCATION> block =
            read_value<IMAGE_BASE_RELOCATION>(a_bytes, *directoryOffset + cursor);
        if (!block || remaining < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > remaining ||
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(std::uint16_t) != 0U)
        {
            return cue::Result<std::vector<std::uint32_t>>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                           "Shipping Product base relocation block is malformed"));
        }
        const std::size_t entryCount = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(std::uint16_t);
        for (std::size_t entryIndex = 0U; entryIndex < entryCount; ++entryIndex)
        {
            const std::optional<std::uint16_t> entry =
                read_value<std::uint16_t>(a_bytes, *directoryOffset + cursor + sizeof(IMAGE_BASE_RELOCATION) +
                                                       entryIndex * sizeof(std::uint16_t));
            if (!entry)
            {
                return cue::Result<std::vector<std::uint32_t>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                               "Shipping Product base relocation entry is truncated"));
            }
            const std::uint16_t type = *entry >> 12U;
            if (type == IMAGE_REL_BASED_ABSOLUTE)
            {
                continue;
            }
            if (type != IMAGE_REL_BASED_DIR64)
            {
                return cue::Result<std::vector<std::uint32_t>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                               "Shipping Product uses a base relocation type outside the x64 policy"));
            }
            const std::uint64_t targetRva = static_cast<std::uint64_t>(block->VirtualAddress) + (*entry & 0x0fffU);
            if (targetRva > std::numeric_limits<std::uint32_t>::max() ||
                !rva_to_offset(static_cast<std::uint32_t>(targetRva), sizeof(std::uint64_t), a_optional, a_sections,
                               a_bytes.size()))
            {
                return cue::Result<std::vector<std::uint32_t>>::failure(
                    make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                               "Shipping Product base relocation target is outside the PE image"));
            }
            relocatedImagePointers.push_back(static_cast<std::uint32_t>(targetRva));
        }
        cursor += block->SizeOfBlock;
    }
    if (relocatedImagePointers.empty())
    {
        return cue::Result<std::vector<std::uint32_t>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product base relocation directory has no x64 relocation entry"));
    }
    std::ranges::sort(relocatedImagePointers);
    for (std::size_t index = 1U; index < relocatedImagePointers.size(); ++index)
    {
        const std::uint64_t previousTarget = relocatedImagePointers[index - 1U];
        const std::uint64_t currentTarget = relocatedImagePointers[index];
        if (currentTarget < previousTarget + sizeof(std::uint64_t))
        {
            return cue::Result<std::vector<std::uint32_t>>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                           "Shipping Product base relocation directory contains overlapping DIR64 targets"));
        }
    }
    return cue::Result<std::vector<std::uint32_t>>::success(std::move(relocatedImagePointers));
}

/// @brief Bounded ASCII NUL終端文字列をPE Byte列から借用する
[[nodiscard]] std::optional<std::string_view> read_ascii_string(std::span<const std::byte> a_bytes,
                                                                std::size_t a_offset, std::size_t a_available) noexcept
{
    if (a_offset >= a_bytes.size() || a_available > a_bytes.size() - a_offset)
    {
        return std::nullopt;
    }
    const std::size_t available = std::min(k_maximumImportNameBytes, a_available);
    const char *text = reinterpret_cast<const char *>(a_bytes.data() + a_offset);
    for (std::size_t index = 0U; index < available; ++index)
    {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        if (value == 0U)
        {
            return std::string_view(text, index);
        }
        if (value < 0x20U || value > 0x7EU)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/// @brief ASCII Import名をCase-insensitive比較用lowercaseへ変換する
[[nodiscard]] std::string lowercase_ascii(std::string_view a_value)
{
    std::string result;
    result.reserve(a_value.size());
    for (const char value : a_value)
    {
        if (value >= 'A' && value <= 'Z')
        {
            result.push_back(static_cast<char>(value - 'A' + 'a'));
        }
        else
        {
            result.push_back(value);
        }
    }
    return result;
}

/// @brief 一LibraryのImport名とGame Module Loader API不在を検証する
[[nodiscard]] cue::Result<void> validate_import_functions(std::span<const std::byte> a_bytes, std::uint32_t a_thunkRva,
                                                          const IMAGE_OPTIONAL_HEADER64 &a_optional,
                                                          std::span<const IMAGE_SECTION_HEADER> a_sections,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    for (std::size_t index = 0U; index < k_maximumImportsPerLibrary; ++index)
    {
        const std::uint64_t thunkRva = static_cast<std::uint64_t>(a_thunkRva) + index * sizeof(IMAGE_THUNK_DATA64);
        if (thunkRva > std::numeric_limits<std::uint32_t>::max())
        {
            break;
        }
        const std::optional<std::size_t> thunkOffset = rva_to_offset(
            static_cast<std::uint32_t>(thunkRva), sizeof(IMAGE_THUNK_DATA64), a_optional, a_sections, a_bytes.size());
        if (!thunkOffset)
        {
            break;
        }
        const std::optional<IMAGE_THUNK_DATA64> thunk = read_value<IMAGE_THUNK_DATA64>(a_bytes, *thunkOffset);
        if (!thunk || thunk->u1.AddressOfData == 0U)
        {
            return thunk ? cue::Result<void>::success()
                         : cue::Result<void>::failure(
                               make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                          "Shipping Product import thunk is invalid"));
        }
        if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal))
        {
            return cue::Result<void>::failure(make_error(a_assertContext,
                                                         cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                         "Shipping Product ordinal imports are not allowed"));
        }
        if (thunk->u1.AddressOfData > std::numeric_limits<std::uint32_t>::max())
        {
            return cue::Result<void>::failure(make_error(a_assertContext,
                                                         cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                         "Shipping Product import name RVA is invalid"));
        }
        const std::optional<RvaFileRange> nameRange = rva_to_file_range(
            static_cast<std::uint32_t>(thunk->u1.AddressOfData), a_optional, a_sections, a_bytes.size());
        const std::optional<std::string_view> name =
            nameRange && nameRange->size > sizeof(WORD)
                ? read_ascii_string(a_bytes, nameRange->offset + sizeof(WORD), nameRange->size - sizeof(WORD))
                : std::nullopt;
        if (!name)
        {
            return cue::Result<void>::failure(make_error(a_assertContext,
                                                         cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                         "Shipping Product import name is invalid"));
        }
        if (std::ranges::find(k_forbiddenLoaderImports, *name) != k_forbiddenLoaderImports.end())
        {
            return cue::Result<void>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                           "Shipping Product links a forbidden dynamic module loader API"));
        }
    }
    return cue::Result<void>::failure(
        make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                   "Shipping Product import thunk is not terminated within the policy limit"));
}

/// @brief Direct Import TableをVersion付きAllowlistとLoader不在Policyへ照合する
[[nodiscard]] cue::Result<std::vector<std::string>> validate_imports(std::span<const std::byte> a_bytes,
                                                                     const IMAGE_OPTIONAL_HEADER64 &a_optional,
                                                                     std::span<const IMAGE_SECTION_HEADER> a_sections,
                                                                     const cue::AssertContext &a_assertContext) noexcept
{
    const IMAGE_DATA_DIRECTORY &directory = a_optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0U || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        return cue::Result<std::vector<std::string>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product import directory is missing"));
    }
    const std::optional<std::size_t> directoryOffset =
        rva_to_offset(directory.VirtualAddress, directory.Size, a_optional, a_sections, a_bytes.size());
    if (!directoryOffset)
    {
        return cue::Result<std::vector<std::string>>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product import directory is invalid"));
    }
    const std::size_t descriptorLimit = std::min(
        k_maximumImportDescriptors, static_cast<std::size_t>(directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR)));
    std::vector<std::string> imports;
    imports.reserve(descriptorLimit);
    for (std::size_t index = 0U; index < descriptorLimit; ++index)
    {
        const std::optional<IMAGE_IMPORT_DESCRIPTOR> descriptor =
            read_value<IMAGE_IMPORT_DESCRIPTOR>(a_bytes, *directoryOffset + index * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!descriptor)
        {
            break;
        }
        if (descriptor->OriginalFirstThunk == 0U && descriptor->FirstThunk == 0U && descriptor->Name == 0U)
        {
            std::ranges::sort(imports);
            return cue::Result<std::vector<std::string>>::success(std::move(imports));
        }
        const std::optional<RvaFileRange> nameRange =
            rva_to_file_range(descriptor->Name, a_optional, a_sections, a_bytes.size());
        const std::optional<std::string_view> name =
            nameRange ? read_ascii_string(a_bytes, nameRange->offset, nameRange->size) : std::nullopt;
        if (!name)
        {
            return cue::Result<std::vector<std::string>>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                           "Shipping Product import library name is invalid"));
        }
        const std::string normalized = lowercase_ascii(*name);
        if (std::ranges::find(k_allowedImports, normalized) == k_allowedImports.end())
        {
            return cue::Result<std::vector<std::string>>::failure(
                make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                           "Shipping Product imports a library outside the M17 allowlist"));
        }
        const std::uint32_t thunkRva =
            descriptor->OriginalFirstThunk != 0U ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        cue::Result<void> functions =
            validate_import_functions(a_bytes, thunkRva, a_optional, a_sections, a_assertContext);
        if (!functions)
        {
            return cue::Result<std::vector<std::string>>::failure(std::move(*functions.try_error()));
        }
        imports.push_back(normalized);
    }
    return cue::Result<std::vector<std::string>>::failure(
        make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                   "Shipping Product import directory is not terminated within the policy limit"));
}

/// @brief Extended DLL CharacteristicsからCET Shadow Stack互換Flagを検証する
[[nodiscard]] cue::Result<void> validate_cet(std::span<const std::byte> a_bytes,
                                             const IMAGE_OPTIONAL_HEADER64 &a_optional,
                                             std::span<const IMAGE_SECTION_HEADER> a_sections,
                                             const cue::AssertContext &a_assertContext) noexcept
{
    const IMAGE_DATA_DIRECTORY &directory = a_optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (directory.VirtualAddress == 0U || directory.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0U)
    {
        return cue::Result<void>::failure(make_error(a_assertContext,
                                                     cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                     "Shipping Product CET evidence is missing"));
    }
    const std::optional<std::size_t> directoryOffset =
        rva_to_offset(directory.VirtualAddress, directory.Size, a_optional, a_sections, a_bytes.size());
    if (!directoryOffset)
    {
        return cue::Result<void>::failure(make_error(a_assertContext,
                                                     cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                     "Shipping Product debug directory is invalid"));
    }
    const std::size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t index = 0U; index < count; ++index)
    {
        const std::optional<IMAGE_DEBUG_DIRECTORY> debug =
            read_value<IMAGE_DEBUG_DIRECTORY>(a_bytes, *directoryOffset + index * sizeof(IMAGE_DEBUG_DIRECTORY));
        if (!debug || debug->Type != IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS)
        {
            continue;
        }
        const std::optional<std::size_t> mappedDataOffset =
            debug->SizeOfData >= sizeof(std::uint32_t)
                ? rva_to_offset(debug->AddressOfRawData, debug->SizeOfData, a_optional, a_sections, a_bytes.size())
                : std::nullopt;
        if (!mappedDataOffset || *mappedDataOffset != debug->PointerToRawData ||
            debug->PointerToRawData > a_bytes.size() || debug->SizeOfData > a_bytes.size() - debug->PointerToRawData)
        {
            return cue::Result<void>::failure(make_error(a_assertContext,
                                                         cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                         "Shipping Product extended DLL characteristics are invalid"));
        }
        const std::optional<std::uint32_t> characteristics =
            read_value<std::uint32_t>(a_bytes, debug->PointerToRawData);
        if (characteristics && (*characteristics & k_cetCompatible) != 0U)
        {
            return cue::Result<void>::success();
        }
    }
    return cue::Result<void>::failure(make_error(a_assertContext,
                                                 cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                 "Shipping Product is not marked CET Shadow Stack compatible"));
}

/// @brief Product Mappingのx64 PE Header、Hardening、Import Policyを検証する
[[nodiscard]] cue::Result<PeSecurityEvidence> validate_pe_security(const MappedProduct &a_product,
                                                                   const cue::AssertContext &a_assertContext) noexcept
{
    const std::span<const std::byte> bytes = a_product.bytes();
    const std::optional<IMAGE_DOS_HEADER> dos = read_value<IMAGE_DOS_HEADER>(bytes, 0U);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product DOS header is invalid"));
    }
    const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    const std::optional<DWORD> signature = read_value<DWORD>(bytes, ntOffset);
    const std::optional<IMAGE_FILE_HEADER> fileHeader = read_value<IMAGE_FILE_HEADER>(bytes, ntOffset + sizeof(DWORD));
    const std::size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (!signature || *signature != IMAGE_NT_SIGNATURE || !fileHeader ||
        fileHeader->Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (fileHeader->Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0U ||
        (fileHeader->Characteristics & IMAGE_FILE_DLL) != 0U ||
        fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product must be an x64 PE executable"));
    }
    const std::optional<IMAGE_OPTIONAL_HEADER64> optional = read_value<IMAGE_OPTIONAL_HEADER64>(bytes, optionalOffset);
    if (!optional || optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        optional->NumberOfRvaAndSizes < IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product optional header is invalid"));
    }
    if ((optional->DllCharacteristics & k_requiredDllCharacteristics) != k_requiredDllCharacteristics ||
        (fileHeader->Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0U)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product does not satisfy ASLR, High Entropy VA, DEP, and CFG header policy"));
    }
    const std::size_t sectionsOffset = optionalOffset + fileHeader->SizeOfOptionalHeader;
    if (fileHeader->NumberOfSections == 0U || sectionsOffset > bytes.size() ||
        static_cast<std::size_t>(fileHeader->NumberOfSections) >
            (bytes.size() - sectionsOffset) / sizeof(IMAGE_SECTION_HEADER))
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product section table is invalid"));
    }
    std::vector<IMAGE_SECTION_HEADER> sections(fileHeader->NumberOfSections);
    std::memcpy(sections.data(), bytes.data() + sectionsOffset, sections.size() * sizeof(IMAGE_SECTION_HEADER));

    cue::Result<std::vector<std::uint32_t>> relocations =
        validate_base_relocations(bytes, *optional, sections, a_assertContext);
    if (!relocations)
    {
        return cue::Result<PeSecurityEvidence>::failure(std::move(*relocations.try_error()));
    }

    const IMAGE_DATA_DIRECTORY &loadDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr std::size_t requiredLoadConfigSize = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags) + sizeof(DWORD);
    const std::optional<std::size_t> loadOffset =
        loadDirectory.VirtualAddress != 0U && loadDirectory.Size >= sizeof(DWORD)
            ? rva_to_offset(loadDirectory.VirtualAddress, sizeof(DWORD), *optional, sections, bytes.size())
            : std::nullopt;
    if (!loadOffset)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product load configuration is missing or truncated"));
    }
    const std::optional<DWORD> loadSize =
        read_value<DWORD>(bytes, *loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, Size));
    const std::optional<std::size_t> validatedLoadOffset =
        loadSize && *loadSize >= requiredLoadConfigSize && *loadSize <= loadDirectory.Size
            ? rva_to_offset(loadDirectory.VirtualAddress, static_cast<std::size_t>(*loadSize), *optional, sections,
                            bytes.size())
            : std::nullopt;
    if (!validatedLoadOffset)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product load configuration does not cover the required security fields"));
    }
    const std::optional<ULONGLONG> securityCookie =
        read_value<ULONGLONG>(bytes, *validatedLoadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie));
    const std::optional<ULONGLONG> guardCheck = read_value<ULONGLONG>(
        bytes, *validatedLoadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer));
    const std::optional<ULONGLONG> guardDispatch = read_value<ULONGLONG>(
        bytes, *validatedLoadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer));
    const std::optional<DWORD> guardFlags =
        read_value<DWORD>(bytes, *validatedLoadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags));
    const std::optional<WORD> dependentLoadFlags =
        read_value<WORD>(bytes, *validatedLoadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DependentLoadFlags));
    const std::optional<MappedImageFileRange> securityCookieRange =
        securityCookie ? mapped_image_va_range(*securityCookie, sizeof(ULONGLONG), *optional, sections, bytes.size())
                       : std::nullopt;
    const std::optional<MappedImageFileRange> guardCheckRange =
        guardCheck ? mapped_image_va_range(*guardCheck, sizeof(ULONGLONG), *optional, sections, bytes.size())
                   : std::nullopt;
    const std::optional<MappedImageFileRange> guardDispatchRange =
        guardDispatch ? mapped_image_va_range(*guardDispatch, sizeof(ULONGLONG), *optional, sections, bytes.size())
                      : std::nullopt;
    const std::optional<ULONGLONG> guardCheckTarget =
        guardCheckRange && has_section_characteristics(*guardCheckRange, k_readOnlyDataSection,
                                                       IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)
            ? read_value<ULONGLONG>(bytes, guardCheckRange->offset)
            : std::nullopt;
    const std::optional<ULONGLONG> guardDispatchTarget =
        guardDispatchRange && has_section_characteristics(*guardDispatchRange, k_readOnlyDataSection,
                                                          IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)
            ? read_value<ULONGLONG>(bytes, guardDispatchRange->offset)
            : std::nullopt;
    const std::optional<MappedImageFileRange> guardCheckTargetRange =
        guardCheckTarget ? mapped_image_va_range(*guardCheckTarget, 1U, *optional, sections, bytes.size())
                         : std::nullopt;
    const std::optional<MappedImageFileRange> guardDispatchTargetRange =
        guardDispatchTarget ? mapped_image_va_range(*guardDispatchTarget, 1U, *optional, sections, bytes.size())
                            : std::nullopt;
    const bool hasMappedSecurityCookie =
        securityCookieRange && has_section_characteristics(*securityCookieRange, k_writableDataSection,
                                                           IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_SHARED);
    const bool hasMappedGuardCheck =
        guardCheckTargetRange &&
        has_section_characteristics(*guardCheckTargetRange, k_executableCodeSection, IMAGE_SCN_MEM_WRITE);
    const bool hasMappedGuardDispatch =
        guardDispatchTargetRange &&
        has_section_characteristics(*guardDispatchTargetRange, k_executableCodeSection, IMAGE_SCN_MEM_WRITE);
    const std::array<std::uint64_t, 5U> requiredRelocations = {
        static_cast<std::uint64_t>(loadDirectory.VirtualAddress) +
            offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie),
        static_cast<std::uint64_t>(loadDirectory.VirtualAddress) +
            offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer),
        static_cast<std::uint64_t>(loadDirectory.VirtualAddress) +
            offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer),
        guardCheck && *guardCheck >= optional->ImageBase ? *guardCheck - optional->ImageBase
                                                         : std::numeric_limits<std::uint64_t>::max(),
        guardDispatch && *guardDispatch >= optional->ImageBase ? *guardDispatch - optional->ImageBase
                                                               : std::numeric_limits<std::uint64_t>::max()};
    const bool hasRequiredRelocations = std::ranges::all_of(
        requiredRelocations,
        [&relocations](const std::uint64_t a_rva) noexcept
        {
            return a_rva <= std::numeric_limits<std::uint32_t>::max() &&
                   std::ranges::binary_search(*relocations.try_value(), static_cast<std::uint32_t>(a_rva));
        });
    if (!hasMappedSecurityCookie || !hasMappedGuardCheck || !hasMappedGuardDispatch || !guardFlags ||
        (*guardFlags & IMAGE_GUARD_CF_INSTRUMENTED) == 0U || (*guardFlags & IMAGE_GUARD_SECURITY_COOKIE_UNUSED) != 0U ||
        !dependentLoadFlags || *dependentLoadFlags != k_requiredDependentLoadFlags || !hasRequiredRelocations)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product load configuration does not satisfy CFG, stack, and dependent-load policy"));
    }
    if (optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress != 0U ||
        optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size != 0U)
    {
        return cue::Result<PeSecurityEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                       "Shipping Product delay imports are not allowed by the M17 policy"));
    }
    cue::Result<std::vector<std::string>> imports = validate_imports(bytes, *optional, sections, a_assertContext);
    if (!imports)
    {
        return cue::Result<PeSecurityEvidence>::failure(std::move(*imports.try_error()));
    }
    cue::Result<void> cet = validate_cet(bytes, *optional, sections, a_assertContext);
    if (!cet)
    {
        return cue::Result<PeSecurityEvidence>::failure(std::move(*cet.try_error()));
    }
    return cue::Result<PeSecurityEvidence>::success({std::move(*imports.try_value())});
}

/// @brief WinTrust State Handleを必ずCloseする
class WinTrustState final
{
  public:
    /// @brief VERIFY実行済みTrust Dataを借用する
    explicit WinTrustState(WINTRUST_DATA &a_data) noexcept : m_data(a_data.hWVTStateData != nullptr ? &a_data : nullptr)
    {
    }
    /// @brief State借用の複製を禁止する
    WinTrustState(const WinTrustState &) = delete;
    /// @brief State借用の複製代入を禁止する
    WinTrustState &operator=(const WinTrustState &) = delete;
    /// @brief State借用の移動を禁止する
    WinTrustState(WinTrustState &&) = delete;
    /// @brief State借用の移動代入を禁止する
    WinTrustState &operator=(WinTrustState &&) = delete;
    /// @brief WinTrust Provider StateをCloseする
    ~WinTrustState()
    {
        if (m_data != nullptr)
        {
            GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            m_data->dwStateAction = WTD_STATEACTION_CLOSE;
            static_cast<void>(WinVerifyTrust(nullptr, &action, m_data));
        }
    }

  private:
    WINTRUST_DATA *m_data;
};

/// @brief WinTrust失敗Codeを安定した署名状態へ分類する
[[nodiscard]] cue::WindowsProductSignatureStatus classify_trust_status(LONG a_status) noexcept
{
    switch (a_status)
    {
    case ERROR_SUCCESS:
        return cue::WindowsProductSignatureStatus::Trusted;
    case TRUST_E_NOSIGNATURE:
        return cue::WindowsProductSignatureStatus::Unsigned;
    case TRUST_E_BAD_DIGEST:
    case NTE_BAD_SIGNATURE:
    case TRUST_E_SUBJECT_NOT_TRUSTED:
    case TRUST_E_MALFORMED_SIGNATURE:
    case TRUST_E_COUNTER_SIGNER:
    case TRUST_E_TIME_STAMP:
    case TRUST_E_NO_SIGNER_CERT:
        return cue::WindowsProductSignatureStatus::InvalidSignature;
    case CERT_E_EXPIRED:
        return cue::WindowsProductSignatureStatus::CertificateExpired;
    case CERT_E_REVOKED:
        return cue::WindowsProductSignatureStatus::CertificateRevoked;
    case CERT_E_CHAINING:
    case CERT_E_UNTRUSTEDROOT:
    case TRUST_E_EXPLICIT_DISTRUST:
    case CERT_E_WRONG_USAGE:
    case CERT_E_INVALID_NAME:
    case CERT_E_INVALID_POLICY:
    case CERT_E_CRITICAL:
    case CERT_E_MALFORMED:
    case CERT_E_PATHLENCONST:
    case CERT_E_ISSUERCHAINING:
    case CERT_E_UNTRUSTEDCA:
    case CERT_E_UNTRUSTEDTESTROOT:
    case CERT_E_VALIDITYPERIODNESTING:
    case CERT_E_PURPOSE:
    case CERT_E_ROLE:
    case CERT_E_CN_NO_MATCH:
    case TRUST_E_BASIC_CONSTRAINTS:
    case TRUST_E_CERT_SIGNATURE:
    case TRUST_E_FINANCIAL_CRITERIA:
        return cue::WindowsProductSignatureStatus::ChainInvalid;
    case CRYPT_E_REVOCATION_OFFLINE:
    case CRYPT_E_NO_REVOCATION_CHECK:
    case CERT_E_REVOCATION_FAILURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
    case TRUST_E_PROVIDER_UNKNOWN:
    case TRUST_E_SYSTEM_ERROR:
    default:
        return cue::WindowsProductSignatureStatus::VerificationUnavailable;
    }
}

/// @brief Digest Byte列をlowercase Hexへ変換する
[[nodiscard]] std::string lowercase_hex(std::span<const BYTE> a_digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(a_digest.size() * 2U);
    for (std::size_t index = 0U; index < a_digest.size(); ++index)
    {
        result[index * 2U] = digits[a_digest[index] >> 4U];
        result[index * 2U + 1U] = digits[a_digest[index] & 0x0FU];
    }
    return result;
}

/// @brief 検証中Product Handleの同一Mapped Byte列からSHA-256を計算する
[[nodiscard]] cue::Result<std::string> product_hash(const MappedProduct &a_product,
                                                    const cue::AssertContext &a_assertContext)
{
    const std::span<const std::byte> bytes = a_product.bytes();
    if (bytes.size() > std::numeric_limits<DWORD>::max())
    {
        return cue::Result<std::string>::failure(make_error(a_assertContext,
                                                            cue::WindowsBuildArtifactError::SecurityPolicyViolation,
                                                            "Shipping Product exceeds the hash provider input limit"));
    }
    std::array<BYTE, 32U> digest{};
    DWORD digestSize = static_cast<DWORD>(digest.size());
    const BOOL hashed =
        CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0U, nullptr, reinterpret_cast<const BYTE *>(bytes.data()),
                              static_cast<DWORD>(bytes.size()), digest.data(), &digestSize);
    if (hashed == FALSE || digestSize != digest.size())
    {
        return cue::Result<std::string>::failure(
            make_windows_error(a_assertContext, cue::WindowsBuildArtifactError::SecurityPolicyViolation, GetLastError(),
                               "Shipping Product security snapshot could not be hashed"));
    }
    return cue::Result<std::string>::success(lowercase_hex(digest));
}

/// @brief DER SubjectPublicKeyInfoをlowercase SHA-256 Identityへ変換する
[[nodiscard]] cue::Result<std::string> publisher_key_id(PCCERT_CONTEXT a_certificate,
                                                        const cue::AssertContext &a_assertContext)
{
    BYTE *encoded = nullptr;
    DWORD encodedSize = 0U;
    if (a_certificate == nullptr || a_certificate->pCertInfo == nullptr ||
        CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &a_certificate->pCertInfo->SubjectPublicKeyInfo,
                            CRYPT_ENCODE_ALLOC_FLAG, nullptr, &encoded, &encodedSize) == FALSE)
    {
        return cue::Result<std::string>::failure(make_error(a_assertContext,
                                                            cue::WindowsBuildArtifactError::SignatureVerificationFailed,
                                                            "Authenticode signer public key could not be encoded"));
    }
    std::array<BYTE, 32U> digest{};
    DWORD digestSize = static_cast<DWORD>(digest.size());
    const BOOL hashed =
        CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0U, nullptr, encoded, encodedSize, digest.data(), &digestSize);
    LocalFree(encoded);
    if (hashed == FALSE || digestSize != digest.size())
    {
        return cue::Result<std::string>::failure(make_error(a_assertContext,
                                                            cue::WindowsBuildArtifactError::SignatureVerificationFailed,
                                                            "Authenticode signer public key could not be hashed"));
    }
    return cue::Result<std::string>::success(lowercase_hex(digest));
}

/// @brief 保持中Product HandleをWindows Authenticode Policyで検証する
[[nodiscard]] cue::Result<cue::WindowsProductTrustEvidence> inspect_trust(const MappedProduct &a_product,
                                                                          const cue::AssertContext &a_assertContext)
{
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = a_product.native_path().c_str();
    file.hFile = a_product.file_handle();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_CACHE_ONLY_URL_RETRIEVAL;
    data.dwUIContext = WTD_UICONTEXT_EXECUTE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    WinTrustState state(data);
    cue::WindowsProductTrustEvidence evidence;
    evidence.signatureStatus = classify_trust_status(status);
    if (status != ERROR_SUCCESS)
    {
        return cue::Result<cue::WindowsProductTrustEvidence>::success(std::move(evidence));
    }
    CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    CRYPT_PROVIDER_SGNR *signer =
        provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0U, FALSE, 0U) : nullptr;
    if (signer == nullptr || signer->csCertChain == 0U || signer->pasCertChain == nullptr)
    {
        return cue::Result<cue::WindowsProductTrustEvidence>::failure(
            make_error(a_assertContext, cue::WindowsBuildArtifactError::SignatureVerificationFailed,
                       "Authenticode signer certificate is unavailable after trust verification"));
    }
    cue::Result<std::string> keyId = publisher_key_id(signer->pasCertChain[0U].pCert, a_assertContext);
    if (!keyId)
    {
        return cue::Result<cue::WindowsProductTrustEvidence>::failure(std::move(*keyId.try_error()));
    }
    evidence.publisherKeyId = std::move(*keyId.try_value());
    return cue::Result<cue::WindowsProductTrustEvidence>::success(std::move(evidence));
}
} // namespace

namespace cue::detail
{
/// @brief 検証済みProduct MappingとEvidenceを同じ寿命で所有する
struct WindowsProductSecuritySnapshot::State final
{
    /// @brief Product MappingとEvidenceの所有権を取得する
    State(MappedProduct a_product, WindowsProductSecurityValidation a_validation) noexcept
        : product(std::move(a_product)), validation(std::move(a_validation))
    {
    }

    MappedProduct product;
    WindowsProductSecurityValidation validation;
};

WindowsProductSecuritySnapshot::WindowsProductSecuritySnapshot(std::unique_ptr<State> a_state) noexcept
    : m_state(std::move(a_state))
{
}

WindowsProductSecuritySnapshot::WindowsProductSecuritySnapshot(WindowsProductSecuritySnapshot &&a_other) noexcept =
    default;

WindowsProductSecuritySnapshot &WindowsProductSecuritySnapshot::operator=(
    WindowsProductSecuritySnapshot &&a_other) noexcept = default;

WindowsProductSecuritySnapshot::~WindowsProductSecuritySnapshot() noexcept = default;

const WindowsProductSecurityValidation &WindowsProductSecuritySnapshot::validation() const noexcept
{
    return m_state->validation;
}

WindowsProductSecurityValidation WindowsProductSecuritySnapshot::take_validation() noexcept
{
    return std::move(m_state->validation);
}

WindowsProductSignatureStatus classify_windows_product_trust_status(std::int32_t a_status) noexcept
{
    return classify_trust_status(static_cast<LONG>(a_status));
}

Result<WindowsProductSecuritySnapshot> validate_windows_shipping_product_security_snapshot(
    std::string a_absoluteProductPath, const BuildProfile &a_profile, const AssertContext &a_assertContext) noexcept
{
    try
    {
        Result<MappedProduct> product = open_product(a_absoluteProductPath, a_assertContext);
        if (!product)
        {
            return Result<WindowsProductSecuritySnapshot>::failure(std::move(*product.try_error()));
        }
        Result<PeSecurityEvidence> pe = validate_pe_security(*product.try_value(), a_assertContext);
        if (!pe)
        {
            return Result<WindowsProductSecuritySnapshot>::failure(std::move(*pe.try_error()));
        }
        Result<WindowsProductTrustEvidence> inspected = inspect_trust(*product.try_value(), a_assertContext);
        if (!inspected)
        {
            return Result<WindowsProductSecuritySnapshot>::failure(std::move(*inspected.try_error()));
        }
        Result<WindowsProductDistributionStatus> distribution =
            evaluate_windows_product_trust_policy(a_profile, *inspected.try_value(), a_assertContext);
        if (!distribution)
        {
            return Result<WindowsProductSecuritySnapshot>::failure(std::move(*distribution.try_error()));
        }
        Result<std::string> hash = product_hash(*product.try_value(), a_assertContext);
        if (!hash)
        {
            return Result<WindowsProductSecuritySnapshot>::failure(std::move(*hash.try_error()));
        }
        WindowsProductSecurityValidation validation{
            static_cast<std::uint64_t>(product.try_value()->bytes().size()), std::move(*hash.try_value()),
            std::move(pe.try_value()->importedLibraries), std::move(*inspected.try_value()), *distribution.try_value()};
        std::unique_ptr<WindowsProductSecuritySnapshot::State> state =
            std::make_unique<WindowsProductSecuritySnapshot::State>(std::move(*product.try_value()),
                                                                    std::move(validation));
        return Result<WindowsProductSecuritySnapshot>::success(WindowsProductSecuritySnapshot(std::move(state)));
    }
    catch (...)
    {
        terminate_security_exception(a_assertContext);
    }
}
} // namespace cue::detail

namespace cue
{
Result<WindowsProductTrustEvidence> inspect_windows_product_trust(std::string a_absoluteProductPath,
                                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        Result<MappedProduct> product = open_product(a_absoluteProductPath, a_assertContext);
        if (!product)
        {
            return Result<WindowsProductTrustEvidence>::failure(std::move(*product.try_error()));
        }
        return inspect_trust(*product.try_value(), a_assertContext);
    }
    catch (...)
    {
        terminate_security_exception(a_assertContext);
    }
}

Result<WindowsProductDistributionStatus> evaluate_windows_product_trust_policy(
    const BuildProfile &a_profile, const WindowsProductTrustEvidence &a_evidence,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_profile.target() != BuildTarget::ShippingProduct ||
            a_profile.configuration() != BuildConfiguration::Release || !a_profile.minimum_trust_mode())
        {
            return Result<WindowsProductDistributionStatus>::failure(
                make_error(a_assertContext, WindowsBuildArtifactError::SecurityPolicyViolation,
                           "Windows Product trust policy requires a Release ShippingProduct profile"));
        }
        if (*a_profile.minimum_trust_mode() == ShippingTrustMode::UnsignedLocal)
        {
            return Result<WindowsProductDistributionStatus>::success(
                WindowsProductDistributionStatus::LocalExecutionOnly);
        }
        if (a_evidence.signatureStatus != WindowsProductSignatureStatus::Trusted)
        {
            return Result<WindowsProductDistributionStatus>::failure(
                make_error(a_assertContext, WindowsBuildArtifactError::SignatureVerificationFailed,
                           "PublisherSigned Product did not satisfy Windows Authenticode trust policy"));
        }
        if (a_evidence.publisherKeyId != a_profile.publisher_key_id())
        {
            return Result<WindowsProductDistributionStatus>::failure(
                make_error(a_assertContext, WindowsBuildArtifactError::PublisherMismatch,
                           "PublisherSigned Product signer does not match the configured Publisher Key ID"));
        }
        return Result<WindowsProductDistributionStatus>::success(
            WindowsProductDistributionStatus::PublisherVerifiedArtifact);
    }
    catch (...)
    {
        terminate_security_exception(a_assertContext);
    }
}

Result<WindowsProductSecurityValidation> validate_windows_shipping_product_security(
    std::string a_absoluteProductPath, const BuildProfile &a_profile, const AssertContext &a_assertContext) noexcept
{
    try
    {
        Result<detail::WindowsProductSecuritySnapshot> snapshot =
            detail::validate_windows_shipping_product_security_snapshot(std::move(a_absoluteProductPath), a_profile,
                                                                        a_assertContext);
        if (!snapshot)
        {
            return Result<WindowsProductSecurityValidation>::failure(std::move(*snapshot.try_error()));
        }
        WindowsProductSecurityValidation validation = snapshot.try_value()->take_validation();
        return Result<WindowsProductSecurityValidation>::success(std::move(validation));
    }
    catch (...)
    {
        terminate_security_exception(a_assertContext);
    }
}
} // namespace cue
