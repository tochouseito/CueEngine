#include <Scene/Scene.h>

#include <span>

#include "../Support/FileSystemProbe.h"
#include "../Support/TemporaryFiles.h"
#include "SceneFixtures.h"

/// @brief 実 File の Unicode Path・所有値・部分 I/O 失敗・Serialize 保存を確認する
int main()
{
    cue::tests::TemporaryFiles fixture;
    if (!fixture.isCreated)
        return 1;
    auto path = fixture.root.join("Scene/回転 Cube.cuescene");
    if (!path.has_value())
        return 2;
    if (!fixture.files
             ->write_all(*path.try_value(),
                         std::as_bytes(std::span(cue::tests::k_sceneText.data(), cue::tests::k_sceneText.size())), true)
             .has_value())
        return 3;
    cue::tests::FileSystemProbe probe(*fixture.files);
    auto scene = cue::Scene::load(probe, *path.try_value());
    if (!scene.has_value() || probe.absoluteCount != 1)
        return 4;
    probe.failRead = true;
    auto failed = cue::Scene::load(probe, *path.try_value());
    if (failed.has_value() || failed.try_error()->operation != "Probe.open")
        return 5;
    auto missing = fixture.root.join("Missing.cuescene");
    auto wrong = fixture.root.join("Wrong.scene");
    if (!missing.has_value() || !wrong.has_value() ||
        cue::Scene::load(*fixture.files, *missing.try_value()).has_value() ||
        cue::Scene::load(*fixture.files, *wrong.try_value()).has_value())
        return 6;
    auto encoded = scene.try_value()->serialize();
    if (!encoded.has_value())
        return 7;
    auto saved = fixture.files->replace_file(
        *path.try_value(), std::as_bytes(std::span(encoded.try_value()->data(), encoded.try_value()->size())));
    if (!saved.has_value())
        return 8;
    auto restored = cue::Scene::load(*fixture.files, *path.try_value());
    if (!restored.has_value() || restored.try_value()->descriptor().objects[0].objectId != cue::tests::k_cubeId)
        return 9;
    return 0;
}
