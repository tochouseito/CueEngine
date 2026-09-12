#include <Cue/Build/Plan.h>
#include <Cue/Build/Windows/WindowsProductSecurity.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include "WindowsProductSecurityInternal.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_publisherKeyId = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::uintmax_t k_oversizedProductBytes = 512ULL * 1024ULL * 1024ULL + 1ULL;
constexpr DWORD k_oversizedIatDirectoryBytes = (65536U + 1U) * sizeof(IMAGE_THUNK_DATA64);
constexpr std::string_view k_guardMetadataStrideReserveBegin = "CueGtStrideBegin";
constexpr std::string_view k_guardMetadataStrideReserveEnd = "CueGtStrideEnd!!";
constexpr std::size_t k_guardMetadataStrideReserveBytes = 1024U;

/// @brief Test中のFatalを即時終了へ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の引数なしFatalを即時終了へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(EXIT_FAILURE);
    }
    /// @brief Fatal理由にかかわらずTest Processを終了する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(EXIT_FAILURE);
    }
};

/// @brief PE Guard TableのRVA、件数、Stride、File Offsetを保持する
struct GuardTableView final
{
    std::uint32_t rva = 0U;
    std::size_t count = 0U;
    std::size_t stride = 0U;
    std::size_t offset = 0U;
};

/// @brief 指定Ordinalの実在FirstThunk RVAと同じImport Tableの終端RVAを保持する
struct ImportOrdinalView final
{
    std::uint32_t firstThunkRva = 0U;
    std::uint32_t terminatorRva = 0U;
};

/// @brief 条件不成立時にTest Processを終了する
void require(bool a_condition, const std::source_location &a_location = std::source_location::current())
{
    if (!a_condition)
    {
        std::fprintf(stderr, "require failed: %s:%u\n", a_location.file_name(), a_location.line());
        std::fflush(nullptr);
        std::_Exit(EXIT_FAILURE);
    }
}

/// @brief Test用Release Shipping Profileを構築する
[[nodiscard]] cue::BuildProfile make_profile(cue::ShippingTrustMode a_mode, const cue::AssertContext &a_assertContext)
{
    cue::Result<cue::BuildProfile> profile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, a_mode,
        a_mode == cue::ShippingTrustMode::PublisherSigned ? std::string(k_publisherKeyId) : std::string(),
        a_assertContext);
    require(profile.has_value());
    return std::move(*profile.try_value());
}

/// @brief File全体をTest所有Byte列へ読む
[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path &a_path)
{
    std::ifstream input(a_path, std::ios::binary);
    require(input.is_open());
    const std::vector<char> characters{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(characters.size());
    std::memcpy(bytes.data(), characters.data(), characters.size());
    return bytes;
}

/// @brief Test所有Byte列を新規Fileへ書く
void write_bytes(const std::filesystem::path &a_path, std::span<const std::byte> a_bytes)
{
    std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
    require(output.is_open());
    output.write(reinterpret_cast<const char *>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
    output.flush();
    require(output.good());
}

/// @brief PE Optional Headerの開始位置を検証付きで取得する
[[nodiscard]] std::size_t optional_header_offset(std::span<const std::byte> a_bytes)
{
    require(a_bytes.size() >= sizeof(IMAGE_DOS_HEADER));
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, a_bytes.data(), sizeof(dos));
    require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0);
    const std::size_t optionalOffset =
        static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    require(optionalOffset <= a_bytes.size() && sizeof(IMAGE_OPTIONAL_HEADER64) <= a_bytes.size() - optionalOffset);
    return optionalOffset;
}

/// @brief PE File Headerの開始位置を検証付きで取得する
[[nodiscard]] std::size_t file_header_offset(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    require(optionalOffset >= sizeof(IMAGE_FILE_HEADER));
    return optionalOffset - sizeof(IMAGE_FILE_HEADER);
}

/// @brief PE Optional HeaderのDynamic Base Flagを除去する
void clear_dynamic_base(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    optional.DllCharacteristics =
        static_cast<WORD>(optional.DllCharacteristics & ~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief PE File HeaderからLarge Address Aware Flagを除去する
void clear_large_address_aware(std::vector<std::byte> &a_bytes)
{
    const std::size_t fileOffset = file_header_offset(a_bytes);
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + fileOffset, sizeof(fileHeader));
    fileHeader.Characteristics = static_cast<WORD>(fileHeader.Characteristics & ~IMAGE_FILE_LARGE_ADDRESS_AWARE);
    std::memcpy(a_bytes.data() + fileOffset, &fileHeader, sizeof(fileHeader));
}

/// @brief Load Configuration DirectoryのRVAを除去する
void clear_load_configuration_rva(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress = 0U;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief PE Headerへ再配置除去Flagを設定する
void mark_relocations_stripped(std::vector<std::byte> &a_bytes)
{
    const std::size_t fileOffset = file_header_offset(a_bytes);
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + fileOffset, sizeof(fileHeader));
    fileHeader.Characteristics = static_cast<WORD>(fileHeader.Characteristics | IMAGE_FILE_RELOCS_STRIPPED);
    std::memcpy(a_bytes.data() + fileOffset, &fileHeader, sizeof(fileHeader));
}

/// @brief PE Section 数を M17 Resource Limit 超過へ改変する
void exceed_section_count_limit(std::vector<std::byte> &a_bytes)
{
    const std::size_t fileOffset = file_header_offset(a_bytes);
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + fileOffset, sizeof(fileHeader));
    constexpr WORD maximumSectionCount = 96U;
    fileHeader.NumberOfSections = maximumSectionCount + 1U;
    std::memcpy(a_bytes.data() + fileOffset, &fileHeader, sizeof(fileHeader));
}

/// @brief Base Relocation DirectoryをBlock Header未満へ切り詰める
void truncate_relocation_directory(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = sizeof(IMAGE_BASE_RELOCATION) - 1U;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief Base Relocation DirectoryをM17 Resource Limit超過へ改変する
void exceed_relocation_directory_limit(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    constexpr DWORD maximumBaseRelocationDirectoryBytes = 1024U * 1024U;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = maximumBaseRelocationDirectoryBytes + 1U;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief PE の SizeOfImage を Base Relocation Directory 終端直前まで切り詰める
void truncate_image_at_relocation_directory(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    require(directory.Size > 0U &&
            directory.VirtualAddress <= std::numeric_limits<DWORD>::max() - (directory.Size - 1U));
    optional.SizeOfImage = directory.VirtualAddress + directory.Size - 1U;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief PE Section Tableを検証付きでTest所有領域へ読む
[[nodiscard]] std::vector<IMAGE_SECTION_HEADER> read_sections(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + file_header_offset(a_bytes), sizeof(fileHeader));
    const std::size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    require(fileHeader.NumberOfSections != 0U && sectionsOffset <= a_bytes.size() &&
            fileHeader.NumberOfSections <= (a_bytes.size() - sectionsOffset) / sizeof(IMAGE_SECTION_HEADER));
    std::vector<IMAGE_SECTION_HEADER> sections(fileHeader.NumberOfSections);
    std::memcpy(sections.data(), a_bytes.data() + sectionsOffset, sections.size() * sizeof(IMAGE_SECTION_HEADER));
    return sections;
}

/// @brief Test用PE RVAをSection内のFile Offsetへ変換する
[[nodiscard]] std::size_t section_rva_offset(std::uint32_t a_rva, std::size_t a_size,
                                             std::span<const IMAGE_SECTION_HEADER> a_sections,
                                             std::span<const std::byte> a_bytes)
{
    for (const IMAGE_SECTION_HEADER &section : a_sections)
    {
        const std::uint64_t start = section.VirtualAddress;
        const std::uint64_t rva = a_rva;
        if (rva < start)
        {
            continue;
        }
        const std::uint64_t delta = rva - start;
        if (delta >= section.SizeOfRawData || a_size > section.SizeOfRawData - static_cast<std::size_t>(delta))
        {
            continue;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(section.PointerToRawData) + delta;
        require(offset <= a_bytes.size() && a_size <= a_bytes.size() - static_cast<std::size_t>(offset));
        return static_cast<std::size_t>(offset);
    }
    require(false);
    return 0U;
}

/// @brief Load Configuration DirectoryをPE Header終端跨ぎに改変する
void cross_header_boundary_for_load_configuration(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    require(optional.SizeOfHeaders != 0U);
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress = optional.SizeOfHeaders - 1U;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief Load Configurationの指定Security Pointerを指定VAへ改変する
void set_load_configuration_pointer(std::vector<std::byte> &a_bytes, std::size_t a_fieldOffset, ULONGLONG a_value)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    require(a_fieldOffset <= directory.Size && sizeof(a_value) <= directory.Size - a_fieldOffset);
    std::memcpy(a_bytes.data() + loadOffset + a_fieldOffset, &a_value, sizeof(a_value));
}

/// @brief Load Configurationの指定Security Pointer値を読む
[[nodiscard]] ULONGLONG load_configuration_pointer(std::span<const std::byte> a_bytes, std::size_t a_fieldOffset)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    require(a_fieldOffset <= directory.Size && sizeof(ULONGLONG) <= directory.Size - a_fieldOffset);
    ULONGLONG value = 0U;
    std::memcpy(&value, a_bytes.data() + loadOffset + a_fieldOffset, sizeof(value));
    return value;
}

/// @brief Load Configurationの指定ULONGLONG値を指定値へ改変する
void set_load_configuration_ulonglong(std::vector<std::byte> &a_bytes, std::size_t a_fieldOffset, ULONGLONG a_value)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    require(a_fieldOffset <= directory.Size && sizeof(a_value) <= directory.Size - a_fieldOffset);
    std::memcpy(a_bytes.data() + loadOffset + a_fieldOffset, &a_value, sizeof(a_value));
}

/// @brief GuardFlagsからFunction-Table-Enabledフラグを除去する
void clear_guard_function_table_flag(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    DWORD guardFlags = 0U;
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    guardFlags &= ~IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT;
    std::memcpy(a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags), &guardFlags,
                sizeof(guardFlags));
}

/// @brief GuardFlags へ指定した未対応 Metadata Flag を追加する
void add_guard_flag(std::vector<std::byte> &a_bytes, DWORD a_flag)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    DWORD guardFlags = 0U;
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    guardFlags |= a_flag;
    std::memcpy(a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags), &guardFlags,
                sizeof(guardFlags));
}

/// @brief Guard Function Tableの指定Entryに対応するFile Offsetを返す
[[nodiscard]] std::size_t guard_function_table_entry_offset(std::span<const std::byte> a_bytes,
                                                            std::size_t a_entryIndex)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    ULONGLONG functionTable = 0U;
    ULONGLONG functionCount = 0U;
    DWORD guardFlags = 0U;
    std::memcpy(&functionTable,
                a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable),
                sizeof(functionTable));
    std::memcpy(&functionCount,
                a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionCount),
                sizeof(functionCount));
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    const std::size_t stride = sizeof(std::uint32_t) + ((guardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK) >>
                                                        IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT);
    require(functionTable >= optional.ImageBase &&
            functionTable - optional.ImageBase <= std::numeric_limits<DWORD>::max() &&
            functionCount <= std::numeric_limits<std::size_t>::max() / stride && a_entryIndex < functionCount);
    const std::size_t tableOffset =
        section_rva_offset(static_cast<std::uint32_t>(functionTable - optional.ImageBase),
                           static_cast<std::size_t>(functionCount) * stride, sections, a_bytes);
    return tableOffset + a_entryIndex * stride;
}

/// @brief Guard Function Tableの指定Entry RVAを読む
[[nodiscard]] std::uint32_t guard_function_table_entry(std::span<const std::byte> a_bytes, std::size_t a_entryIndex)
{
    const std::size_t offset = guard_function_table_entry_offset(a_bytes, a_entryIndex);
    std::uint32_t value = 0U;
    std::memcpy(&value, a_bytes.data() + offset, sizeof(value));
    return value;
}

/// @brief Guard Function Tableの指定Entry RVAを改変する
void set_guard_function_table_entry(std::vector<std::byte> &a_bytes, std::size_t a_entryIndex, std::uint32_t a_value)
{
    const std::size_t offset = guard_function_table_entry_offset(a_bytes, a_entryIndex);
    std::memcpy(a_bytes.data() + offset, &a_value, sizeof(a_value));
}

/// @brief Guard Function Table自体のRVAを返す
[[nodiscard]] std::uint32_t guard_function_table_rva(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const ULONGLONG functionTable =
        load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable));
    require(functionTable >= optional.ImageBase &&
            functionTable - optional.ImageBase <= std::numeric_limits<DWORD>::max());
    return static_cast<std::uint32_t>(functionTable - optional.ImageBase);
}

/// @brief Guard Function Table全体のByte Sizeを返す
[[nodiscard]] std::size_t guard_function_table_size(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    ULONGLONG functionCount = 0U;
    DWORD guardFlags = 0U;
    std::memcpy(&functionCount,
                a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionCount),
                sizeof(functionCount));
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    const std::size_t stride = sizeof(std::uint32_t) + ((guardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK) >>
                                                        IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT);
    require(stride >= sizeof(std::uint32_t) && functionCount > 0U &&
            functionCount <= std::numeric_limits<std::size_t>::max() / stride);
    return static_cast<std::size_t>(functionCount) * stride;
}

