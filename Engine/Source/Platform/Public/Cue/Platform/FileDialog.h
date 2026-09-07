#pragma once

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class FileDialogOwnerAccess;

/// @brief Native File Dialogで要求する選択種別
enum class FileDialogKind : std::uint8_t
{
    OpenFile,
    SaveFile,
    SelectFolder,
};

/// @brief Native File Dialogへ渡す一つの表示Filter
struct FileDialogFilter final
{
    std::string displayName;
    std::string pattern;
};

/// @brief Platform Hostが発行するNative Dialog Ownerの非所有Capability
///
/// TokenはNative Windowの寿命を延長しない。発行元Windowより長く保持せず、そのWindowのOwner Thread上でのみ使用する。
/// Native HandleとWindows型は公開せず、Platform Adapterだけが内容を検証できる。
class FileDialogOwnerToken final
{
  public:
    /// @brief Owner Capabilityの複製を禁止する
    FileDialogOwnerToken(const FileDialogOwnerToken &) = delete;
    /// @brief Owner CapabilityのCopy代入を禁止する
    FileDialogOwnerToken &operator=(const FileDialogOwnerToken &) = delete;
    /// @brief Owner Capabilityを移動し、移動元を無効化する
    FileDialogOwnerToken(FileDialogOwnerToken &&a_other) noexcept;
    /// @brief 現在のCapabilityを破棄してOwner Capabilityを移動する
    FileDialogOwnerToken &operator=(FileDialogOwnerToken &&a_other) noexcept;
    /// @brief 非所有Capabilityを解放する
    ~FileDialogOwnerToken() = default;

  private:
    friend class FileDialogOwnerAccess;

    /// @brief Platform Adapterが検証済みの不透明Owner値、Generation、Owner Threadを保持する
    FileDialogOwnerToken(std::uintptr_t a_nativeValue, std::uint64_t a_ownerGeneration,
                         std::uint32_t a_ownerThreadId) noexcept;

    std::uintptr_t m_nativeValue = 0U;
    std::uint64_t m_ownerGeneration = 0U;
    std::uint32_t m_ownerThreadId = 0U;
};

/// @brief UI非依存のNative File Dialog要求
///
/// initialLocationHintは表示Hintであり、選択結果の信頼境界には使用しない。
class FileDialogRequest final
{
  public:
    /// @brief Dialog種別、表示設定、短命Owner Capabilityを所有する要求を構築する
    FileDialogRequest(FileDialogKind a_kind, std::vector<FileDialogFilter> a_filters, std::string a_defaultExtension,
                      std::string a_initialLocationHint, FileDialogOwnerToken a_owner) noexcept;
    /// @brief Owner Capabilityを一意に保つためCopy構築を禁止する
    FileDialogRequest(const FileDialogRequest &) = delete;
    /// @brief Owner Capabilityを一意に保つためCopy代入を禁止する
    FileDialogRequest &operator=(const FileDialogRequest &) = delete;
    /// @brief Dialog要求の所有権を移動する
    FileDialogRequest(FileDialogRequest &&) noexcept = default;
    /// @brief 現在の要求を解放してDialog要求を移動する
    FileDialogRequest &operator=(FileDialogRequest &&) noexcept = default;
    /// @brief 所有する文字列、Filter、Owner Capabilityを解放する
    ~FileDialogRequest() = default;

    /// @brief 要求したDialog種別を返す
    [[nodiscard]] FileDialogKind kind() const noexcept;
    /// @brief 表示Filterを要求順で返す
    [[nodiscard]] const std::vector<FileDialogFilter> &filters() const noexcept;
    /// @brief Save File用Default Extensionを返す
    [[nodiscard]] std::string_view default_extension() const noexcept;
    /// @brief 初期Location Hintを返す
    [[nodiscard]] std::string_view initial_location_hint() const noexcept;
    /// @brief Dialog表示中だけ使用するOwner Capabilityを返す
    [[nodiscard]] const FileDialogOwnerToken &owner() const noexcept;

  private:
    FileDialogKind m_kind;
    std::vector<FileDialogFilter> m_filters;
    std::string m_defaultExtension;
    std::string m_initialLocationHint;
    FileDialogOwnerToken m_owner;
};

/// @brief Native File Dialogの非失敗Outcome
enum class FileDialogOutcome : std::uint8_t
{
    Selected,
    Cancelled,
};

/// @brief 選択または利用者Cancelを所有値で表すNative File Dialog結果
///
/// Selected Pathは正規化済みUTF-8 Absolute Pathだが未検証Locatorである。Project用途では保存や操作前に
/// ProjectFileService、Project Hub用途ではProjectHubPlatformとCue.Projectで必ず再検証する。
class FileDialogResult final
{
  public:
    /// @brief 未検証UTF-8 Absolute Pathを持つSelected結果を構築する
    [[nodiscard]] static FileDialogResult selected(std::string a_unverifiedPath) noexcept;
    /// @brief Errorではない利用者Cancel結果を構築する
    [[nodiscard]] static FileDialogResult cancelled() noexcept;

    /// @brief Dialogの非失敗Outcomeを返す
    [[nodiscard]] FileDialogOutcome outcome() const noexcept;
    /// @brief Selected時だけ未検証UTF-8 Absolute Pathを返す
    [[nodiscard]] std::optional<std::string_view> selected_path() const noexcept;

  private:
    /// @brief Outcomeと任意の未検証Pathを整合した状態で所有する
    FileDialogResult(FileDialogOutcome a_outcome, std::optional<std::string> a_unverifiedPath) noexcept;

    FileDialogOutcome m_outcome;
    std::optional<std::string> m_unverifiedPath;
};

/// @brief Project HubとEditorで共有するNative File Dialog境界
///
/// ResultのFailureはNative、Owner、UTF、Path構成の失敗を表し、Cancelledへ丸めない。
class FileDialogService
{
  public:
    /// @brief Serviceの一意所有を保つためCopy構築を禁止する
    FileDialogService(const FileDialogService &) = delete;
    /// @brief ServiceのCopy代入を禁止する
    FileDialogService &operator=(const FileDialogService &) = delete;
    /// @brief Platform実装が所有するResourceを解放する
    virtual ~FileDialogService() = default;

    /// @brief Owner Thread上でNative Dialogを同期表示する
    [[nodiscard]] virtual Result<FileDialogResult> show(const FileDialogRequest &a_request) noexcept = 0;

  protected:
    /// @brief Platform実装だけがServiceを構築できる状態にする
    FileDialogService() noexcept = default;
};
} // namespace cue
