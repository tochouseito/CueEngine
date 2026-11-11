#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <Foundation/Result.h>
#include <Script/Abi.h>
#include <Script/Components.h>

namespace cue
{
class Behaviour;
class ScriptModule;
class ScriptTransform;

/// @brief World の更新時間を表す SDK 値。start / stop の時間はゼロ
struct ScriptUpdateContext final
{
    std::chrono::nanoseconds deltaTime{};
    std::uint64_t tick = 0;
};

/// @brief Module 境界の Status を SDK 内の所有 Error に変換する
[[nodiscard]] Result<void> script_result(CueScriptStatus a_status);
/// @brief Module 境界へ渡す固定長 Status を作る。例外処理中も確保しない
[[nodiscard]] CueScriptStatus script_status(const Error &a_error) noexcept;
/// @brief ABI 境界の未処理例外を確保なしの Fatal に変換する
[[nodiscard]] CueScriptStatus script_exception_status() noexcept;

/// @brief Entity の非所有窓口。Callback 中だけ EngineApi を借用し、Component の生 Pointer を渡さない
/// 同じ World の Owner Thread でだけ利用する。Callback 後に保持して再利用してはならない
class GameObject final
{
  public:
    /// @brief SDK の Module Adapter が検証済み API と Handle を束ねる
    GameObject(const CueScriptEngineApi &a_api, CueScriptEntity a_entity) noexcept;
    /// @brief 保存用 Identity と異なる、Process 内の世代付き参照値を返す
    [[nodiscard]] CueScriptEntity handle() const noexcept;
    /// @brief World に照会して Handle の失効を調べる
    [[nodiscard]] Result<bool> is_alive() const;
    /// @brief ECS が所有する Transform の値を複写する
    [[nodiscard]] Result<math::floatTransform> transform() const;
    /// @brief ECS Component の値を複写する。SDK が公開する型を World の Owner Thread で取得する
    template <typename T>
        requires(std::same_as<T, math::floatTransform> || std::same_as<T, ScriptCamera> ||
                 std::same_as<T, ScriptMeshRenderer>)
    [[nodiscard]] Result<T> get_component() const
    {
        if constexpr (std::same_as<T, math::floatTransform>)
        {
            return transform();
        }
        else if constexpr (std::same_as<T, ScriptCamera>)
        {
            return camera();
        }
        else
        {
            return mesh_renderer();
        }
    }
    /// @brief Transform の有限性を検証して World の正本へ反映する
    [[nodiscard]] Result<void> set_transform(const math::floatTransform &a_transform) const;
    /// @brief 同じ Scene Instance の Object を名前で検索する。欠損・複数一致は失敗とする
    [[nodiscard]] Result<GameObject> find_by_name(std::string_view a_name) const;
    /// @brief 指定 SceneInstance 内で名前を検索する。欠損・複数一致は失敗とする
    [[nodiscard]] Result<GameObject> find_by_name(CueScriptSceneInstance a_instance, std::string_view a_name) const;
    /// @brief Host の診断窓口へ通知する。ファイル出力先を Script 側で決めない
    [[nodiscard]] Result<void> report(std::uint32_t a_severity, std::string_view a_text) const;

    /// @brief 起動済みの T を同じ Game Module 内で借用して同期実行する
    /// World の Owner Thread 専用。Behaviour と対象 GameObject は Callback 中のみ有効
    /// 同じ型が複数ある場合は a_instanceId で割当を指定する。Callback の所有値と失敗は呼出側へ返る
    template <std::derived_from<Behaviour> T, typename Callback>
    [[nodiscard]] auto with_script(Callback &&a_callback, std::string_view a_instanceId = {}) const
    {
        if constexpr (std::invocable<Callback, T &, GameObject &>)
        {
            auto object = *this;
            return invoke_script<T>(std::forward<Callback>(a_callback), a_instanceId, object);
        }
        else
        {
            return invoke_script<T>(std::forward<Callback>(a_callback), a_instanceId);
        }
    }

  private:
    friend class Behaviour;
    friend class ScriptModule;
    friend class ScriptTransform;
    static constexpr std::size_t k_maxCallDepth = 32;

