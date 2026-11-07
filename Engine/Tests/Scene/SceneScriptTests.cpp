#include <Scene/Scene.h>

#include <cstdio>
#include <limits>

#include "SceneFixtures.h"

#define CUE_SCRIPT_SCENE_CHECK(...)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(__VA_ARGS__))                                                                                            \
        {                                                                                                              \
            std::fprintf(stderr, "Script Scene test line %d: %s\n", __LINE__, #__VA_ARGS__);                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
    } while (false)

/// @brief 未解決 Script の保存、v1 互換、v2 の厳密な構文・数値検証を確認する
int main()
{
    using namespace cue;
    auto original = Scene::parse(tests::k_sceneText);
    CUE_SCRIPT_SCENE_CHECK(original.has_value() && original.try_value()->descriptor().formatVersion == 1);
    auto descriptor = original.try_value()->descriptor();
    ScriptDescriptor script;
    script.instanceId = "3ce1feb2-e711-4ca3-9698-2b32e0d9637c";
    script.typeId = "missing.RotateCube";
    script.schemaVersion = 7;
    script.isEnabled = false;
    script.properties.push_back({"speed", ScriptPropertyKind::Number, 1.2345678901234567, false});
    script.properties.push_back({"clockwise", ScriptPropertyKind::Boolean, 0, true});
    descriptor.objects.front().scripts.push_back(script);
    script.instanceId = "684f8846-e5ba-45d9-90be-7cbf6f3810be";
    script.properties.front().number = std::numeric_limits<double>::min();
    descriptor.objects.front().scripts.push_back(script);
    auto created = Scene::create(descriptor);
    CUE_SCRIPT_SCENE_CHECK(created.has_value() && created.try_value()->descriptor().formatVersion == 2);
    auto text = created.try_value()->serialize();
    CUE_SCRIPT_SCENE_CHECK(text.has_value() && text.try_value()->starts_with("formatVersion=2\n"));
    auto parsed = Scene::parse(*text.try_value());
    CUE_SCRIPT_SCENE_CHECK(parsed.has_value());
    const auto &scripts = parsed.try_value()->descriptor().objects.front().scripts;
    CUE_SCRIPT_SCENE_CHECK(scripts.size() == 2 && scripts.front().typeId == "missing.RotateCube" &&
                           scripts.front().schemaVersion == 7 && !scripts.front().isEnabled);
    CUE_SCRIPT_SCENE_CHECK(scripts.front().properties.size() == 2 &&
                           scripts.front().properties.front().number == 1.2345678901234567 &&
                           scripts.front().properties.back().boolean &&
                           scripts.back().properties.front().number == std::numeric_limits<double>::min());
    auto again = parsed.try_value()->serialize();
    CUE_SCRIPT_SCENE_CHECK(again.has_value() && *again.try_value() == *text.try_value());

    // v1 は既存の内容を読み続けるが、Script のある新しい形式を誤って受け入れない
    auto oldVersion = *text.try_value();
    oldVersion.replace(0, std::string("formatVersion=2").size(), "formatVersion=1");
    CUE_SCRIPT_SCENE_CHECK(!Scene::parse(oldVersion).has_value());
    const std::string prefix = "script.3ce1feb2-e711-4ca3-9698-2b32e0d9637c.";
    for (const auto &suffix :
         {"typeId=missing.RotateCube\n", "enabled=true\n", "schemaVersion=7\n", "property.speed=number:1\n",
          "property.empty=number:nan\n", "property.empty=number:1e999\n", "property.empty=number:1x\n",
          "property.empty=boolean:yes\n", "unknown=value\n"})
    {
        CUE_SCRIPT_SCENE_CHECK(!Scene::parse(*text.try_value() + prefix + suffix).has_value());
    }
    auto requiredMissing = *text.try_value();
    const std::string enabled = prefix + "enabled=false\n";
    requiredMissing.erase(requiredMissing.find(enabled), enabled.size());
    CUE_SCRIPT_SCENE_CHECK(!Scene::parse(requiredMissing).has_value());

    descriptor.objects.front().scripts.push_back(descriptor.objects.front().scripts.front());
    CUE_SCRIPT_SCENE_CHECK(!Scene::create(descriptor).has_value());
    descriptor.objects.front().scripts.pop_back();
    descriptor.objects.front().scripts.front().properties.front().number = std::numeric_limits<double>::infinity();
    CUE_SCRIPT_SCENE_CHECK(!Scene::create(descriptor).has_value());
    return 0;
}
