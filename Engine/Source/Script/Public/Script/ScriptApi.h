#pragma once

#include <string_view>
#include <type_traits>
#include <utility>

namespace cue
{
/// @brief 公開 API の宣言がない既存 Script に空の転送基底を提供する
/// Instance を保持せず、ScriptReference からだけ構築する
template <typename T, typename Reference, typename = void> class ScriptApiMethods
{
  protected:
    /// @brief ScriptReference の基底としてだけ生成する
    ScriptApiMethods() = default;
    /// @brief ScriptReference と同時に破棄する
    ~ScriptApiMethods() = default;
};

/// @brief Script が宣言した公開関数を ScriptReference の Member として公開する
template <typename T, typename Reference>
class ScriptApiMethods<T, Reference, std::void_t<typename T::template _cueScriptApi<Reference>>>
    : public T::template _cueScriptApi<Reference>
{
  protected:
    /// @brief ScriptReference の基底としてだけ生成する
    ScriptApiMethods() = default;
    /// @brief ScriptReference と同時に破棄する
    ~ScriptApiMethods() = default;
};
} // namespace cue

/// @brief Behaviour 派生 Class の public に一度だけ書き、CUE_SCRIPT_METHOD で公開関数を列挙する
/// 転送窓口は対象を所有せず、Owner Thread の現在の Script Callback 内だけ利用する
/// 生成する内部型名は Editor が Script 名に許可しない先頭の下線を使う
#define CUE_SCRIPT_API(a_scriptType, ...)                                                                              \
    template <typename _cueReference> class _cueScriptApi                                                              \
    {                                                                                                                  \
        using scriptType = a_scriptType;                                                                               \
                                                                                                                       \
      protected:                                                                                                       \
        /** @brief ScriptReference の基底としてだけ構築する */                                                         \
        _cueScriptApi() = default;                                                                                     \
        /** @brief 基底だけのコピーによる窓口の持ち出しを防ぐ */                                                       \
        _cueScriptApi(const _cueScriptApi &) = delete;                                                                 \
        /** @brief 基底だけの代入による窓口の持ち出しを防ぐ */                                                         \
        _cueScriptApi &operator=(const _cueScriptApi &) = delete;                                                      \
        /** @brief ScriptReference と同時に破棄する */                                                                 \
        ~_cueScriptApi() = default;                                                                                    \
                                                                                                                       \
      public:                                                                                                          \
        __VA_ARGS__                                                                                                    \
    };

/// @brief CUE_SCRIPT_API 内で公開する関数名を指定し、引数・戻り値を型付きで転送する
/// Overload と既定引数は対象 Class で解決する。call / invoke / try_error は窓口の予約名
#define CUE_SCRIPT_METHOD(a_method)                                                                                    \
    static_assert(::std::string_view{#a_method} != "call" && ::std::string_view{#a_method} != "invoke" &&              \
                      ::std::string_view{#a_method} != "try_error",                                                    \
                  "Script API method name is reserved by ScriptReference");                                            \
    /** @brief 公開関数を対象の Callback Context で実行し、失敗を Result で返す */                                     \
    template <typename... Args>                                                                                        \
        requires requires(scriptType &a_script, Args &&...a_args) {                                                    \
            a_script.a_method(::std::forward<Args>(a_args)...);                                                        \
        }                                                                                                              \
    [[nodiscard]] auto a_method(Args &&...a_args) const                                                                \
    {                                                                                                                  \
        /* 対象の生存と Context は共通窓口に再検証させ、実体の Pointer を持ち出さない */                               \
        return static_cast<const _cueReference &>(*this).invoke(                                                       \
            [&](scriptType &a_script) -> decltype(auto)                                                                \
            { return a_script.a_method(::std::forward<Args>(a_args)...); });                                           \
    }