/// @brief 指定Load Configuration Fieldが参照するGuard Tableを検証付きで読む
[[nodiscard]] GuardTableView guard_table_view(std::span<const std::byte> a_bytes, std::size_t a_pointerOffset,
                                              std::size_t a_countOffset)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &loadDirectory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset =
        section_rva_offset(loadDirectory.VirtualAddress, loadDirectory.Size, sections, a_bytes);
    ULONGLONG table = 0U;
    ULONGLONG count = 0U;
    DWORD guardFlags = 0U;
    std::memcpy(&table, a_bytes.data() + loadOffset + a_pointerOffset, sizeof(table));
    std::memcpy(&count, a_bytes.data() + loadOffset + a_countOffset, sizeof(count));
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    const std::size_t stride = sizeof(std::uint32_t) + ((guardFlags & IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK) >>
                                                        IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT);
    require(table >= optional.ImageBase && table - optional.ImageBase <= std::numeric_limits<DWORD>::max() &&
            count > 0U && count <= std::numeric_limits<std::size_t>::max() / stride);
    const std::uint32_t tableRva = static_cast<std::uint32_t>(table - optional.ImageBase);
    const std::size_t tableSize = static_cast<std::size_t>(count) * stride;
    return {tableRva, static_cast<std::size_t>(count), stride,
            section_rva_offset(tableRva, tableSize, sections, a_bytes)};
}

/// @brief Guard Address-Taken IAT Tableを検証付きで読む
[[nodiscard]] GuardTableView guard_address_taken_iat_table(std::span<const std::byte> a_bytes)
{
    return guard_table_view(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryTable),
                            offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryCount));
}

/// @brief Guard Address-Taken IAT EntryのRVAを読む
[[nodiscard]] std::uint32_t guard_address_taken_iat_entry(std::span<const std::byte> a_bytes,
                                                          std::size_t a_entryIndex)
{
    const GuardTableView table = guard_address_taken_iat_table(a_bytes);
    require(a_entryIndex < table.count);
    std::uint32_t value = 0U;
    std::memcpy(&value, a_bytes.data() + table.offset + a_entryIndex * table.stride, sizeof(value));
    return value;
}

/// @brief Guard Address-Taken IAT EntryのRVAを改変する
void set_guard_address_taken_iat_entry(std::vector<std::byte> &a_bytes, std::size_t a_entryIndex,
                                       std::uint32_t a_value)
{
    const GuardTableView table = guard_address_taken_iat_table(a_bytes);
    require(a_entryIndex < table.count);
    std::memcpy(a_bytes.data() + table.offset + a_entryIndex * table.stride, &a_value, sizeof(a_value));
}

/// @brief Guard Function／Address-Taken IAT Tableを追加Metadata Byte付きStrideへ再配置する
void rewrite_guard_tables_with_metadata_stride(std::vector<std::byte> &a_bytes, std::uint8_t a_metadataBytes)
{
    require(a_metadataBytes > 0U);
    const GuardTableView functionTable =
        guard_table_view(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable),
                         offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionCount));
    const GuardTableView addressTable = guard_address_taken_iat_table(a_bytes);
    require(functionTable.stride == sizeof(std::uint32_t) && addressTable.stride == sizeof(std::uint32_t));
    std::vector<std::uint32_t> functionEntries(functionTable.count);
    std::vector<std::uint32_t> addressEntries(addressTable.count);
    for (std::size_t index = 0U; index < functionEntries.size(); ++index)
    {
        std::memcpy(&functionEntries[index], a_bytes.data() + functionTable.offset + index * functionTable.stride,
                    sizeof(std::uint32_t));
    }
    for (std::size_t index = 0U; index < addressEntries.size(); ++index)
    {
        std::memcpy(&addressEntries[index], a_bytes.data() + addressTable.offset + index * addressTable.stride,
                    sizeof(std::uint32_t));
    }
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &loadDirectory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset =
        section_rva_offset(loadDirectory.VirtualAddress, loadDirectory.Size, sections, a_bytes);
    DWORD guardFlags = 0U;
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    guardFlags = (guardFlags & ~IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK) |
                 (static_cast<DWORD>(a_metadataBytes) << IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT);
    std::memcpy(a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags), &guardFlags,
                sizeof(guardFlags));
    const std::size_t stride = sizeof(std::uint32_t) + a_metadataBytes;
    const std::size_t functionBytes = functionEntries.size() * stride;
    const std::size_t alignedFunctionBytes = (functionBytes + 3U) & ~std::size_t{3U};
    const std::size_t addressBytes = addressEntries.size() * stride;
    const std::size_t requiredBytes = alignedFunctionBytes + addressBytes;
    std::optional<std::size_t> rewrittenFunctionOffset;
    std::uint32_t rewrittenFunctionRva = 0U;
    std::uint32_t rewrittenAddressRva = 0U;
    require(requiredBytes <= k_guardMetadataStrideReserveBytes);
    for (std::size_t index = 0U;
         index + k_guardMetadataStrideReserveBegin.size() + k_guardMetadataStrideReserveBytes +
                 k_guardMetadataStrideReserveEnd.size() <=
             a_bytes.size();
         ++index)
    {
        if (std::memcmp(a_bytes.data() + index, k_guardMetadataStrideReserveBegin.data(),
                        k_guardMetadataStrideReserveBegin.size()) != 0 ||
            std::memcmp(a_bytes.data() + index + k_guardMetadataStrideReserveBegin.size() +
                            k_guardMetadataStrideReserveBytes,
                        k_guardMetadataStrideReserveEnd.data(), k_guardMetadataStrideReserveEnd.size()) != 0)
        {
            continue;
        }
        require(!rewrittenFunctionOffset.has_value());
        const std::size_t destinationOffset = index + k_guardMetadataStrideReserveBegin.size();
        for (const IMAGE_SECTION_HEADER &section : sections)
        {
            const std::size_t sectionStart = section.PointerToRawData;
            const std::size_t sectionEnd = sectionStart + section.SizeOfRawData;
            if (destinationOffset < sectionStart || destinationOffset > sectionEnd ||
                requiredBytes > sectionEnd - destinationOffset)
            {
                continue;
            }
            require((section.Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) == 0U);
            const std::size_t destinationDelta = destinationOffset - sectionStart;
            require(destinationDelta <= std::numeric_limits<std::uint32_t>::max() - section.VirtualAddress);
            rewrittenFunctionOffset = destinationOffset;
            rewrittenFunctionRva = section.VirtualAddress + static_cast<std::uint32_t>(destinationDelta);
            rewrittenAddressRva = rewrittenFunctionRva + static_cast<std::uint32_t>(alignedFunctionBytes);
            break;
        }
        require(rewrittenFunctionOffset.has_value());
        break;
    }
    require(rewrittenFunctionOffset.has_value());
    set_load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable),
                                   optional.ImageBase + rewrittenFunctionRva);
    set_load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryTable),
                                   optional.ImageBase + rewrittenAddressRva);
    const std::size_t rewrittenAddressOffset = *rewrittenFunctionOffset + alignedFunctionBytes;
    for (std::size_t index = 0U; index < functionEntries.size(); ++index)
    {
        std::memcpy(a_bytes.data() + *rewrittenFunctionOffset + index * stride, &functionEntries[index],
                    sizeof(std::uint32_t));
        std::fill_n(a_bytes.data() + *rewrittenFunctionOffset + index * stride + sizeof(std::uint32_t),
                    a_metadataBytes, std::byte{0U});
    }
    for (std::size_t index = 0U; index < addressEntries.size(); ++index)
    {
        std::memcpy(a_bytes.data() + rewrittenAddressOffset + index * stride, &addressEntries[index],
                    sizeof(std::uint32_t));
        std::fill_n(a_bytes.data() + rewrittenAddressOffset + index * stride + sizeof(std::uint32_t),
                    a_metadataBytes, std::byte{0U});
    }
}

/// @brief Guard Address-Taken IAT Entryの予約Metadata Byteを改変する
void set_guard_address_taken_iat_metadata(std::vector<std::byte> &a_bytes, std::size_t a_entryIndex,
                                          std::size_t a_metadataIndex, std::byte a_value)
{
    const GuardTableView table = guard_address_taken_iat_table(a_bytes);
    require(a_entryIndex < table.count && sizeof(std::uint32_t) + a_metadataIndex < table.stride);
    a_bytes[table.offset + a_entryIndex * table.stride + sizeof(std::uint32_t) + a_metadataIndex] = a_value;
}

/// @brief 指定RVA所属Sectionへ指定Memory属性を追加する
void add_section_characteristics_for_rva(std::vector<std::byte> &a_bytes, std::uint32_t a_rva,
                                         DWORD a_characteristics)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + file_header_offset(a_bytes), sizeof(fileHeader));
    const std::size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    for (std::size_t index = 0U; index < fileHeader.NumberOfSections; ++index)
    {
        const std::size_t sectionOffset = sectionsOffset + index * sizeof(IMAGE_SECTION_HEADER);
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, a_bytes.data() + sectionOffset, sizeof(section));
        const std::uint64_t sectionSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (a_rva < section.VirtualAddress || a_rva - section.VirtualAddress >= sectionSize)
        {
            continue;
        }
        section.Characteristics |= a_characteristics;
        std::memcpy(a_bytes.data() + sectionOffset, &section, sizeof(section));
        return;
    }
    require(false);
}

/// @brief Guard Address-Taken IAT Tableを他のCFG Metadataを含まないread-only Sectionへ移す
[[nodiscard]] std::uint32_t relocate_guard_address_taken_iat_table(std::vector<std::byte> &a_bytes)
{
    const GuardTableView table = guard_address_taken_iat_table(a_bytes);
    std::vector<std::byte> entries(table.count * table.stride);
    std::memcpy(entries.data(), a_bytes.data() + table.offset, entries.size());
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + file_header_offset(a_bytes), sizeof(fileHeader));
    const std::size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    for (std::size_t index = 0U; index < fileHeader.NumberOfSections; ++index)
    {
        const std::size_t sectionOffset = sectionsOffset + index * sizeof(IMAGE_SECTION_HEADER);
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, a_bytes.data() + sectionOffset, sizeof(section));
        const std::uint64_t sectionEnd =
            static_cast<std::uint64_t>(section.VirtualAddress) +
            std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        constexpr DWORD required = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        constexpr DWORD forbidden = IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_SHARED;
        if ((section.Characteristics & required) != required || (section.Characteristics & forbidden) != 0U ||
            (table.rva >= section.VirtualAddress && table.rva < sectionEnd))
        {
            continue;
        }
        const std::size_t destinationDelta =
            (static_cast<std::size_t>(section.Misc.VirtualSize) + 7U) & ~std::size_t{7U};
        if (destinationDelta > section.SizeOfRawData || entries.size() > section.SizeOfRawData - destinationDelta)
        {
            continue;
        }
        const std::size_t destinationOffset = section.PointerToRawData + destinationDelta;
        require(destinationOffset <= a_bytes.size() && entries.size() <= a_bytes.size() - destinationOffset &&
                std::ranges::all_of(std::span(a_bytes).subspan(destinationOffset, entries.size()),
                                    [](std::byte a_value) noexcept { return a_value == std::byte{0U}; }));
        std::memcpy(a_bytes.data() + destinationOffset, entries.data(), entries.size());
        section.Misc.VirtualSize = static_cast<DWORD>(destinationDelta + entries.size());
        std::memcpy(a_bytes.data() + sectionOffset, &section, sizeof(section));
        const std::uint32_t destinationRva =
            section.VirtualAddress + static_cast<std::uint32_t>(destinationDelta);
        set_load_configuration_pointer(a_bytes,
                                       offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryTable),
                                       optional.ImageBase + destinationRva);
        return destinationRva;
    }
    require(false);
    return 0U;
}

/// @brief Guard Address-Taken IAT Table直後のゼロ領域へ実在IAT Entryを追記する
void append_guard_address_taken_iat_entry(std::vector<std::byte> &a_bytes, std::uint32_t a_entryRva)
{
    const GuardTableView table = guard_address_taken_iat_table(a_bytes);
    require(table.count > 0U && guard_address_taken_iat_entry(a_bytes, table.count - 1U) < a_entryRva);
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const std::size_t expandedSize = (table.count + 1U) * table.stride;
    const std::size_t expandedOffset = section_rva_offset(table.rva, expandedSize, sections, a_bytes);
    require(expandedOffset == table.offset &&
            std::ranges::all_of(std::span(a_bytes).subspan(table.offset + table.count * table.stride, table.stride),
                                [](std::byte a_value) noexcept { return a_value == std::byte{0U}; }));
    std::memcpy(a_bytes.data() + table.offset + table.count * table.stride, &a_entryRva, sizeof(a_entryRva));
    set_load_configuration_ulonglong(a_bytes,
                                     offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryCount),
                                     static_cast<ULONGLONG>(table.count + 1U));
}

/// @brief 指定RVAを対象とするDIR64再配置EntryをABSOLUTEへ置換する
void clear_dir64_relocation(std::vector<std::byte> &a_bytes, std::uint32_t a_targetRva)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const std::size_t directoryOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    std::size_t cursor = 0U;
    while (cursor < directory.Size)
    {
        IMAGE_BASE_RELOCATION block{};
        std::memcpy(&block, a_bytes.data() + directoryOffset + cursor, sizeof(block));
        require(block.SizeOfBlock >= sizeof(block) && block.SizeOfBlock <= directory.Size - cursor);
        const std::size_t entryCount = (block.SizeOfBlock - sizeof(block)) / sizeof(std::uint16_t);
        for (std::size_t index = 0U; index < entryCount; ++index)
        {
            const std::size_t entryOffset = directoryOffset + cursor + sizeof(block) + index * sizeof(std::uint16_t);
            std::uint16_t entry = 0U;
            std::memcpy(&entry, a_bytes.data() + entryOffset, sizeof(entry));
            const std::uint32_t target = block.VirtualAddress + (entry & 0x0fffU);
            if ((entry >> 12U) == IMAGE_REL_BASED_DIR64 && target == a_targetRva)
            {
                entry = static_cast<std::uint16_t>(entry & 0x0fffU);
                std::memcpy(a_bytes.data() + entryOffset, &entry, sizeof(entry));
                return;
            }
        }
        cursor += block.SizeOfBlock;
    }
    require(false);
}

