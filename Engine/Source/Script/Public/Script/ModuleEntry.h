#pragma once

#include <cstdint>
#include <vector>

#include <Script/Module.h>

/// @brief 自動生成される Module 入口から参照する一つの型登録を定義する。各 Script の Global Scope で使う
/// a_name は Project 内で一意な単一識別子。登録式は Module 生成時に評価し、C++ 所有値を DLL 外へ出さない
#define CUE_DEFINE_SCRIPT(a_name, ...)                                                                                 \
    ::cue::ScriptRegistration CueScriptRegister_##a_name()                                                             \
    {                                                                                                                  \
        return __VA_ARGS__;                                                                                            \
    }

/// @brief 型登録の列挙から共通の C ABI 入口を生成する。Global Scope の一つの cpp で一度だけ使う
/// 引数は ScriptRegistration の式。最初の呼出中に評価し、成功した Module を Unload まで所有する
/// Session の Owner Thread で呼び、Unload 前に全 Instance を破棄する。登録の例外は Status へ変換する
/// 登録の Result 失敗は保持する。登録式が例外を投げた場合は次の呼出で初期化を再試行する
#define CUE_DEFINE_SCRIPT_MODULE(...)                                                                                  \
    extern "C" CUE_SCRIPT_EXPORT CueScriptStatus CUE_SCRIPT_CALL CueScriptGetModule(                                   \
        ::std::uint32_t a_version, CueScriptModuleApi *a_api) noexcept                                                 \
    {                                                                                                                  \
        try                                                                                                            \
        {                                                                                                              \
            if (!a_api)                                                                                                \
                return ::cue::script_status({::cue::ErrorCategory::InvalidArgument, "ScriptModule.api_null"});         \
            static auto module = ::cue::ScriptModule::create(::std::vector<::cue::ScriptRegistration>{__VA_ARGS__});   \
            if (!module.has_value())                                                                                   \
                return ::cue::script_status(*module.try_error());                                                      \
            return (*module.try_value())->get_api(a_version, *a_api);                                                  \
        }                                                                                                              \
        catch (...)                                                                                                    \
        {                                                                                                              \
            return ::cue::script_exception_status();                                                                   \
        }                                                                                                              \
    }
