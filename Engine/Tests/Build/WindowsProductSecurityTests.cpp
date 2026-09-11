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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_publisherKeyId = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::uintmax_t k_oversizedProductBytes = 512ULL * 1024ULL * 1024ULL + 1ULL;

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

/// @brief 条件不成立時にTest Processを終了する
void require(bool a_condition)
{
    if (!a_condition)
    {
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

/// @brief Base Relocation DirectoryをBlock Header未満へ切り詰める
void truncate_relocation_directory(std::vector<std::byte> &a_bytes)
{
    const std::size_t optionalOffset = optional_header_offset(a_bytes);
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::memcpy(&optional, a_bytes.data() + optionalOffset, sizeof(optional));
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = sizeof(IMAGE_BASE_RELOCATION) - 1U;
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

    const std::filesystem::path directory = create_test_directory();
    std::vector<std::byte> bytes = read_bytes(a_validProduct);
    clear_dynamic_base(bytes);
    const std::filesystem::path missingAslr = directory / "MissingAslr.exe";
    write_bytes(missingAslr, bytes);
    require(
        !cue::validate_windows_shipping_product_security(missingAslr.generic_string(), localProfile, a_assertContext));

    bytes = read_bytes(a_validProduct);
    replace_import_library(bytes);
    const std::filesystem::path unknownImport = directory / "UnknownImport.exe";
    write_bytes(unknownImport, bytes);
    require(!cue::validate_windows_shipping_product_security(unknownImport.generic_string(), localProfile,
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
    truncate_relocation_directory(bytes);
    const std::filesystem::path truncatedRelocations = directory / "TruncatedRelocations.exe";
    write_bytes(truncatedRelocations, bytes);
    require(!cue::validate_windows_shipping_product_security(truncatedRelocations.generic_string(), localProfile,
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

    constexpr std::array<std::pair<std::size_t, std::size_t>, 4U> unrelocatedLoadConfigurationControls = {
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, Size), sizeof(DWORD)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionCount), sizeof(ULONGLONG)},
        std::pair{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags), sizeof(DWORD)},
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
    require(!cue::validate_windows_shipping_product_security(functionTableOverrun.generic_string(), localProfile,
                                                             a_assertContext));

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
    require(a_argumentCount == 5);
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_product_security(std::filesystem::path(a_arguments[1]), std::filesystem::path(a_arguments[2]),
                          std::filesystem::path(a_arguments[3]), std::filesystem::path(a_arguments[4]), assertContext);
    return 0;
}