/// @brief 非必須DIR64 Entryを指定必須RVA（必要なら+offset）へ置換する
bool duplicate_dir64_relocation(std::vector<std::byte> &a_bytes, std::uint32_t a_targetRva,
                                std::span<const std::uint32_t> a_requiredRvas, std::uint16_t a_offset = 0U)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const std::size_t directoryOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    std::size_t cursor = 0U;
    while (cursor < directory.Size)
    {
        IMAGE_BASE_RELOCATION block{};
        std::memcpy(&block, a_bytes.data() + directoryOffset + cursor, sizeof(block));
        require(block.SizeOfBlock >= sizeof(block) && block.SizeOfBlock <= directory.Size - cursor);
        if (a_targetRva >= block.VirtualAddress && a_targetRva - block.VirtualAddress <= 0x0fffU)
        {
            const std::uint16_t baseOffset = static_cast<std::uint16_t>(a_targetRva - block.VirtualAddress);
            if (a_offset > 0x0fffU - baseOffset)
            {
                cursor += block.SizeOfBlock;
                continue;
            }
            const std::uint16_t duplicate = static_cast<std::uint16_t>(
                (IMAGE_REL_BASED_DIR64 << 12U) | static_cast<std::uint16_t>(baseOffset + a_offset));
            const std::size_t entryCount = (block.SizeOfBlock - sizeof(block)) / sizeof(std::uint16_t);
            for (std::size_t index = 0U; index < entryCount; ++index)
            {
                const std::size_t entryOffset =
                    directoryOffset + cursor + sizeof(block) + index * sizeof(std::uint16_t);
                std::uint16_t entry = 0U;
                std::memcpy(&entry, a_bytes.data() + entryOffset, sizeof(entry));
                const std::uint32_t target = block.VirtualAddress + (entry & 0x0fffU);
                const bool required = std::ranges::find(a_requiredRvas, target) != a_requiredRvas.end();
                if ((entry >> 12U) == IMAGE_REL_BASED_DIR64 && !required)
                {
                    std::memcpy(a_bytes.data() + entryOffset, &duplicate, sizeof(duplicate));
                    return true;
                }
            }
        }
        cursor += block.SizeOfBlock;
    }
    return false;
}

/// @brief Relocation Directory末尾へ指定RVAのDIR64 Blockを追加する
void append_dir64_relocation(std::vector<std::byte> &a_bytes, std::uint32_t a_targetRva)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    constexpr DWORD blockSize = sizeof(IMAGE_BASE_RELOCATION) + 2U * sizeof(std::uint16_t);
    const std::size_t directoryOffset =
        section_rva_offset(directory.VirtualAddress, directory.Size + blockSize, sections, a_bytes);
    IMAGE_BASE_RELOCATION block{};
    block.VirtualAddress = a_targetRva & ~0x0fffU;
    block.SizeOfBlock = blockSize;
    const std::uint16_t entry =
        static_cast<std::uint16_t>((IMAGE_REL_BASED_DIR64 << 12U) | (a_targetRva - block.VirtualAddress));
    const std::uint16_t padding = 0U;
    std::memcpy(a_bytes.data() + directoryOffset + directory.Size, &block, sizeof(block));
    std::memcpy(a_bytes.data() + directoryOffset + directory.Size + sizeof(block), &entry, sizeof(entry));
    std::memcpy(a_bytes.data() + directoryOffset + directory.Size + sizeof(block) + sizeof(entry), &padding,
                sizeof(padding));
    directory.Size += blockSize;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief 非必須DIR64 Entryを必須RVA+offsetに置換する
void overlap_dir64_relocation(std::vector<std::byte> &a_bytes, std::span<const std::uint32_t> a_requiredRvas,
                              std::uint16_t a_offset)
{
    for (const std::uint32_t requiredRva : a_requiredRvas)
    {
        if (duplicate_dir64_relocation(a_bytes, requiredRva, a_requiredRvas, a_offset))
        {
            return;
        }
    }
    require(false);
}

/// @brief Security Evidenceが参照する全絶対VA格納位置のRVAを返す
[[nodiscard]] std::array<std::uint32_t, 6U> required_security_relocation_rvas(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const IMAGE_DATA_DIRECTORY &loadDirectory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const ULONGLONG guardCheck =
        load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer));
    const ULONGLONG guardDispatch =
        load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer));
    const ULONGLONG guardFunctionTable =
        load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable));
    require(guardCheck >= optional.ImageBase && guardCheck - optional.ImageBase <= std::numeric_limits<DWORD>::max());
    require(guardDispatch >= optional.ImageBase &&
            guardDispatch - optional.ImageBase <= std::numeric_limits<DWORD>::max());
    require(guardFunctionTable >= optional.ImageBase &&
            guardFunctionTable - optional.ImageBase <= std::numeric_limits<DWORD>::max());
    return {loadDirectory.VirtualAddress + static_cast<DWORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie)),
            loadDirectory.VirtualAddress +
                static_cast<DWORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer)),
            loadDirectory.VirtualAddress +
                static_cast<DWORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer)),
            loadDirectory.VirtualAddress +
                static_cast<DWORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable)),
            static_cast<DWORD>(guardCheck - optional.ImageBase),
            static_cast<DWORD>(guardDispatch - optional.ImageBase)};
}

/// @brief Image内VAが指す8Byte値を指定値へ改変する
void set_image_va_value(std::vector<std::byte> &a_bytes, ULONGLONG a_va, ULONGLONG a_value)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    require(a_va >= optional.ImageBase && a_va - optional.ImageBase <= std::numeric_limits<std::uint32_t>::max());
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const std::size_t valueOffset =
        section_rva_offset(static_cast<std::uint32_t>(a_va - optional.ImageBase), sizeof(a_value), sections, a_bytes);
    std::memcpy(a_bytes.data() + valueOffset, &a_value, sizeof(a_value));
}

/// @brief Image内RVAが指す8Byte値を指定値へ改変する
void set_image_rva_value(std::vector<std::byte> &a_bytes, std::uint32_t a_rva, std::uint64_t a_value)
{
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const std::size_t valueOffset = section_rva_offset(a_rva, sizeof(a_value), sections, a_bytes);
    std::memcpy(a_bytes.data() + valueOffset, &a_value, sizeof(a_value));
}

/// @brief Image内VAが指す8Byte値を読む
[[nodiscard]] ULONGLONG image_va_value(std::span<const std::byte> a_bytes, ULONGLONG a_va)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    require(a_va >= optional.ImageBase && a_va - optional.ImageBase <= std::numeric_limits<std::uint32_t>::max());
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const std::size_t valueOffset =
        section_rva_offset(static_cast<std::uint32_t>(a_va - optional.ImageBase), sizeof(ULONGLONG), sections, a_bytes);
    ULONGLONG value = 0U;
    std::memcpy(&value, a_bytes.data() + valueOffset, sizeof(value));
    return value;
}

/// @brief Load ConfigurationのSecurity Pointer拒否境界となるVA一覧を作る
[[nodiscard]] std::vector<ULONGLONG> invalid_load_configuration_pointer_values(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    require(!sections.empty() && sections.front().SizeOfRawData >= sizeof(ULONGLONG));
    return {1U, optional.ImageBase, optional.ImageBase + optional.SizeOfImage,
            optional.ImageBase + sections.front().VirtualAddress + sections.front().SizeOfRawData -
                sizeof(ULONGLONG) / 2U};
}

/// @brief Load Configuration内部SizeをGuardFlags終端未満へ切り詰める
void truncate_load_configuration_internal_size(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    constexpr DWORD truncatedSize = static_cast<DWORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags));
    std::memcpy(a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, Size), &truncatedSize,
                sizeof(truncatedSize));
}

/// @brief Load Configuration Data Directory SizeをGuardFlags終端未満へ切り詰める
void truncate_load_configuration_directory_size(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    constexpr DWORD truncatedSize = static_cast<DWORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags));
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size = truncatedSize;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief Load ConfigurationへSecurity Cookie未使用Flagを設定する
void mark_security_cookie_unused(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    DWORD guardFlags = 0U;
    std::memcpy(&guardFlags, a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags),
                sizeof(guardFlags));
    guardFlags |= IMAGE_GUARD_SECURITY_COOKIE_UNUSED;
    std::memcpy(a_bytes.data() + loadOffset + offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags), &guardFlags,
                sizeof(guardFlags));
}

/// @brief Security Cookie所属SectionへProcess間共有Flagを設定する
void mark_security_cookie_section_shared(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + file_header_offset(a_bytes), sizeof(fileHeader));
    const ULONGLONG securityCookie =
        load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie));
    require(securityCookie >= optional.ImageBase &&
            securityCookie - optional.ImageBase <= std::numeric_limits<std::uint32_t>::max());
    const std::uint32_t cookieRva = static_cast<std::uint32_t>(securityCookie - optional.ImageBase);
    const std::size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    for (std::size_t index = 0U; index < fileHeader.NumberOfSections; ++index)
    {
        const std::size_t sectionOffset = sectionsOffset + index * sizeof(IMAGE_SECTION_HEADER);
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, a_bytes.data() + sectionOffset, sizeof(section));
        const std::uint64_t sectionStart = section.VirtualAddress;
        const std::uint64_t sectionSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (cookieRva < sectionStart || cookieRva - sectionStart >= sectionSize)
        {
            continue;
        }
        section.Characteristics |= IMAGE_SCN_MEM_SHARED;
        std::memcpy(a_bytes.data() + sectionOffset, &section, sizeof(section));
        return;
    }
    require(false);
}

/// @brief Guard Function Tableが格納されるSectionへ実行属性を付与する
void mark_guard_function_table_section_executable(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    IMAGE_FILE_HEADER fileHeader{};
    std::memcpy(&fileHeader, a_bytes.data() + file_header_offset(a_bytes), sizeof(fileHeader));
    const ULONGLONG guardFunctionTable =
        load_configuration_pointer(a_bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable));
    require(guardFunctionTable >= optional.ImageBase &&
            guardFunctionTable - optional.ImageBase <= std::numeric_limits<DWORD>::max());
    const std::uint32_t functionTableRva = static_cast<std::uint32_t>(guardFunctionTable - optional.ImageBase);
    const std::size_t sectionsOffset = optionalOffset + fileHeader.SizeOfOptionalHeader;
    for (std::size_t index = 0U; index < fileHeader.NumberOfSections; ++index)
    {
        const std::size_t sectionOffset = sectionsOffset + index * sizeof(IMAGE_SECTION_HEADER);
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, a_bytes.data() + sectionOffset, sizeof(section));
        const std::uint64_t sectionStart = section.VirtualAddress;
        const std::uint64_t sectionSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (functionTableRva < sectionStart || functionTableRva - sectionStart >= sectionSize)
        {
            continue;
        }
        section.Characteristics |= IMAGE_SCN_MEM_EXECUTE;
        std::memcpy(a_bytes.data() + sectionOffset, &section, sizeof(section));
        return;
    }
    require(false);
}

/// @brief Import Library名を所属Section終端から外へ跨ぐRVAへ改変する
void cross_section_boundary_for_import_name(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const std::size_t descriptorOffset =
        section_rva_offset(optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,
                           sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
    for (const IMAGE_SECTION_HEADER &section : sections)
    {
        const std::uint64_t start = section.VirtualAddress;
        const std::uint64_t nameRva = descriptor.Name;
        if (section.SizeOfRawData == 0U || nameRva < start || nameRva - start >= section.SizeOfRawData)
        {
            continue;
        }
        const std::uint64_t boundaryRva = start + section.SizeOfRawData - 1U;
        require(boundaryRva <= std::numeric_limits<std::uint32_t>::max());
        const std::size_t boundaryOffset =
            static_cast<std::size_t>(section.PointerToRawData) + section.SizeOfRawData - 1U;
        require(boundaryOffset < a_bytes.size());
        descriptor.Name = static_cast<DWORD>(boundaryRva);
        std::memcpy(a_bytes.data() + descriptorOffset, &descriptor, sizeof(descriptor));
        a_bytes[boundaryOffset] = std::byte{static_cast<unsigned char>('K')};
        return;
    }
    require(false);
}

/// @brief CET Debug DataをFile終端外へ跨ぐ範囲へ改変する
void cross_file_boundary_for_cet_data(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    require(directory.Size >= sizeof(IMAGE_DEBUG_DIRECTORY) && directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) == 0U);
    const std::size_t directoryOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    const std::size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t index = 0U; index < count; ++index)
    {
        const std::size_t debugOffset = directoryOffset + index * sizeof(IMAGE_DEBUG_DIRECTORY);
        IMAGE_DEBUG_DIRECTORY debug{};
        std::memcpy(&debug, a_bytes.data() + debugOffset, sizeof(debug));
        if (debug.Type != IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS)
        {
            continue;
        }
        require(a_bytes.size() >= sizeof(std::uint32_t) &&
                a_bytes.size() - sizeof(std::uint32_t) <= std::numeric_limits<DWORD>::max());
        debug.SizeOfData = sizeof(std::uint64_t);
        debug.PointerToRawData = static_cast<DWORD>(a_bytes.size() - sizeof(std::uint32_t));
        std::memcpy(a_bytes.data() + debugOffset, &debug, sizeof(debug));
        constexpr std::uint32_t cetCompatible = IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT;
        std::memcpy(a_bytes.data() + debug.PointerToRawData, &cetCompatible, sizeof(cetCompatible));
        return;
    }
    require(false);
}

