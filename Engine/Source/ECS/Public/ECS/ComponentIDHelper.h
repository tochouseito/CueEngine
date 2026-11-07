// ComponentIDHelper の役割と公開要素を定義する

#pragma once

// === C++ includes ===
#include <cstddef>
#include <typeinfo>

namespace cue::ecs
{
/// @brief 非空の型識別名を Process 内で採番する。採番は Thread-safe、戻り値は永続 ID ではない
extern "C" std::size_t ECS_RegisterComponentID(const char *a_uniqueName);

/// @brief 同じ実行 Module が共有する Registry から型の一時 ID を取得する
template <class T> [[nodiscard]] inline std::size_t ComponentID()
{
    // ※ RTTI がオフなら #T の文字列などにする
    static std::size_t id = ECS_RegisterComponentID(typeid(T).name());
    return id;
}
} // namespace cue::ecs
