#include <Scene/Scene.h>

#include <cstdio>
#include <limits>

#include <Foundation/Uuid.h>

#include "SceneFixtures.h"

namespace
{
/// @brief 値の独立性、Serialize の完全復元、各 Component の任意性を確認する
int test_values()
{
    std::string text(cue::tests::k_sceneText);
    auto parsed = cue::Scene::parse(text);
    if (!parsed.has_value())
        return __LINE__;
    text.clear();
    const auto &descriptor = parsed.try_value()->descriptor();
    if (descriptor.objects.size() != 1 || descriptor.objects[0].objectId != cue::tests::k_cubeId ||
        descriptor.objects[0].transform->position != cue::math::float3{1, 2, 3})
        return __LINE__;
    auto serialized = parsed.try_value()->serialize();
    if (!serialized.has_value())
        return __LINE__;
    auto roundTrip = cue::Scene::parse(*serialized.try_value());
    if (!roundTrip.has_value() ||
        roundTrip.try_value()->descriptor().objects[0].transform->scale != cue::math::float3{2, 3, 4})
        return __LINE__;
    auto values = descriptor;
    values.objects[0].name = "回転 Cube=1 #表示";
    values.objects[0].transform->position = {1.2345678f, (std::numeric_limits<float>::min)(), -123456.75f};
    values.objects[0].isActive = false;
    auto owned = cue::Scene::create(values);
    if (!owned.has_value())
        return __LINE__;
    auto encoded = owned.try_value()->serialize();
    if (!encoded.has_value())
        return __LINE__;
    auto decoded = cue::Scene::parse(*encoded.try_value());
    if (!decoded.has_value() ||
        decoded.try_value()->descriptor().objects[0].transform->position != values.objects[0].transform->position ||
        decoded.try_value()->descriptor().objects[0].name != values.objects[0].name ||
        decoded.try_value()->descriptor().objects[0].isActive)
        return __LINE__;
    values.objects[0].transform.reset();
    auto plain = cue::Scene::create(values);
    if (!plain.has_value())
        return __LINE__;
    auto plainText = plain.try_value()->serialize();
    if (!plainText.has_value() || plainText.try_value()->find("transform.") != std::string::npos)
        return __LINE__;
    values.objects.clear();
    return cue::Scene::create(std::move(values)).has_value() ? 0 : __LINE__;
}

/// @brief 未対応形式・未知 Component・重複・非有限値を部分データとして公開しない
int test_invalid_data()
{
    using cue::tests::scene_replaced;
    for (const auto &text :
         {std::string{}, scene_replaced("formatVersion=1", "formatVersion=3"),
          scene_replaced("sceneId=8d94c2b1-9b6a-435c-ade7-c01f473c3951", "sceneId=bad"),
          scene_replaced("name=Cube", "name="), scene_replaced("active=true", "active=yes"),
          scene_replaced("transform.position=1,2,3", "transform.position=1,2"),
          scene_replaced("transform.position=1,2,3", "transform.position=1,2,3,4"),
          scene_replaced("transform.position=1,2,3", "transform.position=nan,2,3"),
          scene_replaced("transform.position=1,2,3", "transform.position=1e99,2,3"),
          scene_replaced("transform.position=1,2,3", "transform.position=,2,3"),
          scene_replaced("transform.position=1,2,3", "transform.position=1x,2,3"),
          scene_replaced("transform.rotation=0,0,0,1", "transform.rotation=0,0,0,0"),
          scene_replaced("transform.scale=2,3,4", ""), scene_replaced("transform.scale=2,3,4", "mesh.asset=Unknown"),
          scene_replaced("[object ", "[unknown "), std::string(cue::tests::k_sceneText) + "name=Duplicate\n",
          std::string(cue::tests::k_sceneText) + "\n[object 9a00d731-7759-424c-9ade-fd086d624551]\nname=Duplicate\n",
          std::string(cue::tests::k_sceneText) + "#\xff\n",
          std::string(cue::tests::k_sceneText) + std::string("#bad\0comment", 12)})
    {
        if (cue::Scene::parse(text).has_value())
            return __LINE__;
    }
    auto valid = cue::Scene::parse(cue::tests::k_sceneText);
    if (!valid.has_value())
        return __LINE__;
    auto duplicate = valid.try_value()->descriptor();
    duplicate.objects.push_back(duplicate.objects.front());
    if (cue::Scene::create(std::move(duplicate)).has_value())
        return __LINE__;
    auto tooMany = valid.try_value()->descriptor();
    tooMany.objects.resize(cue::Scene::k_maxObjects + 1);
    if (cue::Scene::create(std::move(tooMany)).has_value())
        return __LINE__;
    return 0;
}

/// @brief BOM / CRLF・Comment と Byte 上限・UUID の正規表記を確認する
int test_text_boundaries()
{
    std::string crlf = "\xef\xbb\xbf# Comment\r\n";
    for (const char value : cue::tests::k_sceneText)
        crlf += value == '\n' ? "\r\n" : std::string(1, value);
    if (!cue::Scene::parse(crlf).has_value())
        return __LINE__;
    std::string limit(cue::tests::k_sceneText);
    limit += '#';
    limit.resize(cue::Scene::k_maxFileBytes, 'x');
    if (!cue::Scene::parse(limit).has_value())
        return __LINE__;
    limit += 'x';
    if (cue::Scene::parse(limit).has_value() || cue::is_valid_uuid_text("00000000-0000-0000-0000-000000000000") ||
        cue::is_valid_uuid_text("9A00d731-7759-424c-9ade-fd086d624551"))
        return __LINE__;
    return 0;
}
} // namespace

/// @brief Scene の形式・値・Serialize を Platform Backend なしで検証する
int main()
{
    for (const auto test : {test_values, test_invalid_data, test_text_boundaries})
    {
        const auto result = test();
        if (result)
        {
            std::fprintf(stderr, "Scene test failed at line %d\n", result);
            return 1;
        }
    }
    return 0;
}