/// @brief CET Debug DataのRVAを同じRaw位置へ対応しない有効RVAへ改変する
void mismatch_cet_data_rva(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    require(directory.Size >= sizeof(IMAGE_DEBUG_DIRECTORY) && directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) == 0U);
    const std::size_t directoryOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    const std::size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t index = 0U; index < count; ++index)
    {
        const std::size_t debugOffset = directoryOffset + index * sizeof(IMAGE_DEBUG_DIRECTORY);
        IMAGE_DEBUG_DIRECTORY debug{};
        std::memcpy(&debug, a_bytes.data() + debugOffset, sizeof(debug));
        if (debug.Type != IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS)
        {
            continue;
        }
        require(directory.Size >= debug.SizeOfData && directory.VirtualAddress != debug.AddressOfRawData);
        debug.AddressOfRawData = directory.VirtualAddress;
        std::memcpy(a_bytes.data() + debugOffset, &debug, sizeof(debug));
        return;
    }
    require(false);
}

/// @brief 最初のImport Descriptorが参照するLibrary名、Lookup Thunk、IAT、Import名RVAを返す
[[nodiscard]] std::array<std::uint32_t, 4U> first_import_metadata_rvas(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::size_t descriptorOffset =
        section_rva_offset(directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
    const std::uint32_t lookupThunk =
        descriptor.OriginalFirstThunk != 0U ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
    require(descriptor.Name != 0U && lookupThunk != 0U && descriptor.FirstThunk != 0U);
    const std::size_t thunkOffset = section_rva_offset(lookupThunk, sizeof(IMAGE_THUNK_DATA64), sections, a_bytes);
    IMAGE_THUNK_DATA64 thunk{};
    std::memcpy(&thunk, a_bytes.data() + thunkOffset, sizeof(thunk));
    require(!IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal) &&
            thunk.u1.AddressOfData <= std::numeric_limits<std::uint32_t>::max());
    return {descriptor.Name, lookupThunk, descriptor.FirstThunk, static_cast<std::uint32_t>(thunk.u1.AddressOfData)};
}

/// @brief Image内のnull終端ASCII文字列をTest用に読む
[[nodiscard]] std::string image_ascii_string(std::span<const std::byte> a_bytes, std::uint32_t a_rva,
                                             std::span<const IMAGE_SECTION_HEADER> a_sections)
{
    const std::size_t offset = section_rva_offset(a_rva, 1U, a_sections, a_bytes);
    std::string value;
    for (std::size_t index = offset; index < a_bytes.size() && value.size() < 256U; ++index)
    {
        const char character = static_cast<char>(a_bytes[index]);
        if (character == '\0')
        {
            return value;
        }
        value.push_back(character);
    }
    require(false);
    return {};
}

/// @brief 指定LibraryのOrdinal Importに対応するIAT SlotとTable終端を返す
[[nodiscard]] ImportOrdinalView import_ordinal_view(std::span<const std::byte> a_bytes,
                                                    std::string_view a_library, WORD a_ordinal)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    require(directory.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR));
    const std::size_t maximumDescriptors = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t descriptorIndex = 0U; descriptorIndex < maximumDescriptors; ++descriptorIndex)
    {
        const std::uint32_t descriptorRva =
            directory.VirtualAddress + static_cast<std::uint32_t>(descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        const std::size_t descriptorOffset =
            section_rva_offset(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
        if (descriptor.Name == 0U && descriptor.FirstThunk == 0U && descriptor.OriginalFirstThunk == 0U)
        {
            return {};
        }
        if (image_ascii_string(a_bytes, descriptor.Name, sections) != a_library)
        {
            continue;
        }
        const std::uint32_t lookupRva =
            descriptor.OriginalFirstThunk != 0U ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        std::uint32_t ordinalFirstThunkRva = 0U;
        for (std::size_t thunkIndex = 0U; thunkIndex < 4096U; ++thunkIndex)
        {
            const std::uint32_t thunkRva =
                lookupRva + static_cast<std::uint32_t>(thunkIndex * sizeof(IMAGE_THUNK_DATA64));
            const std::size_t thunkOffset = section_rva_offset(thunkRva, sizeof(IMAGE_THUNK_DATA64), sections, a_bytes);
            IMAGE_THUNK_DATA64 thunk{};
            std::memcpy(&thunk, a_bytes.data() + thunkOffset, sizeof(thunk));
            if (thunk.u1.Ordinal == 0U)
            {
                return {ordinalFirstThunkRva,
                        descriptor.FirstThunk +
                            static_cast<std::uint32_t>(thunkIndex * sizeof(IMAGE_THUNK_DATA64))};
            }
            if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal) && IMAGE_ORDINAL64(thunk.u1.Ordinal) == a_ordinal)
            {
                ordinalFirstThunkRva =
                    descriptor.FirstThunk +
                    static_cast<std::uint32_t>(thunkIndex * sizeof(IMAGE_THUNK_DATA64));
            }
        }
        require(false);
    }
    return {};
}

/// @brief 指定LibraryのImport Ordinalを一件だけ置換する
[[nodiscard]] bool replace_import_ordinal(std::vector<std::byte> &a_bytes, std::string_view a_library,
                                          WORD a_expected, ULONGLONG a_replacement)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::size_t maximumDescriptors = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t descriptorIndex = 0U; descriptorIndex < maximumDescriptors; ++descriptorIndex)
    {
        const std::uint32_t descriptorRva =
            directory.VirtualAddress + static_cast<std::uint32_t>(descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        const std::size_t descriptorOffset =
            section_rva_offset(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
        if (descriptor.Name == 0U && descriptor.FirstThunk == 0U && descriptor.OriginalFirstThunk == 0U)
        {
            return false;
        }
        if (image_ascii_string(a_bytes, descriptor.Name, sections) != a_library)
        {
            continue;
        }
        const std::uint32_t lookupRva =
            descriptor.OriginalFirstThunk != 0U ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        for (std::size_t thunkIndex = 0U; thunkIndex < 4096U; ++thunkIndex)
        {
            const std::uint32_t thunkRva =
                lookupRva + static_cast<std::uint32_t>(thunkIndex * sizeof(IMAGE_THUNK_DATA64));
            const std::size_t thunkOffset = section_rva_offset(thunkRva, sizeof(IMAGE_THUNK_DATA64), sections, a_bytes);
            IMAGE_THUNK_DATA64 thunk{};
            std::memcpy(&thunk, a_bytes.data() + thunkOffset, sizeof(thunk));
            if (thunk.u1.Ordinal == 0U)
            {
                return false;
            }
            if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal) && IMAGE_ORDINAL64(thunk.u1.Ordinal) == a_expected)
            {
                thunk.u1.Ordinal = IMAGE_ORDINAL_FLAG64 | a_replacement;
                std::memcpy(a_bytes.data() + thunkOffset, &thunk, sizeof(thunk));
                return true;
            }
        }
        require(false);
    }
    return false;
}

/// @brief 最初のImport DescriptorをBound Importとして改変する
void set_first_import_timestamp(std::vector<std::byte> &a_bytes, DWORD a_timestamp)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::size_t descriptorOffset =
        section_rva_offset(directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
    require(descriptor.Name != 0U && descriptor.FirstThunk != 0U);
    descriptor.TimeDateStamp = a_timestamp;
    std::memcpy(a_bytes.data() + descriptorOffset, &descriptor, sizeof(descriptor));
}

/// @brief 最初のImport Lookup Thunkを指定序数へ改変する
void set_first_import_ordinal(std::vector<std::byte> &a_bytes, WORD a_ordinal)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::size_t descriptorOffset =
        section_rva_offset(directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
    const DWORD lookupThunk =
        descriptor.OriginalFirstThunk != 0U ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
    const std::size_t thunkOffset = section_rva_offset(lookupThunk, sizeof(IMAGE_THUNK_DATA64), sections, a_bytes);
    IMAGE_THUNK_DATA64 thunk{};
    thunk.u1.Ordinal = IMAGE_ORDINAL_FLAG64 | a_ordinal;
    std::memcpy(a_bytes.data() + thunkOffset, &thunk, sizeof(thunk));
}

/// @brief Bound Import Data Directoryを既存Import Directoryへ改変する
void set_bound_import_directory(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const IMAGE_DATA_DIRECTORY &importDirectory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    require(importDirectory.VirtualAddress != 0U && importDirectory.Size != 0U);
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = importDirectory;
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief 最初の Import Descriptor の FirstThunk を指定 RVA へ改変する
void set_first_import_first_thunk(std::vector<std::byte> &a_bytes, DWORD a_firstThunkRva)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::size_t descriptorOffset =
        section_rva_offset(directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
    require(descriptor.Name != 0U && descriptor.FirstThunk != 0U);
    descriptor.FirstThunk = a_firstThunkRva;
    std::memcpy(a_bytes.data() + descriptorOffset, &descriptor, sizeof(descriptor));
}

/// @brief 最初の Import Descriptor の OriginalFirstThunk を指定 RVA へ改変する
void set_first_import_original_thunk(std::vector<std::byte> &a_bytes, DWORD a_originalThunkRva)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::size_t descriptorOffset =
        section_rva_offset(directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, a_bytes.data() + descriptorOffset, sizeof(descriptor));
    require(descriptor.Name != 0U && descriptor.OriginalFirstThunk != 0U && descriptor.FirstThunk != 0U);
    descriptor.OriginalFirstThunk = a_originalThunkRva;
    std::memcpy(a_bytes.data() + descriptorOffset, &descriptor, sizeof(descriptor));
}

/// @brief 二番目の Import Descriptor を最初のDescriptorと同じThunk範囲へ改変する
void duplicate_first_import_descriptor(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    require(directory.Size >= 3U * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    const std::size_t descriptorOffset =
        section_rva_offset(directory.VirtualAddress, 2U * sizeof(IMAGE_IMPORT_DESCRIPTOR), sections, a_bytes);
    IMAGE_IMPORT_DESCRIPTOR first{};
    IMAGE_IMPORT_DESCRIPTOR second{};
    std::memcpy(&first, a_bytes.data() + descriptorOffset, sizeof(first));
    std::memcpy(&second, a_bytes.data() + descriptorOffset + sizeof(first), sizeof(second));
    require(first.Name != 0U && first.FirstThunk != 0U && second.Name != 0U && second.FirstThunk != 0U);
    std::memcpy(a_bytes.data() + descriptorOffset + sizeof(first), &first, sizeof(first));
}

/// @brief IAT Data Directory を指定範囲へ改変する
void set_iat_directory(std::vector<std::byte> &a_bytes, DWORD a_rva, DWORD a_size)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {a_rva, a_size};
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief Debug Directory を M17 Resource Limit 超過へ改変する
void exceed_debug_directory_limit(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    constexpr DWORD maximumDebugDirectoryEntries = 4096U;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size =
        (maximumDebugDirectoryEntries + 1U) * sizeof(IMAGE_DEBUG_DIRECTORY);
    std::memcpy(a_bytes.data() + optionalOffset, &optional, sizeof(optional));
}

/// @brief CET Evidenceに使うDebug DescriptorとExtended DLL Characteristics DataのRVAを返す
[[nodiscard]] std::array<std::uint32_t, 2U> cet_metadata_rvas(std::span<const std::byte> a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    const std::vector<IMAGE_SECTION_HEADER> sections = read_sections(a_bytes);
    const IMAGE_DATA_DIRECTORY &directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    require(directory.Size >= sizeof(IMAGE_DEBUG_DIRECTORY) && directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) == 0U);
    const std::size_t directoryOffset = section_rva_offset(directory.VirtualAddress, directory.Size, sections, a_bytes);
    const std::size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t index = 0U; index < count; ++index)
    {
        IMAGE_DEBUG_DIRECTORY debug{};
        std::memcpy(&debug, a_bytes.data() + directoryOffset + index * sizeof(debug), sizeof(debug));
        if (debug.Type == IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS)
        {
            return {directory.VirtualAddress + static_cast<std::uint32_t>(index * sizeof(debug)),
                    debug.AddressOfRawData};
        }
    }
    require(false);
    return {};
}

/// @brief 許可済みSystem DLL名を同じ長さの未知App-local DLL名へ置換する
void replace_import_library(std::vector<std::byte> &a_bytes)
{
    constexpr std::string_view expected = "KERNEL32.dll";
    constexpr std::string_view replacement = "EVILAPP0.dll";
    static_assert(expected.size() == replacement.size());
    const auto begin = reinterpret_cast<const char *>(a_bytes.data());
    const std::string_view image(begin, a_bytes.size());
    const std::size_t offset = image.find(expected);
    require(offset != std::string_view::npos);
    std::memcpy(a_bytes.data() + offset, replacement.data(), replacement.size());
}

/// @brief 一意なTest DirectoryをSystem Temp直下に作成する
[[nodiscard]] std::filesystem::path create_test_directory()
{
    std::filesystem::path path = std::filesystem::temp_directory_path();
    path /= "CueEngine-M17-ProductSecurity-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64());
    std::error_code error;
    require(std::filesystem::create_directory(path, error) && !error);
    return path;
}