    /// @brief Game Module 内の現在の呼出を保持し、入れ子から戻ると前の呼出を復元する
    class CallbackScope final
    {
      public:
        /// @brief 対象 Script に所属 Entity と時間を呼出中だけ公開する
        CallbackScope(Behaviour &a_script, const GameObject &a_object, ScriptUpdateContext a_update) noexcept;
        /// @brief 例外を含む全経路で外側の呼出を復元する
        ~CallbackScope();
        /// @brief この呼出の最初の暗黙書き込み失敗を保持する
        void remember(Result<void> a_result);
        /// @brief 明示的な失敗を優先し、成功時には記録された失敗も確認する
        template <typename T> [[nodiscard]] Result<T> finish(Result<T> a_result)
        {
            if (a_result.has_value() && m_error)
            {
                return Result<T>::failure(std::move(*m_error));
            }
            return a_result;
        }

        const GameObject &object;
        const ScriptUpdateContext update;

      private:
        Behaviour &m_script;
        CallbackScope *m_previousScript;
        CallbackScope *m_previousModule;
        std::optional<Error> m_error;
    };

    // この Table は SDK 内だけで借用する。型情報や Behaviour を C ABI へ渡さない
    struct ScriptAccess final
    {
        void *context;
        std::size_t *callDepth;
        Result<Behaviour *> (*find)(void *, const GameObject &, const std::type_info &, std::string_view);
        CallbackScope *callback = nullptr;
    };

    class CallScope final
    {
      public:
        /// @brief 同期呼出の再帰を Game Module 内の深さとして記録する
        explicit CallScope(std::size_t &a_depth) noexcept : m_depth(a_depth) { ++m_depth; }
        /// @brief 例外経路でも元の深さに戻す
        ~CallScope() { --m_depth; }

      private:
        std::size_t &m_depth;
    };

    /// @brief SDK の Module Adapter だけが C++ Script の参照解決を追加する
    GameObject(const CueScriptEngineApi &a_api, CueScriptEntity a_entity, ScriptAccess *a_access) noexcept;
    /// @brief World が所有する値へ一度の ABI 呼出で変更を適用する
    [[nodiscard]] Result<void> change_transform(const CueScriptTransformChange &a_change) const;
    /// @brief Component の投影設定を値へ変換する
    [[nodiscard]] Result<ScriptCamera> camera() const;
    /// @brief Component の Asset ID を値へ変換する
    [[nodiscard]] Result<ScriptMeshRenderer> mesh_renderer() const;
    /// @brief 再帰の上限を検証し、型と割当を Module 内で解決する
    [[nodiscard]] Result<Behaviour *> find_script(const std::type_info &a_type, std::string_view a_instanceId) const;
    template <typename T> struct CallbackResult
    {
        using resultType = Result<T>;
        using valueType = T;
    };

    template <typename T> struct CallbackResult<Result<T>> : CallbackResult<T>
    {
    };

    /// @brief Module 内で解決した実体を借用し、Callback の所有値・失敗を Result にまとめる
    template <typename T, typename Callback, typename... Args>
    [[nodiscard]] auto invoke_script(Callback &&a_callback, std::string_view a_instanceId, Args &...a_args) const
    {
        using Return = std::invoke_result_t<Callback, T &, Args &...>;
        using Traits = CallbackResult<Return>;
        using Output = typename Traits::resultType;
        static_assert(!std::is_reference_v<Return> && !std::is_pointer_v<typename Traits::valueType> &&
                      !std::same_as<typename Traits::valueType, GameObject>);
        auto target = find_script(typeid(T), a_instanceId);
        if (!target.has_value())
        {
            return Output::failure(*target.try_error());
        }
        auto *script = dynamic_cast<T *>(*target.try_value());
        if (!script)
        {
            return Output::failure({ErrorCategory::InvalidState, "GameObject.with_script.type"});
        }
        CallScope scope(*m_access->callDepth);
        CallbackScope callback(*script, *this, m_access->callback->update);
        if constexpr (std::same_as<Return, void>)
        {
            std::invoke(std::forward<Callback>(a_callback), *script, a_args...);
            return callback.finish(Output::success());
        }
        else if constexpr (std::same_as<Return, Output>)
        {
            return callback.finish(std::invoke(std::forward<Callback>(a_callback), *script, a_args...));
        }
        else
        {
            return callback.finish(Output::success(std::invoke(std::forward<Callback>(a_callback), *script, a_args...)));
        }
    }

    const CueScriptEngineApi &m_api;
    CueScriptEntity m_entity;
    ScriptAccess *m_access;
};
} // namespace cue
