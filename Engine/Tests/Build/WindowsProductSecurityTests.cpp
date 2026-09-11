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
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_NOSIGNATURE) ==
            cue::WindowsProductSignatureStatus::Unsigned);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_PROVIDER_UNKNOWN) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
    require(cue::detail::classify_windows_product_trust_status(TRUST_E_SUBJECT_FORM_UNKNOWN) ==
            cue::WindowsProductSignatureStatus::VerificationUnavailable);
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

    bytes = read_bytes(a_validProduct);
    cross_header_boundary_for_load_configuration(bytes);
    const std::filesystem::path headerBoundary = directory / "HeaderBoundary.exe";
    write_bytes(headerBoundary, bytes);
    require(!cue::validate_windows_shipping_product_security(headerBoundary.generic_string(), localProfile,
                                                             a_assertContext));

    constexpr std::array<std::size_t, 3U> loadPointerOffsets = {
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer),
        offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer)};
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