/// @brief Simulated Evidenceで公開拒否とPublisher一致Policyを再現検証する
void test_trust_policy(const cue::BuildProfile &a_localProfile, const cue::BuildProfile &a_signedProfile,
                       const cue::AssertContext &a_assertContext)
{
    constexpr std::array rejectedStatuses = {cue::WindowsProductSignatureStatus::Unsigned,
                                             cue::WindowsProductSignatureStatus::InvalidSignature,
                                             cue::WindowsProductSignatureStatus::CertificateExpired,
                                             cue::WindowsProductSignatureStatus::CertificateRevoked,
                                             cue::WindowsProductSignatureStatus::ChainInvalid,
                                             cue::WindowsProductSignatureStatus::VerificationUnavailable};
    for (const cue::WindowsProductSignatureStatus status : rejectedStatuses)
    {
        cue::WindowsProductTrustEvidence evidence{status, {}};
        require(!cue::evaluate_windows_product_trust_policy(a_signedProfile, evidence, a_assertContext));
    }
    cue::WindowsProductTrustEvidence wrongPublisher{cue::WindowsProductSignatureStatus::Trusted, std::string(64U, 'f')};
    require(!cue::evaluate_windows_product_trust_policy(a_signedProfile, wrongPublisher, a_assertContext));

    cue::WindowsProductTrustEvidence trusted{cue::WindowsProductSignatureStatus::Trusted,
                                             std::string(k_publisherKeyId)};
    cue::Result<cue::WindowsProductDistributionStatus> signedResult =
        cue::evaluate_windows_product_trust_policy(a_signedProfile, trusted, a_assertContext);
    require(signedResult &&
            *signedResult.try_value() == cue::WindowsProductDistributionStatus::PublisherVerifiedArtifact);
    cue::Result<cue::WindowsProductDistributionStatus> localResult =
        cue::evaluate_windows_product_trust_policy(a_localProfile, trusted, a_assertContext);
    require(localResult && *localResult.try_value() == cue::WindowsProductDistributionStatus::LocalExecutionOnly);
}

