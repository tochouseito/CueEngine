#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Assets/BuiltinMesh.h>
#include <Foundation/Path.h>
#include <Math/Transform.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
class IFileSystem;

/// @brief 保存用の透視 Camera 設定。Runtime Component や表示先の Aspect を保存しない
struct SceneCameraDescriptor final
{
    float verticalFovRadians = math::degrees_to_radians(60.0f);
    float nearClip = 0.1f;
    float farClip = 1000.0f;
};

/// @brief 保存用 Mesh の永続参照。MeshHandle や File Path を Identity としない
struct SceneMeshDescriptor final
{
    std::string assetId{k_cubeMeshId};
};

/// @brief Scene 内で永続 ID を持つ保存用 Object。Runtime Handle や GPU 所有値を含めない
struct SceneObjectDescriptor final
{
    std::string objectId;
    std::string name;
    bool isActive = true;
    std::optional<math::floatTransform> transform;
    std::optional<SceneCameraDescriptor> camera;
    std::optional<SceneMeshDescriptor> mesh;
    std::vector<ScriptDescriptor> scripts;
};

/// @brief .cuescene v1 / v2 の所有データ。Script を追加した v1 は create で v2 へ昇格する
struct SceneDescriptor final
{
    std::uint32_t formatVersion = 1;
    std::string sceneId;
    std::string name;
    std::vector<SceneObjectDescriptor> objects;
};

/// @brief 検証済みで不変の Scene 定義を所有する。World / Editor / File Handle は保持しない
/// 借用は破棄・Move・代入まで有効。公開後の読み取り・Serialize は複数 Thread から利用できる
class Scene final
{
  public:
    static constexpr std::size_t k_maxFileBytes = 4 * 1024 * 1024;
    static constexpr std::size_t k_maxObjects = 4096;

    /// @brief 全 ID・名前・既知 Component を検証して所有値を受け取る。失敗では部分 Scene を公開しない
    [[nodiscard]] static Result<Scene> create(SceneDescriptor a_descriptor);
    /// @brief UTF-8 の .cuescene v1 / v2 を解釈し、入力から独立した所有値を返す
    [[nodiscard]] static Result<Scene> parse(std::string_view a_text);
    /// @brief .cuescene を抽象 FileSystem から上限付きで読む。CWD を変えず、FileSystem は呼出中だけ借用する
    [[nodiscard]] static Result<Scene> load(IFileSystem &a_files, const Path &a_file);
    /// @brief round-trip 可能な UTF-8 を所有文字列で返す。外部出力はせず、保存側が replace_file を利用する
    [[nodiscard]] Result<std::string> serialize() const;
    /// @brief Scene が所有する永続設定を読み取り専用で借用する
    [[nodiscard]] const SceneDescriptor &descriptor() const noexcept;

  private:
    /// @brief 検証後の所有データだけを受け取る
    explicit Scene(SceneDescriptor a_descriptor);
    SceneDescriptor m_descriptor;
};
} // namespace cue
