#include <ECS/ComponentIDHelper.h>

// === C++ includes ===
#include <mutex>
#include <string>
#include <unordered_map>

namespace cue::ecs
{
extern "C" std::size_t ECS_RegisterComponentID(const char *a_uniqueName) // C で export すると楽
{
    static std::unordered_map<std::string, std::size_t> map;
    static std::size_t nextID = 0;
    static std::mutex m;
    std::lock_guard lk(m);
    auto [it, inserted] = map.try_emplace(a_uniqueName, nextID);
    if (inserted)
    {
        ++nextID; // 新規登録なら採番
    }
    return it->second; // 既存なら同じ ID を返す
}
} // namespace cue::ecs