/// @brief WinVerifyTrustの署名なしとProvider検証不能を区別する
void test_trust_status_classification()
{
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_NOSIGNATURE, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::Unsigned);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_NOSIGNATURE,
                                                               static_cast<std::uint32_t>(TRUST_E_NOSIGNATURE)) ==
            cue::WindowsProductSignatureStatus::Unsigned);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_NOSIGNATURE, ERROR_ACCESS_DENIED) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_NOSIGNATURE,
                                                               static_cast<std::uint32_t>(TRUST_E_PROVIDER_UNKNOWN)) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(
                TRUST_E_NOSIGNATURE, static_cast<std::uint32_t>(TRUST_E_SUBJECT_FORM_UNKNOWN)) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_MALFORMED_SIGNATURE, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::InvalidSignature);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_NO_SIGNER_CERT, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::InvalidSignature);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_PROVIDER_UNKNOWN, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_SUBJECT_FORM_UNKNOWN, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(CRYPT_E_REVOCATION_OFFLINE, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(CRYPT_E_NO_REVOCATION_CHECK, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(CERT_E_REVOCATION_FAILURE, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(CERT_E_WRONG_USAGE, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::ChainInvalid);
    require(cue::detail::classify_windows_product_trust_status(CERT_E_INVALID_NAME, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::ChainInvalid);
    require(cue::detail::classify_windows_product_trust_status(CERT_E_INVALID_POLICY, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::ChainInvalid);
    require(cue::detail::classify_windows_product_trust_status(CERT_E_VALIDITYPERIODNESTING, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::ChainInvalid);
    require(cue::detail::classify_windows_product_trust_status(CERT_E_UNTRUSTEDTESTROOT, ERROR_SUCCESS) ==
            cue::WindowsProductSignatureStatus::ChainInvalid);
}

/// @brief Security Snapshot生存中のWrite／Delete共有拒否と解放を検証する
void test_security_snapshot_lease(const std::filesystem::path &a_validProduct, const cue::BuildProfile &a_localProfile,
                                  const cue::AssertContext &a_assertContext)
{
    const std::filesystem::path directory = create_test_directory();
    const std::filesystem::path product = directory / "SnapshotLease.exe";
    write_bytes(product, read_bytes(a_validProduct));
    {
        cue::Result<cue::detail::WindowsProductSecuritySnapshot> snapshot =
            cue::detail::validate_windows_shipping_product_security_snapshot(product.generic_string(), a_localProfile,
                                                                             a_assertContext);
        require(snapshot.has_value());
        HANDLE writeHandle =
            CreateFileW(product.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD writeError = GetLastError();
        require(writeHandle == INVALID_HANDLE_VALUE && writeError == ERROR_SHARING_VIOLATION);
        HANDLE deleteHandle =
            CreateFileW(product.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD deleteError = GetLastError();
        require(deleteHandle == INVALID_HANDLE_VALUE && deleteError == ERROR_SHARING_VIOLATION);
    }
    HANDLE releasedHandle =
        CreateFileW(product.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(releasedHandle != INVALID_HANDLE_VALUE);
    require(CloseHandle(releasedHandle) != FALSE);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    require(!error);
}

/// @brief 実PEでHardening、Import、Loader、Unsigned検証を実行する
void test_product_security(const std::filesystem::path &a_validProduct,
                           const std::filesystem::path &a_d3d12OrdinalProduct,
                           const std::filesystem::path &a_unhardenedProduct,
                           const std::filesystem::path &a_loaderProduct,
                           const std::filesystem::path &a_packagedLoaderProduct,
                           const cue::AssertContext &a_assertContext)
{
    const cue::BuildProfile localProfile = make_profile(cue::ShippingTrustMode::UnsignedLocal, a_assertContext);
    const cue::BuildProfile signedProfile = make_profile(cue::ShippingTrustMode::PublisherSigned, a_assertContext);
    test_trust_status_classification();
    test_security_snapshot_lease(a_validProduct, localProfile, a_assertContext);

    cue::Result<cue::WindowsProductSecurityValidation> valid =
        cue::validate_windows_shipping_product_security(a_validProduct.generic_string(), localProfile, a_assertContext);
    if (!valid)
    {
        std::fprintf(stderr, "error=%.*s\n", static_cast<int>(valid.try_error()->summary().size()),
                     valid.try_error()->summary().data());
    }
    require(valid && valid.try_value()->byteSize == std::filesystem::file_size(a_validProduct) &&
            valid.try_value()->contentHash.size() == 64U &&
            valid.try_value()->distributionStatus == cue::WindowsProductDistributionStatus::LocalExecutionOnly &&
            valid.try_value()->trustEvidence.signatureStatus == cue::WindowsProductSignatureStatus::Unsigned &&
            std::find(valid.try_value()->importedLibraries.begin(), valid.try_value()->importedLibraries.end(),
                      "kernel32.dll") != valid.try_value()->importedLibraries.end());
    const std::filesystem::path directory = create_test_directory();
    std::vector<std::byte> d3d12OrdinalBytes = read_bytes(a_d3d12OrdinalProduct);
    const ImportOrdinalView d3d12Ordinal = import_ordinal_view(d3d12OrdinalBytes, "d3d12.dll", 101U);
    require(d3d12Ordinal.firstThunkRva != 0U && d3d12Ordinal.terminatorRva != 0U);
    append_guard_address_taken_iat_entry(d3d12OrdinalBytes, d3d12Ordinal.firstThunkRva);
    const std::filesystem::path d3d12AddressTakenOrdinal = directory / "D3D12AddressTakenOrdinal.exe";
    write_bytes(d3d12AddressTakenOrdinal, d3d12OrdinalBytes);
    cue::Result<cue::WindowsProductSecurityValidation> d3d12OrdinalValid =
        cue::validate_windows_shipping_product_security(d3d12AddressTakenOrdinal.generic_string(), localProfile,
                                                        a_assertContext);
    const GuardTableView d3d12AddressTable = guard_address_taken_iat_table(d3d12OrdinalBytes);
    require(d3d12AddressTable.count >= 2U && d3d12Ordinal.firstThunkRva != 0U &&
            d3d12Ordinal.terminatorRva != 0U);
    bool d3d12OrdinalIsAddressTaken = false;
    for (std::size_t index = 0U; index < d3d12AddressTable.count; ++index)
    {
        d3d12OrdinalIsAddressTaken = d3d12OrdinalIsAddressTaken ||
                                     guard_address_taken_iat_entry(d3d12OrdinalBytes, index) ==
                                         d3d12Ordinal.firstThunkRva;
    }
    require(d3d12OrdinalValid &&
            std::find(d3d12OrdinalValid.try_value()->importedLibraries.begin(),
                      d3d12OrdinalValid.try_value()->importedLibraries.end(),
                      "d3d12.dll") != d3d12OrdinalValid.try_value()->importedLibraries.end() &&
            d3d12OrdinalIsAddressTaken);
    cue::Result<cue::WindowsProductTrustEvidence> unsignedEvidence =
        cue::inspect_windows_product_trust(a_validProduct.generic_string(), a_assertContext);
    require(unsignedEvidence &&
            unsignedEvidence.try_value()->signatureStatus == cue::WindowsProductSignatureStatus::Unsigned);
    require(!cue::validate_windows_shipping_product_security(a_validProduct.generic_string(), signedProfile,
                                                             a_assertContext));
    require(!cue::validate_windows_shipping_product_security(a_unhardenedProduct.generic_string(), localProfile,
                                                             a_assertContext));
    require(!cue::validate_windows_shipping_product_security(a_loaderProduct.generic_string(), localProfile,
                                                             a_assertContext));
    require(!cue::validate_windows_shipping_product_security(a_packagedLoaderProduct.generic_string(), localProfile,
                                                             a_assertContext));

    std::vector<std::byte> bytes = read_bytes(a_validProduct);
    clear_dynamic_base(bytes);
    const std::filesystem::path missingAslr = directory / "MissingAslr.exe";
    write_bytes(missingAslr, bytes);
    require(
        !cue::validate_windows_shipping_product_security(missingAslr.generic_string(), localProfile, a_assertContext));

    bytes = read_bytes(a_validProduct);
    clear_large_address_aware(bytes);
    const std::filesystem::path missingLargeAddressAware = directory / "MissingLargeAddressAware.exe";
    write_bytes(missingLargeAddressAware, bytes);
    require(!cue::validate_windows_shipping_product_security(missingLargeAddressAware.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    replace_import_library(bytes);
    const std::filesystem::path unknownImport = directory / "UnknownImport.exe";
    write_bytes(unknownImport, bytes);
    require(!cue::validate_windows_shipping_product_security(unknownImport.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_first_import_ordinal(bytes, 101U);
    const std::filesystem::path unknownOrdinalImport = directory / "UnknownOrdinalImport.exe";
    write_bytes(unknownOrdinalImport, bytes);
    require(!cue::validate_windows_shipping_product_security(unknownOrdinalImport.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = d3d12OrdinalBytes;
    require(replace_import_ordinal(bytes, "d3d12.dll", 101U, 102U));
    const std::filesystem::path unknownD3d12OrdinalImport = directory / "UnknownD3d12OrdinalImport.exe";
    write_bytes(unknownD3d12OrdinalImport, bytes);
    require(!cue::validate_windows_shipping_product_security(unknownD3d12OrdinalImport.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = d3d12OrdinalBytes;
    require(replace_import_ordinal(bytes, "d3d12.dll", 101U, (1ULL << 16U) | 101U));
    const std::filesystem::path reservedD3d12OrdinalBits = directory / "ReservedD3D12OrdinalBits.exe";
    write_bytes(reservedD3d12OrdinalBits, bytes);
    require(!cue::validate_windows_shipping_product_security(reservedD3d12OrdinalBits.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_first_import_timestamp(bytes, 1U);
    const std::filesystem::path boundImportDescriptor = directory / "BoundImportDescriptor.exe";
    write_bytes(boundImportDescriptor, bytes);
    require(!cue::validate_windows_shipping_product_security(boundImportDescriptor.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_bound_import_directory(bytes);
    const std::filesystem::path boundImportDirectory = directory / "BoundImportDirectory.exe";
    write_bytes(boundImportDirectory, bytes);
    require(!cue::validate_windows_shipping_product_security(boundImportDirectory.generic_string(), localProfile,
                                                             a_assertContext));

    IMAGE_OPTIONAL_HEADER64 d3d12Optional{};
    std::memcpy(&d3d12Optional, d3d12OrdinalBytes.data() + optional_header_offset(d3d12OrdinalBytes),
                sizeof(d3d12Optional));
    const IMAGE_DATA_DIRECTORY d3d12IatDirectory = d3d12Optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    const IMAGE_DATA_DIRECTORY d3d12LoadDirectory =
        d3d12Optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    require(d3d12IatDirectory.Size >= 2U * sizeof(IMAGE_THUNK_DATA64));

    bytes = d3d12OrdinalBytes;
    set_iat_directory(bytes, d3d12IatDirectory.VirtualAddress, k_oversizedIatDirectoryBytes);
    const std::filesystem::path oversizedIatDirectory = directory / "OversizedIatDirectory.exe";
    write_bytes(oversizedIatDirectory, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> oversizedIatDirectoryResult =
        cue::validate_windows_shipping_product_security(oversizedIatDirectory.generic_string(), localProfile,
                                                        a_assertContext);
    require(!oversizedIatDirectoryResult &&
            oversizedIatDirectoryResult.try_error()->summary() ==
                "Shipping Product IAT directory exceeds the M17 resource limit");

    bytes = d3d12OrdinalBytes;
    require(d3d12IatDirectory.VirtualAddress <=
            std::numeric_limits<std::uint32_t>::max() - d3d12IatDirectory.Size);
    set_guard_address_taken_iat_entry(bytes, 1U,
                                      d3d12IatDirectory.VirtualAddress + d3d12IatDirectory.Size);
    const std::filesystem::path addressTakenOutsideIat = directory / "AddressTakenOutsideIat.exe";
    write_bytes(addressTakenOutsideIat, bytes);
    require(!cue::validate_windows_shipping_product_security(addressTakenOutsideIat.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = d3d12OrdinalBytes;
    set_guard_address_taken_iat_entry(bytes, 0U, guard_address_taken_iat_entry(bytes, 0U) + 1U);
    const std::filesystem::path unalignedAddressTakenIat = directory / "UnalignedAddressTakenIat.exe";
    write_bytes(unalignedAddressTakenIat, bytes);
    require(!cue::validate_windows_shipping_product_security(unalignedAddressTakenIat.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = d3d12OrdinalBytes;
    set_guard_address_taken_iat_entry(bytes, 1U, guard_address_taken_iat_entry(bytes, 0U));
    const std::filesystem::path unorderedAddressTakenIat = directory / "UnorderedAddressTakenIat.exe";
    write_bytes(unorderedAddressTakenIat, bytes);
    require(!cue::validate_windows_shipping_product_security(unorderedAddressTakenIat.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = d3d12OrdinalBytes;
    set_guard_address_taken_iat_entry(bytes, d3d12AddressTable.count - 1U, d3d12Ordinal.terminatorRva);
    const std::filesystem::path addressTakenIatTerminator = directory / "AddressTakenIatTerminator.exe";
    write_bytes(addressTakenIatTerminator, bytes);
    require(!cue::validate_windows_shipping_product_security(addressTakenIatTerminator.generic_string(), localProfile,
                                                             a_assertContext));

    std::vector<std::byte> isolatedAddressTableBytes = d3d12OrdinalBytes;
    const std::uint32_t isolatedAddressTableRva =
        relocate_guard_address_taken_iat_table(isolatedAddressTableBytes);
    const std::filesystem::path isolatedAddressTakenIat = directory / "IsolatedAddressTakenIat.exe";
    write_bytes(isolatedAddressTakenIat, isolatedAddressTableBytes);
    require(cue::validate_windows_shipping_product_security(isolatedAddressTakenIat.generic_string(), localProfile,
                                                            a_assertContext)
                .has_value());

    bytes = isolatedAddressTableBytes;
    add_section_characteristics_for_rva(bytes, isolatedAddressTableRva, IMAGE_SCN_MEM_WRITE);
    const std::filesystem::path writableAddressTakenIat = directory / "WritableAddressTakenIat.exe";
    write_bytes(writableAddressTakenIat, bytes);
    require(!cue::validate_windows_shipping_product_security(writableAddressTakenIat.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = isolatedAddressTableBytes;
    add_section_characteristics_for_rva(bytes, isolatedAddressTableRva, IMAGE_SCN_MEM_EXECUTE);
    const std::filesystem::path executableAddressTakenIat = directory / "ExecutableAddressTakenIat.exe";
    write_bytes(executableAddressTakenIat, bytes);
    require(!cue::validate_windows_shipping_product_security(executableAddressTakenIat.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = isolatedAddressTableBytes;
    const GuardTableView isolatedAddressTable = guard_address_taken_iat_table(bytes);
    const std::uint32_t lastAddressEntryRva =
        isolatedAddressTable.rva +
        static_cast<std::uint32_t>((isolatedAddressTable.count - 1U) * isolatedAddressTable.stride);
    const std::uint32_t overlappingRelocationRva = lastAddressEntryRva + sizeof(std::uint32_t) - 1U;
    set_image_rva_value(bytes, overlappingRelocationRva, d3d12Optional.ImageBase);
    append_dir64_relocation(bytes, overlappingRelocationRva);
    const std::filesystem::path relocatedAddressTakenIat = directory / "RelocatedAddressTakenIat.exe";
    write_bytes(relocatedAddressTakenIat, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> relocatedAddressTakenIatResult =
        cue::validate_windows_shipping_product_security(relocatedAddressTakenIat.generic_string(), localProfile,
                                                        a_assertContext);
    require(!relocatedAddressTakenIatResult &&
            relocatedAddressTakenIatResult.try_error()->summary() ==
                "Shipping Product load configuration does not satisfy CFG, stack, and dependent-load policy");

    bytes = d3d12OrdinalBytes;
    clear_dir64_relocation(
        bytes, d3d12LoadDirectory.VirtualAddress +
                   static_cast<std::uint32_t>(
                       offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryTable)));
    const std::filesystem::path unrelocatedAddressTakenIatPointer =
        directory / "UnrelocatedAddressTakenIatPointer.exe";
    write_bytes(unrelocatedAddressTakenIatPointer, bytes);
    require(!cue::validate_windows_shipping_product_security(unrelocatedAddressTakenIatPointer.generic_string(),
                                                             localProfile, a_assertContext));

    bytes = d3d12OrdinalBytes;
    rewrite_guard_tables_with_metadata_stride(bytes, 1U);
    const std::filesystem::path validAddressTakenIatMetadata = directory / "ValidAddressTakenIatMetadata.exe";
    write_bytes(validAddressTakenIatMetadata, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> validAddressTakenMetadataResult =
        cue::validate_windows_shipping_product_security(validAddressTakenIatMetadata.generic_string(), localProfile,
                                                        a_assertContext);
    if (!validAddressTakenMetadataResult)
    {
        std::fprintf(stderr, "metadata stride validation failed: %.*s\n",
                     static_cast<int>(validAddressTakenMetadataResult.try_error()->summary().size()),
                     validAddressTakenMetadataResult.try_error()->summary().data());
    }
    require(validAddressTakenMetadataResult.has_value());
    set_guard_address_taken_iat_metadata(bytes, 0U, 0U, std::byte{1U});
    const std::filesystem::path nonzeroAddressTakenIatMetadata = directory / "NonzeroAddressTakenIatMetadata.exe";
    write_bytes(nonzeroAddressTakenIatMetadata, bytes);
    require(!cue::validate_windows_shipping_product_security(nonzeroAddressTakenIatMetadata.generic_string(),
                                                             localProfile, a_assertContext));

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 emptyLongJumpOptional{};
    std::memcpy(&emptyLongJumpOptional, bytes.data() + optional_header_offset(bytes), sizeof(emptyLongJumpOptional));
    append_dir64_relocation(
        bytes, emptyLongJumpOptional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress +
                   static_cast<std::uint32_t>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardLongJumpTargetTable)));
    const std::filesystem::path relocatedEmptyLongJumpPointer = directory / "RelocatedEmptyLongJumpPointer.exe";
    write_bytes(relocatedEmptyLongJumpPointer, bytes);
    require(!cue::validate_windows_shipping_product_security(relocatedEmptyLongJumpPointer.generic_string(),
                                                             localProfile, a_assertContext));

    std::error_code error;
    bytes = read_bytes(a_validProduct);
    clear_load_configuration_rva(bytes);
    const std::filesystem::path missingLoadConfiguration = directory / "MissingLoadConfiguration.exe";
    write_bytes(missingLoadConfiguration, bytes);
    require(!cue::validate_windows_shipping_product_security(missingLoadConfiguration.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    mark_relocations_stripped(bytes);
    const std::filesystem::path strippedRelocations = directory / "StrippedRelocations.exe";
    write_bytes(strippedRelocations, bytes);
    require(!cue::validate_windows_shipping_product_security(strippedRelocations.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    exceed_section_count_limit(bytes);
    const std::filesystem::path excessiveSections = directory / "ExcessiveSections.exe";
    write_bytes(excessiveSections, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> excessiveSectionsResult =
        cue::validate_windows_shipping_product_security(excessiveSections.generic_string(), localProfile,
                                                        a_assertContext);
    require(!excessiveSectionsResult && excessiveSectionsResult.try_error()->summary() ==
                                            "Shipping Product section count exceeds the M17 resource limit");

    bytes = read_bytes(a_validProduct);
    truncate_relocation_directory(bytes);
    const std::filesystem::path truncatedRelocations = directory / "TruncatedRelocations.exe";
    write_bytes(truncatedRelocations, bytes);
    require(!cue::validate_windows_shipping_product_security(truncatedRelocations.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    exceed_relocation_directory_limit(bytes);
    const std::filesystem::path oversizedRelocationDirectory = directory / "OversizedRelocationDirectory.exe";
    write_bytes(oversizedRelocationDirectory, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> oversizedRelocationResult =
        cue::validate_windows_shipping_product_security(oversizedRelocationDirectory.generic_string(), localProfile,
                                                        a_assertContext);
    require(!oversizedRelocationResult &&
            oversizedRelocationResult.try_error()->summary() ==
                "Shipping Product base relocation directory exceeds the M17 resource limit");

    bytes = read_bytes(a_validProduct);
    truncate_image_at_relocation_directory(bytes);
    const std::filesystem::path relocationOutsideImage = directory / "RelocationCrossesImageEnd.exe";
    write_bytes(relocationOutsideImage, bytes);
    require(!cue::validate_windows_shipping_product_security(relocationOutsideImage.generic_string(), localProfile,
                                                             a_assertContext));

    const std::array<std::uint32_t, 6U> requiredRelocations = required_security_relocation_rvas(bytes);
    for (std::size_t index = 0U; index < requiredRelocations.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        clear_dir64_relocation(bytes, requiredRelocations[index]);
        const std::filesystem::path missingSecurityRelocation =
            directory / ("MissingSecurityRelocation-" + std::to_string(index) + ".exe");
        write_bytes(missingSecurityRelocation, bytes);
        require(!cue::validate_windows_shipping_product_security(missingSecurityRelocation.generic_string(),
                                                                 localProfile, a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    require(duplicate_dir64_relocation(bytes, requiredRelocations.front(), requiredRelocations));
    const std::filesystem::path duplicateSecurityRelocation = directory / "DuplicateSecurityRelocation.exe";
    write_bytes(duplicateSecurityRelocation, bytes);
    require(!cue::validate_windows_shipping_product_security(duplicateSecurityRelocation.generic_string(), localProfile,
                                                             a_assertContext));

    for (std::uint16_t offset = 1U; offset <= 7U; ++offset)
    {
        bytes = read_bytes(a_validProduct);
        overlap_dir64_relocation(bytes, requiredRelocations, offset);
        const std::filesystem::path overlappingSecurityRelocation =
            directory / ("OverlappingSecurityRelocation-Offset" + std::to_string(offset) + ".exe");
        write_bytes(overlappingSecurityRelocation, bytes);
        require(!cue::validate_windows_shipping_product_security(overlappingSecurityRelocation.generic_string(),
                                                                 localProfile, a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    cross_header_boundary_for_load_configuration(bytes);
    const std::filesystem::path headerBoundary = directory / "HeaderBoundary.exe";
    write_bytes(headerBoundary, bytes);
    require(!cue::validate_windows_shipping_product_security(headerBoundary.generic_string(), localProfile,
                                                             a_assertContext));

    constexpr std::array<std::size_t, 4U> loadPointerOffsets = {
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable)};
    const std::vector<ULONGLONG> invalidPointerValues = invalid_load_configuration_pointer_values(bytes);
    for (std::size_t fieldIndex = 0U; fieldIndex < loadPointerOffsets.size(); ++fieldIndex)
    {
        for (std::size_t valueIndex = 0U; valueIndex < invalidPointerValues.size(); ++valueIndex)
        {
            bytes = read_bytes(a_validProduct);
            set_load_configuration_pointer(bytes, loadPointerOffsets[fieldIndex], invalidPointerValues[valueIndex]);
            const std::filesystem::path invalidLoadPointer =
                directory /
                ("InvalidLoadPointer-" + std::to_string(fieldIndex) + "-" + std::to_string(valueIndex) + ".exe");
            write_bytes(invalidLoadPointer, bytes);
            require(!cue::validate_windows_shipping_product_security(invalidLoadPointer.generic_string(), localProfile,
                                                                     a_assertContext));
        }
    }

    bytes = read_bytes(a_validProduct);
    const ULONGLONG securityCookie =
        load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie));
    const ULONGLONG guardCheck =
        load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer));
    const ULONGLONG guardDispatch =
        load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer));
    const ULONGLONG guardFunctionTable =
        load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable));
    constexpr ULONGLONG msvcX64DefaultSecurityCookie = 0x00002B992DDFA232ULL;
    require(image_va_value(bytes, securityCookie) == msvcX64DefaultSecurityCookie);
    const ULONGLONG guardCheckTarget = image_va_value(bytes, guardCheck);
    const ULONGLONG guardDispatchTarget = image_va_value(bytes, guardDispatch);

    constexpr std::array<std::size_t, 2U> unsupportedGuardTableOffsets = {
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryTable),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardLongJumpTargetTable)};
    for (std::size_t index = 0U; index < unsupportedGuardTableOffsets.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        set_load_configuration_pointer(bytes, unsupportedGuardTableOffsets[index], guardFunctionTable);
        const std::filesystem::path unsupportedGuardTable =
            directory / ("UnsupportedGuardTable-" + std::to_string(index) + ".exe");
        write_bytes(unsupportedGuardTable, bytes);
        require(!cue::validate_windows_shipping_product_security(unsupportedGuardTable.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 auxiliaryGuardOptional{};
    std::memcpy(&auxiliaryGuardOptional, bytes.data() + optional_header_offset(bytes), sizeof(auxiliaryGuardOptional));
    const IMAGE_DATA_DIRECTORY &auxiliaryLoadDirectory =
        auxiliaryGuardOptional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr std::array<std::size_t, 2U> unsupportedGuardCountOffsets = {
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryCount),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardLongJumpTargetCount)};
    for (std::size_t index = 0U; index < unsupportedGuardCountOffsets.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        set_load_configuration_ulonglong(bytes, unsupportedGuardCountOffsets[index], auxiliaryGuardOptional.ImageBase);
        require(unsupportedGuardCountOffsets[index] <=
                std::numeric_limits<std::uint32_t>::max() - auxiliaryLoadDirectory.VirtualAddress);
        append_dir64_relocation(bytes, auxiliaryLoadDirectory.VirtualAddress +
                                           static_cast<std::uint32_t>(unsupportedGuardCountOffsets[index]));
        const std::filesystem::path unsupportedGuardCount =
            directory / ("UnsupportedGuardCount-" + std::to_string(index) + ".exe");
        write_bytes(unsupportedGuardCount, bytes);
        require(!cue::validate_windows_shipping_product_security(unsupportedGuardCount.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    constexpr std::array<DWORD, 7U> unsupportedGuardFlags = {
        IMAGE_GUARD_RF_INSTRUMENTED, IMAGE_GUARD_RF_ENABLE,
        IMAGE_GUARD_RF_STRICT,       IMAGE_GUARD_EH_CONTINUATION_TABLE_PRESENT,
        IMAGE_GUARD_XFG_ENABLED,     IMAGE_GUARD_CASTGUARD_PRESENT,
        IMAGE_GUARD_MEMCPY_PRESENT};
    for (std::size_t index = 0U; index < unsupportedGuardFlags.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        if (unsupportedGuardFlags[index] == IMAGE_GUARD_EH_CONTINUATION_TABLE_PRESENT)
        {
            set_load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardEHContinuationTable),
                                           guardFunctionTable);
            set_load_configuration_ulonglong(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardEHContinuationCount),
                                             1U);
        }
        add_guard_flag(bytes, unsupportedGuardFlags[index]);
        const std::filesystem::path unsupportedGuardFlag =
            directory / ("UnsupportedGuardFlag-" + std::to_string(index) + ".exe");
        write_bytes(unsupportedGuardFlag, bytes);
        require(!cue::validate_windows_shipping_product_security(unsupportedGuardFlag.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    set_image_va_value(bytes, securityCookie, msvcX64DefaultSecurityCookie + 2U);
    const std::filesystem::path predictableSecurityCookie = directory / "PredictableSecurityCookie.exe";
    write_bytes(predictableSecurityCookie, bytes);
    require(!cue::validate_windows_shipping_product_security(predictableSecurityCookie.generic_string(), localProfile,
                                                             a_assertContext));

    IMAGE_OPTIONAL_HEADER64 securityCookieOptional{};
    std::memcpy(&securityCookieOptional, bytes.data() + optional_header_offset(bytes), sizeof(securityCookieOptional));
    require(securityCookie >= securityCookieOptional.ImageBase &&
            securityCookie - securityCookieOptional.ImageBase <= std::numeric_limits<std::uint32_t>::max());
    const std::uint32_t securityCookieRva =
        static_cast<std::uint32_t>(securityCookie - securityCookieOptional.ImageBase);

    bytes = read_bytes(a_validProduct);
    set_first_import_first_thunk(bytes, securityCookieRva);
    const std::filesystem::path firstThunkOutsideIat = directory / "FirstThunkOutsideIat.exe";
    write_bytes(firstThunkOutsideIat, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> firstThunkOutsideIatResult =
        cue::validate_windows_shipping_product_security(firstThunkOutsideIat.generic_string(), localProfile,
                                                        a_assertContext);
    require(!firstThunkOutsideIatResult &&
            firstThunkOutsideIatResult.try_error()->summary() ==
                "Shipping Product import FirstThunk is outside the declared IAT directory");

    bytes = read_bytes(a_validProduct);
    duplicate_first_import_descriptor(bytes);
    const std::filesystem::path duplicateImportDescriptor = directory / "DuplicateImportDescriptor.exe";
    write_bytes(duplicateImportDescriptor, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> duplicateImportDescriptorResult =
        cue::validate_windows_shipping_product_security(duplicateImportDescriptor.generic_string(), localProfile,
                                                        a_assertContext);
    require(!duplicateImportDescriptorResult &&
            duplicateImportDescriptorResult.try_error()->summary() ==
                "Shipping Product import descriptors overlap a nonnull IAT slot");

    bytes = read_bytes(a_validProduct);
    set_first_import_first_thunk(bytes, securityCookieRva);
    set_iat_directory(bytes, securityCookieRva, sizeof(IMAGE_THUNK_DATA64));
    const std::filesystem::path iatOverlapsSecurityCookie = directory / "IatOverlapsSecurityCookie.exe";
    write_bytes(iatOverlapsSecurityCookie, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> iatOverlapsSecurityCookieResult =
        cue::validate_windows_shipping_product_security(iatOverlapsSecurityCookie.generic_string(), localProfile,
                                                        a_assertContext);
    require(!iatOverlapsSecurityCookieResult &&
            iatOverlapsSecurityCookieResult.try_error()->summary() ==
                "Shipping Product IAT directory overlaps security or loader metadata");

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 iatOptional{};
    std::memcpy(&iatOptional, bytes.data() + optional_header_offset(bytes), sizeof(iatOptional));
    const IMAGE_DATA_DIRECTORY &importDirectoryForIat = iatOptional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    set_iat_directory(bytes, importDirectoryForIat.VirtualAddress, importDirectoryForIat.Size);
    const std::filesystem::path iatOverlapsImportDirectory = directory / "IatOverlapsImportDirectory.exe";
    write_bytes(iatOverlapsImportDirectory, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> iatOverlapsImportDirectoryResult =
        cue::validate_windows_shipping_product_security(iatOverlapsImportDirectory.generic_string(), localProfile,
                                                        a_assertContext);
    require(!iatOverlapsImportDirectoryResult &&
            iatOverlapsImportDirectoryResult.try_error()->summary() ==
                "Shipping Product IAT directory overlaps security or loader metadata");

    bytes = read_bytes(a_validProduct);
    set_iat_directory(bytes, iatOptional.AddressOfEntryPoint, sizeof(IMAGE_THUNK_DATA64));
    const std::filesystem::path executableIat = directory / "ExecutableIat.exe";
    write_bytes(executableIat, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> executableIatResult =
        cue::validate_windows_shipping_product_security(executableIat.generic_string(), localProfile, a_assertContext);
    require(!executableIatResult &&
            executableIatResult.try_error()->summary() == "Shipping Product IAT directory is invalid");

    bytes = read_bytes(a_validProduct);
    const std::array<std::uint32_t, 4U> iatImportMetadataRvas = first_import_metadata_rvas(bytes);
    set_first_import_first_thunk(bytes, iatImportMetadataRvas[3U]);
    set_iat_directory(bytes, iatImportMetadataRvas[3U], sizeof(IMAGE_THUNK_DATA64));
    const std::filesystem::path iatOverlapsImportName = directory / "IatOverlapsImportName.exe";
    write_bytes(iatOverlapsImportName, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> iatOverlapsImportNameResult =
        cue::validate_windows_shipping_product_security(iatOverlapsImportName.generic_string(), localProfile,
                                                        a_assertContext);
    require(!iatOverlapsImportNameResult && iatOverlapsImportNameResult.try_error()->summary() ==
                                                "Shipping Product IAT directory overlaps import loader metadata");

    bytes = read_bytes(a_validProduct);
    const std::array<std::uint32_t, 4U> iatLookupMetadataRvas = first_import_metadata_rvas(bytes);
    require(iatLookupMetadataRvas[2U] <= std::numeric_limits<std::uint32_t>::max() - sizeof(IMAGE_THUNK_DATA64));
    set_first_import_original_thunk(bytes, iatLookupMetadataRvas[2U] + static_cast<DWORD>(sizeof(IMAGE_THUNK_DATA64)));
    const std::filesystem::path iatOverlapsLookupThunk = directory / "IatOverlapsLookupThunk.exe";
    write_bytes(iatOverlapsLookupThunk, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> iatOverlapsLookupThunkResult =
        cue::validate_windows_shipping_product_security(iatOverlapsLookupThunk.generic_string(), localProfile,
                                                        a_assertContext);
    require(!iatOverlapsLookupThunkResult && iatOverlapsLookupThunkResult.try_error()->summary() ==
                                                 "Shipping Product IAT directory overlaps import loader metadata");

    bytes = read_bytes(a_validProduct);
    const std::array<std::uint32_t, 4U> iatLibraryMetadataRvas = first_import_metadata_rvas(bytes);
    set_iat_directory(bytes, iatLibraryMetadataRvas[0U], sizeof(IMAGE_THUNK_DATA64));
    const std::filesystem::path iatOverlapsLibraryName = directory / "IatOverlapsLibraryName.exe";
    write_bytes(iatOverlapsLibraryName, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> iatOverlapsLibraryNameResult =
        cue::validate_windows_shipping_product_security(iatOverlapsLibraryName.generic_string(), localProfile,
                                                        a_assertContext);
    require(!iatOverlapsLibraryNameResult && iatOverlapsLibraryNameResult.try_error()->summary() ==
                                                 "Shipping Product IAT directory overlaps import loader metadata");

    bytes = read_bytes(a_validProduct);
    set_first_import_original_thunk(bytes, 0U);
    const std::filesystem::path importLookupUsesIat = directory / "ImportLookupUsesIat.exe";
    write_bytes(importLookupUsesIat, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> importLookupUsesIatResult =
        cue::validate_windows_shipping_product_security(importLookupUsesIat.generic_string(), localProfile,
                                                        a_assertContext);
    require(importLookupUsesIatResult.has_value());

    for (std::uint16_t offset = 0U; offset <= 7U; ++offset)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, securityCookieRva + offset);
        const std::filesystem::path relocatedSecurityCookie =
            directory / ("RelocatedSecurityCookie-Offset" + std::to_string(offset) + ".exe");
        write_bytes(relocatedSecurityCookie, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedSecurityCookie.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    const std::uint32_t guardFunctionTableRva = guard_function_table_rva(bytes);
    const std::size_t guardFunctionTableSize = guard_function_table_size(bytes);
    require(guardFunctionTableRva >= 7U && guardFunctionTableSize > 1U &&
            guardFunctionTableSize - 1U <= std::numeric_limits<std::uint32_t>::max() - guardFunctionTableRva);
    const std::array<std::uint32_t, 5U> relocatedFunctionTableRvas = {
        guardFunctionTableRva - 7U, guardFunctionTableRva - 1U, guardFunctionTableRva,
        guardFunctionTableRva + static_cast<std::uint32_t>(guardFunctionTableSize / 2U),
        guardFunctionTableRva + static_cast<std::uint32_t>(guardFunctionTableSize - 1U)};
    for (std::size_t index = 0U; index < relocatedFunctionTableRvas.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, relocatedFunctionTableRvas[index]);
        const std::filesystem::path relocatedFunctionTable =
            directory / ("RelocatedGuardFunctionTable-" + std::to_string(index) + ".exe");
        write_bytes(relocatedFunctionTable, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedFunctionTable.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    IMAGE_OPTIONAL_HEADER64 loadControlOptional{};
    std::memcpy(&loadControlOptional, bytes.data() + optional_header_offset(bytes), sizeof(loadControlOptional));
    const IMAGE_DATA_DIRECTORY &loadControlDirectory =
        loadControlOptional.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t loadDirectoryDescriptorRva = optional_header_offset(bytes) +
                                                   offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
                                                   IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG * sizeof(IMAGE_DATA_DIRECTORY);
    require(loadDirectoryDescriptorRva <=
            std::numeric_limits<std::uint32_t>::max() - (sizeof(IMAGE_DATA_DIRECTORY) - 1U));
    for (std::size_t byteIndex = 0U; byteIndex < sizeof(IMAGE_DATA_DIRECTORY); ++byteIndex)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, static_cast<std::uint32_t>(loadDirectoryDescriptorRva + byteIndex));
        const std::filesystem::path relocatedLoadDirectory =
            directory / ("RelocatedLoadDirectory-" + std::to_string(byteIndex) + ".exe");
        write_bytes(relocatedLoadDirectory, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedLoadDirectory.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 relocationOptional{};
    std::memcpy(&relocationOptional, bytes.data() + optional_header_offset(bytes), sizeof(relocationOptional));
    const IMAGE_DATA_DIRECTORY &relocationDirectory = relocationOptional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    require(relocationDirectory.VirtualAddress >= 7U && relocationDirectory.Size > 1U &&
            relocationDirectory.Size - 1U <=
                std::numeric_limits<std::uint32_t>::max() - relocationDirectory.VirtualAddress);
    const std::array<std::uint32_t, 5U> relocatedRelocationDirectoryRvas = {
        relocationDirectory.VirtualAddress - 7U, relocationDirectory.VirtualAddress - 1U,
        relocationDirectory.VirtualAddress,
        relocationDirectory.VirtualAddress + static_cast<std::uint32_t>(relocationDirectory.Size / 2U),
        relocationDirectory.VirtualAddress + relocationDirectory.Size - 1U};
    for (std::size_t index = 0U; index < relocatedRelocationDirectoryRvas.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, relocatedRelocationDirectoryRvas[index]);
        const std::filesystem::path relocatedRelocationDirectory =
            directory / ("RelocatedRelocationDirectory-" + std::to_string(index) + ".exe");
        write_bytes(relocatedRelocationDirectory, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedRelocationDirectory.generic_string(),
                                                                 localProfile, a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 relocationValueOptional{};
    std::memcpy(&relocationValueOptional, bytes.data() + optional_header_offset(bytes),
                sizeof(relocationValueOptional));
    require(relocationValueOptional.ImageBase > 0U &&
            relocationValueOptional.ImageBase <=
                std::numeric_limits<std::uint64_t>::max() - relocationValueOptional.SizeOfImage);
    const std::array<std::uint64_t, 3U> invalidRelocationValues = {0U, relocationValueOptional.ImageBase - 1U,
                                                                   relocationValueOptional.ImageBase +
                                                                       relocationValueOptional.SizeOfImage};
    for (std::size_t index = 0U; index < invalidRelocationValues.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        set_image_rva_value(bytes, relocationValueOptional.AddressOfEntryPoint, invalidRelocationValues[index]);
        append_dir64_relocation(bytes, relocationValueOptional.AddressOfEntryPoint);
        const std::filesystem::path invalidRelocationValue =
            directory / ("InvalidRelocationValue-" + std::to_string(index) + ".exe");
        write_bytes(invalidRelocationValue, bytes);
        require(!cue::validate_windows_shipping_product_security(invalidRelocationValue.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 importOptional{};
    std::memcpy(&importOptional, bytes.data() + optional_header_offset(bytes), sizeof(importOptional));
    const IMAGE_DATA_DIRECTORY &importDirectory = importOptional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    require(importDirectory.VirtualAddress >= 7U && importDirectory.Size > 1U &&
            importDirectory.Size - 1U <= std::numeric_limits<std::uint32_t>::max() - importDirectory.VirtualAddress);
    const std::array<std::uint32_t, 5U> relocatedImportDirectoryRvas = {
        importDirectory.VirtualAddress - 7U, importDirectory.VirtualAddress - 1U, importDirectory.VirtualAddress,
        importDirectory.VirtualAddress + static_cast<std::uint32_t>(importDirectory.Size / 2U),
        importDirectory.VirtualAddress + importDirectory.Size - 1U};
    for (std::size_t index = 0U; index < relocatedImportDirectoryRvas.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, relocatedImportDirectoryRvas[index]);
        const std::filesystem::path relocatedImportDirectory =
            directory / ("RelocatedImportDirectory-" + std::to_string(index) + ".exe");
        write_bytes(relocatedImportDirectory, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedImportDirectory.generic_string(),
                                                                 localProfile, a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    const std::array<std::uint32_t, 4U> importMetadataRvas = first_import_metadata_rvas(bytes);
    for (std::size_t index = 0U; index < importMetadataRvas.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, importMetadataRvas[index]);
        const std::filesystem::path relocatedImportMetadata =
            directory / ("RelocatedImportMetadata-" + std::to_string(index) + ".exe");
        write_bytes(relocatedImportMetadata, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedImportMetadata.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    bytes = read_bytes(a_validProduct);
    const std::array<std::uint32_t, 2U> cetMetadataRvas = cet_metadata_rvas(bytes);
    for (std::size_t index = 0U; index < cetMetadataRvas.size(); ++index)
    {
        bytes = read_bytes(a_validProduct);
        append_dir64_relocation(bytes, cetMetadataRvas[index]);
        const std::filesystem::path relocatedCetMetadata =
            directory / ("RelocatedCetMetadata-" + std::to_string(index) + ".exe");
        write_bytes(relocatedCetMetadata, bytes);
        require(!cue::validate_windows_shipping_product_security(relocatedCetMetadata.generic_string(), localProfile,
                                                                 a_assertContext));
    }

    constexpr std::array<std::pair<std::size_t, std::size_t>, 6U> unrelocatedLoadConfigurationControls = {
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, Size), sizeof(DWORD)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionCount), sizeof(ULONGLONG)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags), sizeof(DWORD)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryCount), sizeof(ULONGLONG)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardLongJumpTargetCount), sizeof(ULONGLONG)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DependentLoadFlags), sizeof(WORD)}};
    for (std::size_t fieldIndex = 0U; fieldIndex < unrelocatedLoadConfigurationControls.size(); ++fieldIndex)
    {
        for (std::size_t byteIndex = 0U; byteIndex < unrelocatedLoadConfigurationControls[fieldIndex].second;
             ++byteIndex)
        {
            bytes = read_bytes(a_validProduct);
            append_dir64_relocation(bytes, loadControlDirectory.VirtualAddress +
                                               static_cast<std::uint32_t>(
                                                   unrelocatedLoadConfigurationControls[fieldIndex].first + byteIndex));
            const std::filesystem::path relocatedLoadConfigurationControl =
                directory / ("RelocatedLoadConfigurationControl-" + std::to_string(fieldIndex) + "-" +
                             std::to_string(byteIndex) + ".exe");
            write_bytes(relocatedLoadConfigurationControl, bytes);
            require(!cue::validate_windows_shipping_product_security(relocatedLoadConfigurationControl.generic_string(),
                                                                     localProfile, a_assertContext));
        }
    }

    bytes = read_bytes(a_validProduct);
    set_load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie), guardCheck);
    const std::filesystem::path readOnlySecurityCookie = directory / "ReadOnlySecurityCookie.exe";
    write_bytes(readOnlySecurityCookie, bytes);
    require(!cue::validate_windows_shipping_product_security(readOnlySecurityCookie.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    mark_security_cookie_section_shared(bytes);
    const std::filesystem::path sharedSecurityCookie = directory / "SharedSecurityCookie.exe";
    write_bytes(sharedSecurityCookie, bytes);
    require(!cue::validate_windows_shipping_product_security(sharedSecurityCookie.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_image_va_value(bytes, securityCookie, guardCheckTarget);
    set_load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer),
                                   securityCookie);
    const std::filesystem::path writableGuardCheck = directory / "WritableGuardCheck.exe";
    write_bytes(writableGuardCheck, bytes);
    require(!cue::validate_windows_shipping_product_security(writableGuardCheck.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_image_va_value(bytes, securityCookie, guardDispatchTarget);
    set_load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer),
                                   securityCookie);
    const std::filesystem::path writableGuardDispatch = directory / "WritableGuardDispatch.exe";
    write_bytes(writableGuardDispatch, bytes);
    require(!cue::validate_windows_shipping_product_security(writableGuardDispatch.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_image_va_value(bytes, guardCheck, securityCookie);
    const std::filesystem::path nonExecutableGuardCheckTarget = directory / "NonExecutableGuardCheckTarget.exe";
    write_bytes(nonExecutableGuardCheckTarget, bytes);
    require(!cue::validate_windows_shipping_product_security(nonExecutableGuardCheckTarget.generic_string(),
                                                             localProfile, a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_image_va_value(bytes, guardDispatch, securityCookie);
    const std::filesystem::path nonExecutableGuardDispatchTarget = directory / "NonExecutableGuardDispatchTarget.exe";
    write_bytes(nonExecutableGuardDispatchTarget, bytes);
    require(!cue::validate_windows_shipping_product_security(nonExecutableGuardDispatchTarget.generic_string(),
                                                             localProfile, a_assertContext));

    bytes = read_bytes(a_validProduct);
    truncate_load_configuration_internal_size(bytes);
    const std::filesystem::path truncatedInternalLoadSize = directory / "TruncatedInternalLoadSize.exe";
    write_bytes(truncatedInternalLoadSize, bytes);
    require(!cue::validate_windows_shipping_product_security(truncatedInternalLoadSize.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    truncate_load_configuration_directory_size(bytes);
    const std::filesystem::path truncatedDirectoryLoadSize = directory / "TruncatedDirectoryLoadSize.exe";
    write_bytes(truncatedDirectoryLoadSize, bytes);
    require(!cue::validate_windows_shipping_product_security(truncatedDirectoryLoadSize.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    mark_security_cookie_unused(bytes);
    const std::filesystem::path unusedSecurityCookie = directory / "UnusedSecurityCookie.exe";
    write_bytes(unusedSecurityCookie, bytes);
    require(!cue::validate_windows_shipping_product_security(unusedSecurityCookie.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    clear_guard_function_table_flag(bytes);
    const std::filesystem::path functionTableFlagMissing = directory / "MissingFunctionTableFlag.exe";
    write_bytes(functionTableFlagMissing, bytes);
    require(!cue::validate_windows_shipping_product_security(functionTableFlagMissing.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_load_configuration_ulonglong(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionCount),
                                     std::numeric_limits<ULONGLONG>::max());
    const std::filesystem::path functionTableOverrun = directory / "FunctionTableOverrun.exe";
    write_bytes(functionTableOverrun, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> functionTableOverrunResult =
        cue::validate_windows_shipping_product_security(functionTableOverrun.generic_string(), localProfile,
                                                        a_assertContext);
    require(!functionTableOverrunResult && functionTableOverrunResult.try_error()->summary() ==
                                               "Shipping Product CFG function table exceeds the M17 resource limit");

    bytes = read_bytes(a_validProduct);
    mark_guard_function_table_section_executable(bytes);
    const std::filesystem::path executableFunctionTable = directory / "ExecutableFunctionTable.exe";
    write_bytes(executableFunctionTable, bytes);
    require(!cue::validate_windows_shipping_product_security(executableFunctionTable.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    IMAGE_OPTIONAL_HEADER64 functionTargetOptional{};
    std::memcpy(&functionTargetOptional, bytes.data() + optional_header_offset(bytes), sizeof(functionTargetOptional));
    set_guard_function_table_entry(bytes, 0U, functionTargetOptional.SizeOfImage);
    const std::filesystem::path outOfImageFunctionTarget = directory / "OutOfImageFunctionTarget.exe";
    write_bytes(outOfImageFunctionTarget, bytes);
    require(!cue::validate_windows_shipping_product_security(outOfImageFunctionTarget.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_guard_function_table_entry(bytes, 0U, guard_function_table_rva(bytes));
    const std::filesystem::path nonExecutableFunctionTarget = directory / "NonExecutableFunctionTarget.exe";
    write_bytes(nonExecutableFunctionTarget, bytes);
    require(!cue::validate_windows_shipping_product_security(nonExecutableFunctionTarget.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    set_guard_function_table_entry(bytes, 1U, guard_function_table_entry(bytes, 0U));
    const std::filesystem::path duplicateFunctionTarget = directory / "DuplicateFunctionTarget.exe";
    write_bytes(duplicateFunctionTarget, bytes);
    require(!cue::validate_windows_shipping_product_security(duplicateFunctionTarget.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    const ULONGLONG unalignedFunctionTable =
        load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable)) + 1U;
    set_load_configuration_pointer(bytes, offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable),
                                   unalignedFunctionTable);
    const std::filesystem::path unalignedFunctionTablePath = directory / "UnalignedFunctionTable.exe";
    write_bytes(unalignedFunctionTablePath, bytes);
    require(!cue::validate_windows_shipping_product_security(unalignedFunctionTablePath.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    cross_section_boundary_for_import_name(bytes);
    const std::filesystem::path sectionBoundary = directory / "SectionBoundary.exe";
    write_bytes(sectionBoundary, bytes);
    require(!cue::validate_windows_shipping_product_security(sectionBoundary.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    cross_file_boundary_for_cet_data(bytes);
    const std::filesystem::path cetBoundary = directory / "CetBoundary.exe";
    write_bytes(cetBoundary, bytes);
    require(
        !cue::validate_windows_shipping_product_security(cetBoundary.generic_string(), localProfile, a_assertContext));

    bytes = read_bytes(a_validProduct);
    mismatch_cet_data_rva(bytes);
    const std::filesystem::path mismatchedCetData = directory / "MismatchedCetData.exe";
    write_bytes(mismatchedCetData, bytes);
    require(!cue::validate_windows_shipping_product_security(mismatchedCetData.generic_string(), localProfile,
                                                             a_assertContext));

    bytes = read_bytes(a_validProduct);
    exceed_debug_directory_limit(bytes);
    const std::filesystem::path oversizedDebugDirectory = directory / "OversizedDebugDirectory.exe";
    write_bytes(oversizedDebugDirectory, bytes);
    cue::Result<cue::WindowsProductSecurityValidation> oversizedDebugDirectoryResult =
        cue::validate_windows_shipping_product_security(oversizedDebugDirectory.generic_string(), localProfile,
                                                        a_assertContext);
    require(!oversizedDebugDirectoryResult && oversizedDebugDirectoryResult.try_error()->summary() ==
                                                  "Shipping Product debug directory exceeds the M17 resource limit");

    const std::filesystem::path oversizedProduct = directory / "OversizedProduct.exe";
    write_bytes(oversizedProduct, std::span<const std::byte>{});
    std::filesystem::resize_file(oversizedProduct, k_oversizedProductBytes, error);
    require(!error);
    require(!cue::validate_windows_shipping_product_security(oversizedProduct.generic_string(), localProfile,
                                                             a_assertContext));

    std::filesystem::remove_all(directory, error);
    require(!error);
    test_trust_policy(localProfile, signedProfile, a_assertContext);
}
} // namespace

/// @brief Windows Shipping Product Security Policyの機械検証を実行する
int main(int a_argumentCount, char **a_arguments)
{
    require(a_argumentCount == 6);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_product_security(std::filesystem::path(a_arguments[1]), std::filesystem::path(a_arguments[2]),
                          std::filesystem::path(a_arguments[3]), std::filesystem::path(a_arguments[4]),
                          std::filesystem::path(a_arguments[5]), assertContext);
    return 0;
}
