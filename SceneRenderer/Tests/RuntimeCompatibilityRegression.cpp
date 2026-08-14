#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

import sr.scene;
import sr.scene_wallpaper;
import sr.fs;
import sr.json;
import sr.pkg.parse;
import sr.pkg_fs;
import sr.scene_uniform_updater;
import sr.spec_texs;
import sr.types;
import sr.vulkan;
import sr.vulkan_render;
import eigen;
import rstd;
import wavsen.audio;

namespace
{

int g_failures = 0;

void Check(bool ok, std::string_view what) {
    if (ok) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << '\n';
}

bool Near(float actual, float expected, float epsilon = 0.001f) {
    return std::abs(actual - expected) <= epsilon;
}

class ProbeImageParser final : public sr::IImageParser {
public:
    bool contains { false };

    bool Contains(const std::string&) const override { return contains; }
    std::shared_ptr<sr::Image> Parse(const std::string&) override { return nullptr; }
    sr::ImageHeader ParseHeader(const std::string&) override { return {}; }
};

sr::Json Parse(std::string_view source) {
    auto value = sr::ParseJson(source);
    if (value.is_ok()) return value.unwrap();
    ++g_failures;
    std::cerr << "FAIL: invalid test JSON\n";
    return sr::Json::Null();
}

class ProbeSound final : public sr::SceneSoundControl {
public:
    void Play() override {
        ++play_count;
        playing = true;
    }
    void Stop() override {
        ++stop_count;
        playing = false;
    }
    void Pause() override { playing = false; }
    bool IsPlaying() const override { return playing; }
    void SetVolume(float value) override { volume = value; }

    int   play_count { 0 };
    int   stop_count { 0 };
    bool  playing { true };
    float volume { 1.0f };
};

void TestExplicitCameraFactories() {
    auto ortho = sr::SceneCamera::MakeOrthographic(1920.5, 1080.25, -1.0, 1.0);
    Check(! ortho.IsPerspective(), "orthographic factory selects orthographic projection");
    Check(ortho.Width() == 1920.5, "orthographic factory preserves fractional width");
    Check(ortho.Height() == 1080.25, "orthographic factory preserves fractional height");

    auto perspective = sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 45.0);
    Check(perspective.IsPerspective(), "perspective factory selects perspective projection");
    Check(perspective.Aspect() == 16.0 / 9.0, "perspective factory preserves aspect");
    Check(perspective.Fov() == 45.0, "perspective factory preserves field of view");
}

void TestPerspectiveFillModePreservesFov() {
    sr::Scene scene;
    scene.SetProjectionKind(sr::SceneProjectionKind::Perspective3D);
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;
    scene.cameras["global"] = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -5000.0, 5000.0));
    auto perspective = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 10000.0, 50.0));
    sr::SceneNode camera_node;
    perspective->AttatchNode(&camera_node);
    scene.cameras["global_perspective"] = perspective;
    scene.activeCamera = perspective.get();

    sr::vulkan::UpdateCameraFillModeForExtent(
        scene, sr::FillMode::ASPECTCROP, 1920, 1080);

    Check(Near(perspective->Fov(), 50.0f),
          "perspective scene fill mode preserves authored field of view");
    Check(Near(perspective->Aspect(), 16.0f / 9.0f),
          "perspective scene fill mode updates output aspect");
}

void TestOrthographicFillModeDerivesPerspectiveFov() {
    sr::Scene scene;
    scene.SetProjectionKind(sr::SceneProjectionKind::OrthographicCanvas);
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;
    scene.cameras["global"] = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -5000.0, 5000.0));
    auto perspective = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakePerspective(16.0 / 9.0, 5.0, 15000.0, 50.0));
    sr::SceneNode camera_node;
    perspective->AttatchNode(&camera_node);
    scene.cameras["global_perspective"] = perspective;

    sr::vulkan::UpdateCameraFillModeForExtent(
        scene, sr::FillMode::ASPECTCROP, 1920, 1080);

    const float expected = static_cast<float>(
        std::atan(1080.0 / 1000.0 / 2.0) * 2.0 * 180.0 / std::numbers::pi);
    Check(Near(perspective->Fov(), expected),
          "orthographic scene fill mode derives embedded perspective field of view");
}

void TestAuthoredSceneZoom() {
    sr::Scene scene;
    scene.ortho[0] = 1920;
    scene.ortho[1] = 1080;
    scene.SetViewportScale(1.5f);
    auto extent = scene.OrthographicProjectionExtent();
    Check(extent[0] == 1280.0 && extent[1] == 720.0,
          "authored zoom scales the orthographic projection extent");
    scene.SetViewportScale(0.0f);
    extent = scene.OrthographicProjectionExtent();
    Check(extent[0] == 1920.0 && extent[1] == 1080.0,
          "invalid authored zoom falls back to unity");
}

sr::SceneAnimationCurve RootZoomCurve(float final_zoom) {
    sr::SceneAnimationCurve curve;
    curve.fps    = 30.0f;
    curve.length = 450;
    curve.mode   = "single";
    curve.c0.push_back({ .frame = 0, .value = 3.0f });
    curve.c0.push_back({ .frame = 300, .value = 3.0f });
    curve.c0.push_back({ .frame = 450, .value = final_zoom });
    return curve;
}

void TestAnimatedSceneZoom() {
    sr::Scene scene;
    scene.SetViewportScale(3.0f);
    scene.SetViewportScaleAnimation(RootZoomCurve(1.0f));
    auto global = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(100.0, 50.0, -1.0, 1.0));
    auto linked = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0));
    scene.cameras["global"] = global;
    scene.cameras["linked"] = linked;
    scene.linkedCameras["global"].push_back("linked");
    scene.CaptureCameraPathViewports();

    scene.elapsingTime = 0.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 100.0f) && Near(global->Height(), 50.0f),
          "root zoom animation preserves the authored initial close-up");

    scene.elapsingTime = 10.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 100.0f) && Near(global->Height(), 50.0f),
          "root zoom animation holds the initial value through frame 300");

    scene.elapsingTime = 15.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 300.0f) && Near(global->Height(), 150.0f),
          "root zoom animation expands the viewport when zoom reaches one");
    Check(Near(linked->Width(), 300.0f) && Near(linked->Height(), 150.0f),
          "root zoom animation propagates to linked cameras");

    global->SetWidth(200.0);
    global->SetHeight(100.0);
    scene.CaptureCameraPathViewports();
    scene.TickCameraPaths();
    Check(Near(global->Width(), 600.0f) && Near(global->Height(), 300.0f),
          "root zoom baseline recapture does not apply the animated ratio twice");
}

void TestAnimatedSceneZoomWithCameraPath() {
    sr::Scene scene;
    scene.SetViewportScale(3.0f);
    scene.SetViewportScaleAnimation(RootZoomCurve(1.0f));
    auto global = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(120.0, 60.0, -1.0, 1.0));
    scene.cameras["global"] = global;

    sr::SceneNode node;
    auto path          = std::make_shared<sr::SceneCameraPath>();
    path->camera_name  = "global";
    path->camera       = global;
    path->node         = &node;
    path->zoom_base    = 2.0f;
    path->zoom_curve   = RootZoomCurve(4.0f);
    scene.camera_paths = { path };
    scene.CaptureCameraPathViewports();

    scene.elapsingTime = 15.0;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 90.0f) && Near(global->Height(), 45.0f),
          "root zoom composes multiplicatively with camera object zoom");

    path->enabled = false;
    scene.TickCameraPaths();
    Check(Near(global->Width(), 360.0f) && Near(global->Height(), 180.0f),
          "disabled camera paths keep the root zoom animation active");
}

void TestInvalidAnimatedSceneZoom() {
    sr::Scene scene;
    scene.SetViewportScale(3.0f);
    scene.SetViewportScaleAnimation(RootZoomCurve(-1.0f));
    auto global = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(100.0, 50.0, -1.0, 1.0));
    scene.cameras["global"] = global;
    scene.CaptureCameraPathViewports();
    scene.elapsingTime = 15.0;
    scene.TickCameraPaths();
    Check(std::isfinite(global->Width()) && std::isfinite(global->Height()) &&
              global->Width() > 0.0 && global->Height() > 0.0,
          "invalid animated root zoom remains finite and positive");
}

void TestPointerUniformsIgnoreParallaxDelay() {
    sr::Scene scene;
    scene.frameTime = 1.0 / 30.0;
    auto camera = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(1920.0, 1080.0, -1.0, 1.0));
    scene.activeCamera = camera.get();

    sr::SceneUniformUpdater updater(&scene);
    updater.SetCameraParallax({ .enable = false, .amount = 0.01f, .delay = 0.9f,
                                .mouseinfluence = 0.5f });

    sr::SceneNode node;
    auto mesh = std::make_shared<sr::SceneMesh>();
    mesh->AddMaterial(sr::SceneMaterial {});
    node.AddMesh(mesh);
    updater.InitUniforms(&node, [](std::string_view name) {
        return name == "g_PointerPosition" || name == "g_PointerPositionLast";
    });

    sr::sprite_map_t sprites;
    std::array<float, 2> current {};
    std::array<float, 2> previous {};
    auto capture = [&](std::string_view name, sr::ShaderValue value) {
        if (name == "g_PointerPosition") current = { value[0], value[1] };
        if (name == "g_PointerPositionLast") previous = { value[0], value[1] };
    };

    updater.MouseInput(0.2, 0.8);
    updater.FrameBegin();
    updater.UpdateUniforms(&node, sprites, capture, sr::SceneRenderViewKind::Primary,
                           sr::SceneRenderAlphaMode::Composite);
    Check(Near(current[0], 0.2f) && Near(current[1], 0.8f) &&
              Near(previous[0], 0.2f) && Near(previous[1], 0.8f),
          "first pointer sample initializes current and previous uniforms");

    updater.MouseInput(0.85, 0.15);
    updater.FrameBegin();
    updater.UpdateUniforms(&node, sprites, capture, sr::SceneRenderViewKind::Primary,
                           sr::SceneRenderAlphaMode::Composite);
    Check(Near(current[0], 0.85f) && Near(current[1], 0.15f),
          "pointer uniform uses the current raw frame sample");
    Check(Near(previous[0], 0.2f) && Near(previous[1], 0.8f),
          "previous pointer uniform uses the preceding raw frame sample");
}

void TestPlanarReflectionSemantics() {
    auto camera = sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0);
    camera.SetLookAt({ 1.0, 2.0, 3.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 });
    const auto primary_eye    = camera.GetPosition();
    const auto reflection_eye = camera.GetPosition(sr::SceneRenderViewKind::Reflection);
    Check(primary_eye.x() == reflection_eye.x() && primary_eye.y() == -reflection_eye.y() &&
              primary_eye.z() == reflection_eye.z(),
          "reflection camera mirrors only the world Y coordinate");
    const auto primary_vp = camera.GetViewProjectionMatrix();
    const auto reflection_vp =
        camera.GetViewProjectionMatrix(sr::SceneRenderViewKind::Reflection);
    Check(primary_vp.allFinite() && reflection_vp.allFinite() &&
              ! primary_vp.isApprox(reflection_vp),
          "reflection view-projection is finite and distinct from the primary view");

    sr::Scene scene;
    scene.ortho[0] = 1280;
    scene.ortho[1] = 720;
    scene.renderTargets["_rt_default"] = { .width = 2560, .height = 1440 };
    scene.EnablePlanarReflection();
    scene.EnablePlanarReflection();
    const auto reflection = scene.renderTargets.find("_rt_Reflection");
    Check(scene.PlanarReflectionEnabled(), "reflection texture enables the planar render stage");
    Check(reflection != scene.renderTargets.end(), "reflection render target is registered");
    if (reflection != scene.renderTargets.end()) {
        Check(reflection->second.width == 2560 && reflection->second.height == 1440,
              "reflection render target follows the primary target extent");
        Check(reflection->second.withDepth && reflection->second.bind.screen &&
                  reflection->second.preserve_on_write,
              "reflection target keeps depth and accumulated reflected layers");
    }

    sr::SceneNode node;
    Check(! node.Reflected(), "generated scene nodes are not reflected by default");
    node.SetReflected(true);
    Check(node.Reflected(), "authored reflection state is retained on scene nodes");

    Check(sr::wpscene::ImageObject {}.reflected && sr::wpscene::TextObject {}.reflected &&
              sr::wpscene::ModelObject {}.reflected && sr::wpscene::ParticleObject {}.reflected,
          "authored renderable layer kinds preserve Wallpaper Engine's reflected default");
}

void TestWrappedAnimationCurves() {
    sr::SceneAnimationCurve tail_wrap;
    tail_wrap.fps      = 1.0f;
    tail_wrap.length   = 4;
    tail_wrap.mode     = "loop";
    tail_wrap.wraploop = true;
    tail_wrap.c0.push_back({ .frame = 0, .value = 0.0f });
    tail_wrap.c0.push_back({ .frame = 2, .value = 10.0f });
    Check(Near(tail_wrap.EvaluateScalar(0.0f, 3.0), 5.0f),
          "wraploop interpolates last key back to first");

    sr::SceneAnimationCurve head_wrap;
    head_wrap.fps      = 1.0f;
    head_wrap.length   = 4;
    head_wrap.mode     = "loop";
    head_wrap.wraploop = true;
    head_wrap.c0.push_back({ .frame = 1, .value = 10.0f });
    head_wrap.c0.push_back({ .frame = 3, .value = 30.0f });
    Check(Near(head_wrap.EvaluateScalar(0.0f, 0.0), 20.0f),
          "wraploop interpolates previous cycle before first key");

    sr::SceneAnimationCurve mirror;
    mirror.fps      = 1.0f;
    mirror.length   = 2;
    mirror.mode     = "mirror";
    mirror.wraploop = true;
    mirror.c0.push_back({ .frame = 0, .value = 0.0f });
    mirror.c0.push_back({ .frame = 2, .value = 10.0f });
    Check(Near(mirror.EvaluateScalar(0.0f, 3.0), 5.0f),
          "mirror mode takes precedence over wraploop");
}

void TestFieldAnimationPlayback() {
    auto playback = std::make_shared<sr::SceneAnimationPlayback>(
        "face", 10.0f, 10, "single", false, true);

    sr::SceneAnimationCurve origin;
    origin.fps      = 10.0f;
    origin.length   = 10;
    origin.mode     = "single";
    origin.relative = true;
    origin.playback = playback;
    origin.c0.push_back({ .frame = 0, .value = 0.0f });
    origin.c0.push_back({ .frame = 10, .value = 10.0f });

    sr::SceneAnimationCurve angles;
    angles.fps      = 10.0f;
    angles.length   = 10;
    angles.mode     = "single";
    angles.playback = playback;
    angles.c2.push_back({ .frame = 0, .value = 0.0f });
    angles.c2.push_back({ .frame = 10, .value = 1.0f });

    sr::SceneNode node;
    node.SetOriginAnimation(std::move(origin));
    node.SetRotationAnimation(std::move(angles));
    Check(node.FindAnimation("face") == playback, "named field animation is registered");

    node.TickFieldAnimations(0.0);
    node.TickFieldAnimations(1.0);
    Check(Near(node.Translate().x(), 0.0f) && Near(node.Rotation().z(), 0.0f),
          "start-paused combined animation remains at frame zero");

    playback->Play();
    node.TickFieldAnimations(1.5);
    Check(Near(playback->Frame(), 5.0f) && Near(node.Translate().x(), 5.0f) &&
              Near(node.Rotation().z(), 0.5f),
          "combined animation tracks share one playback frame");

    playback->Pause();
    node.TickFieldAnimations(2.0);
    Check(Near(playback->Frame(), 5.0f) && Near(node.Translate().x(), 5.0f),
          "paused animation holds its current frame");

    playback->SetFrame(8.0);
    node.TickFieldAnimations(2.0);
    Check(Near(node.Translate().x(), 8.0f) && Near(node.Rotation().z(), 0.8f),
          "setFrame applies to every combined track");

    playback->Stop();
    node.TickFieldAnimations(2.0);
    Check(Near(playback->Frame(), 0.0f) && Near(node.Translate().x(), 0.0f),
          "stop restores the first frame");

    playback->SetRate(2.0);
    playback->Play();
    node.TickFieldAnimations(2.25);
    node.TickFieldAnimations(2.5);
    Check(playback->Status() == sr::SceneAnimationPlaybackStatus::Completed &&
              Near(playback->Frame(), 10.0f) && Near(node.Translate().x(), 10.0f),
          "single animation completes at and holds the last frame");

    playback->Play();
    node.TickFieldAnimations(2.5);
    Check(playback->IsPlaying() && Near(playback->Frame(), 0.0f) &&
              Near(node.Translate().x(), 0.0f),
          "completed single animation replays from frame zero");
}

void TestSoundVisibilityAndVolume() {
    sr::SceneNode node;
    node.SetVolume(0.4f);
    Check(Near(node.Volume(), 0.4f), "sound volume is retained before playback is attached");
    auto          sound = std::make_shared<ProbeSound>();
    node.SetSoundControl(sound);
    Check(Near(sound->volume, 0.4f), "attaching playback applies the retained sound volume");

    node.SetVisible(false);
    Check(sound->stop_count == 1 && ! sound->playing, "hiding sound layer stops playback");
    node.SetVisible(false);
    Check(sound->stop_count == 1, "repeated hide does not restart sound state");
    node.SetVisible(true);
    Check(sound->play_count == 1 && sound->playing, "showing sound layer resumes playback");

    node.SetVolume(2.0f);
    Check(Near(node.Volume(), 1.0f) && Near(sound->volume, 1.0f),
          "sound volume actuator clamps and stores the upper bound");
    node.SetVolume(-1.0f);
    Check(Near(node.Volume(), 0.0f) && Near(sound->volume, 0.0f),
          "sound volume actuator clamps and stores the lower bound");
}

void TestCameraTransformControls() {
    auto camera = sr::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0);
    const sr::SceneCameraTransforms authored {
        .eye = { 2.0, 3.0, 4.0 },
        .center = { -1.0, 0.5, 0.0 },
        .up = { 0.0, 1.0, 0.0 },
    };
    Check(camera.SetTransforms(authored), "camera accepts finite non-degenerate transforms");
    const auto roundtrip = camera.Transforms();
    Check(roundtrip.eye.isApprox(authored.eye) && roundtrip.center.isApprox(authored.center) &&
              roundtrip.up.isApprox(authored.up),
          "camera transform controls preserve eye, center and up");

    auto degenerate = authored;
    degenerate.center = degenerate.eye;
    Check(! camera.SetTransforms(degenerate), "camera rejects a zero-length view direction");
    degenerate        = authored;
    degenerate.up     = authored.center - authored.eye;
    Check(! camera.SetTransforms(degenerate), "camera rejects a collinear up vector");

    sr::Scene scene;
    auto      primary = std::make_shared<sr::SceneCamera>(camera);
    auto      linked  = std::make_shared<sr::SceneCamera>(camera);
    scene.cameras.emplace("primary", primary);
    scene.cameras.emplace("linked", linked);
    scene.linkedCameras["primary"].push_back("linked");
    scene.activeCamera = primary.get();

    const sr::SceneCameraTransforms changed {
        .eye = { 8.0, 7.0, 6.0 },
        .center = { 0.0, 1.0, 0.0 },
        .up = { 0.0, 1.0, 1.0 },
    };
    Check(scene.SetActiveCameraTransforms(changed),
          "scene applies transforms to the active camera");
    const auto active = scene.ActiveCameraTransforms();
    Check(active.has_value() && active->eye.isApprox(changed.eye) &&
              linked->Transforms().eye.isApprox(changed.eye),
          "active camera transforms propagate to linked cameras");
}

void TestMaterialKeyAliases() {
    sr::Scene         scene;
    sr::SceneMaterial material;
    sr::SceneShaderVariantDesc variant;
    variant.uniform_aliases["Tint"] = "g_Tint";
    material.customShader.variant   = std::move(variant);
    material.customShader.constValues["g_Tint"] =
        sr::ShaderValue(std::array<float, 3> { 1.0f, 1.0f, 1.0f });

    Check(scene.SetMaterialShaderValueByKey(
              material, "Tint", sr::ShaderValue(std::array<float, 3> { 0.2f, 0.4f, 0.6f })),
          "material key writes resolve through shader uniform aliases");
    const auto& value = material.customShader.constValues.at("g_Tint");
    Check(value.size() == 3 && Near(value[0], 0.2f) && Near(value[1], 0.4f) &&
              Near(value[2], 0.6f),
          "material alias writes update the resolved uniform");
    Check(! material.customShader.constValues.contains("Tint"),
          "material alias writes do not create an unresolved duplicate");
}

void TestLimitedStreamSeekSemantics() {
    std::vector<uint8_t> bytes(12, 0);
    auto backing = std::make_shared<sr::fs::MemBinaryStream>(std::move(bytes));
    sr::fs::LimitedBinaryStream stream(backing, 3, 6);

    Check(stream.SeekEnd(0) && stream.Tell() == 6,
          "limited stream seek-to-end reaches the position after the final byte");
    Check(stream.SeekEnd(-2) && stream.Tell() == 4,
          "limited stream negative end seek uses standard relative offsets");
    Check(! stream.SeekEnd(1) && stream.Tell() == 4,
          "limited stream rejects positions past its logical end");
}

void TestMaterialAndModelSchemaCompatibility() {
    sr::wpscene::ObjectInstance instance;
    Check(instance.FromJson(Parse(
              R"JSON({"textures":["override-a",null,"override-c"],"combos":{"MODE":2}})JSON")),
          "material instance overrides parse");
    sr::wpscene::Material instance_material;
    instance_material.textures = { "base-a", "base-b" };
    instance.ApplyTo(instance_material);
    Check(instance_material.textures.size() == 3 &&
              instance_material.textures[0] == "override-a" &&
              instance_material.textures[1] == "base-b" &&
              instance_material.textures[2] == "override-c",
          "material instances preserve empty texture slots while applying overrides");
    Check(instance_material.combos.at("MODE") == 2,
          "material instances apply authored combo overrides");

    sr::wpscene::Material scripted_material;
    Check(scripted_material.FromJson(Parse(R"JSON({
        "passes":[{
            "shader":"unit",
            "constantshadervalues":{
                "Tint":{
                    "value":[0.1,0.2,0.3,0.4],
                    "script":"export function update() { return new Vec4(1, 2, 3, 4); }",
                    "scriptproperties":{"speed":1}
                }
            }
        }]
    })JSON")),
          "scripted material constant parses");
    const auto binding = scripted_material.constantshadervalues_bindings.scripts.find("Tint");
    Check(binding != scripted_material.constantshadervalues_bindings.scripts.end() &&
              ! binding->second.source.empty(),
          "material constants retain their field script bindings");
    auto cloned_material = scripted_material.clone();
    Check(cloned_material.constantshadervalues_bindings.scripts.contains("Tint"),
          "material clones retain constant field script bindings");

    sr::fs::VFS            vfs;
    sr::wpscene::ModelObject model;
    Check(model.FromJson(Parse(R"JSON({"model":"models/unit.mdl","skin":3})JSON"), vfs) &&
              model.skin == 3,
          "model objects retain their authored material skin index");
}

void TestObjectSpaceRotation() {
    sr::SceneNode node;
    node.RotateObjectSpace({ 0.0f, 0.0f, static_cast<float>(std::numbers::pi / 2.0) });
    Check(Near(node.Rotation().x(), 0.0f) && Near(node.Rotation().y(), 0.0f) &&
              Near(node.Rotation().z(), static_cast<float>(std::numbers::pi / 2.0)),
          "object-space rotation composes onto the authored node orientation");
}

void TestShortShaderVectorShaping() {
    sr::SceneMaterial material;
    material.customShader.constValues["g_Test"] =
        sr::ShaderValue(std::array<float, 4> { 9.0f, 9.0f, 9.0f, 9.0f });
    material.SetShaderValue("g_Test", sr::ShaderValue(std::array<float, 2> { 1.0f, 2.0f }));
    const auto& vector = material.customShader.constValues.at("g_Test");
    Check(vector.size() == 4, "short shader vector retains declared width");
    Check(Near(vector[0], 1.0f) && Near(vector[1], 2.0f) && Near(vector[2], 0.0f) &&
              Near(vector[3], 0.0f),
          "short shader vector preserves supplied components and zero-fills tail");

    material.SetShaderValue("g_Test", sr::ShaderValue(0.25f));
    const auto& scalar = material.customShader.constValues.at("g_Test");
    Check(Near(scalar[0], 0.25f) && Near(scalar[1], 0.25f) && Near(scalar[2], 0.25f) &&
              Near(scalar[3], 0.25f),
          "scalar shader value still splats to declared vector width");
}

void TestAlphaToCoveragePipelineState() {
    VkPipelineColorBlendAttachmentState blend {};
    sr::vulkan::SetBlend(sr::BlendMode::AlphaToCoverage, blend);
    Check(blend.blendEnable == VK_FALSE, "alpha-to-coverage disables conventional blending");

    VkPipelineMultisampleStateCreateInfo multisample {};
    sr::vulkan::SetAlphaToCoverage(sr::BlendMode::AlphaToCoverage, multisample);
    Check(multisample.alphaToCoverageEnable == VK_TRUE,
          "alpha-to-coverage enables multisample coverage conversion");

    VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    sr::vulkan::SetAttachmentLoadOp(sr::BlendMode::AlphaToCoverage, load_op);
    Check(load_op == VK_ATTACHMENT_LOAD_OP_LOAD,
          "alpha-to-coverage preserves the existing color attachment");
    Check(sr::vulkan::IsDepthWritingBlendMode(sr::BlendMode::AlphaToCoverage),
          "alpha-to-coverage retains material depth writes");
}

void TestTextSurfaceSelection() {
    Check(sr::ResolveTextRenderMode({}) == sr::TextRenderMode::Direct,
          "ordinary text renders directly");
    Check(sr::ResolveTextRenderMode({ .has_effect = true }) == sr::TextRenderMode::Offscreen,
          "text effects retain an independent surface");
    Check(sr::ResolveTextRenderMode({ .copy_background = true }) == sr::TextRenderMode::Offscreen,
          "copy-background text retains an independent surface");
    Check(sr::ResolveTextRenderMode({ .opaque_background = true }) == sr::TextRenderMode::Offscreen,
          "opaque-background text retains an independent surface");
    Check(sr::ResolveTextRenderMode({ .linked_source = true }) == sr::TextRenderMode::Offscreen,
          "linked text retains a sampleable surface");
}

void TestDirectShapeLayerState() {
    sr::SceneNode             node;
    sr::SceneImageEffectLayer layer(&node, 1920.0f, 1080.0f, "shape_a", "shape_b");
    Check(layer.RequiresSourceDraw(), "ordinary effect layer draws its image source");
    layer.SetRequiresSourceDraw(false);
    Check(! layer.RequiresSourceDraw(), "direct shape effect layer suppresses image source draw");
    Check(sr::wpscene::ShapeObject {}.reflected,
          "shape layers participate in planar reflection by default");

    sr::SceneNode source;
    Eigen::Matrix4d frame = Eigen::Matrix4d::Identity();
    frame(0, 3)          = 42.0;
    source.SetLocalFrame(frame);
    sr::SceneNode copy;
    copy.CopyTrans(source);
    Check(copy.LocalFrame().isApprox(frame), "transform copies preserve puppet attachment frame");
}

void TestJsonArraysAndSceneDocumentMetadata() {
    auto root = Parse(R"JSON({
        "camera": {},
        "general": {
            "clearcolor": [0.1, 0.2, 0.3],
            "orthogonalprojection": {"width": 1920, "height": 1080}
        },
        "objects": [
            {
                "id": 42,
                "name": "Shape",
                "shape": "rectangle",
                "visible": {"value": false, "user": {"name": "show_shape"}},
                "solid": true,
                "origin": [1.0, 2.0, 3.0]
            },
            {"name": "Container"}
        ]
    })JSON");
    auto document = sr::wpscene::ParseSceneDocumentValue(
        std::move(root), sr::wpscene::kSceneVersionUnknown);
    Check(document.has_value(), "scene document accepts authored JSON arrays");
    if (! document) return;

    Check(document->metadata.general.clearcolor == std::array<float, 3> { 0.1f, 0.2f, 0.3f },
          "fixed-size numeric fields decode from JSON arrays");
    Check(document->objects_are_array && document->objects.size() == 2,
          "scene document preserves the canonical object array");
    const auto& shape = document->objects[0];
    Check(shape.metadata.kind == sr::wpscene::SceneObjectKind::Shape,
          "scene document classifies shape objects");
    Check(shape.metadata.has_id && shape.metadata.id == 42,
          "scene document distinguishes authored IDs from default zero");
    Check(! shape.metadata.visible && shape.metadata.visible_user.name == "show_shape",
          "scene document preserves visibility user bindings");
    Check(shape.metadata.solid, "scene document preserves solid metadata");
    Check(shape.authored.get("origin").is_some(),
          "scene document retains the authored object record");
    Check(! document->objects[1].metadata.has_id,
          "scene document records a missing object ID explicitly");

    std::vector<float> dynamic { 9.0f };
    Check(sr::GetJsonValue(Parse("[1, 2.5, 3]"), dynamic) &&
              dynamic == std::vector<float> { 1.0f, 2.5f, 3.0f },
          "dynamic numeric fields decode from JSON arrays");
    std::array<float, 2> wrong_size {};
    Check(! sr::GetJsonValue(Parse("[1, 2, 3]"), wrong_size),
          "fixed-size numeric fields reject mismatched JSON arrays");

    auto invalid_objects = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({"camera": {}, "general": {}, "objects": {}})JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(invalid_objects.has_value() && ! invalid_objects->objects_are_array,
          "scene document records non-array objects without inventing entries");
}

void TestEffectSelfCompositeStaysLocal() {
    const char* assets_root = std::getenv("SCENERENDERER_ASSETS_DIR");
    if (assets_root == nullptr || assets_root[0] == '\0') return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(assets_root)),
          "self-composite regression mounts the Wallpaper Engine assets");
    if (! vfs.Open("/assets/effects/godrays/effect.json")) return;

    auto document = sr::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 568,
                "name": "Self Composite",
                "image": "models/util/fullscreenlayer.json",
                "copybackground": true,
                "effects": [{
                    "file": "effects/godrays/effect.json",
                    "visible": true,
                    "passes": [{}, {}, {}, {}, {
                        "textures": [null, "_rt_imageLayerComposite_568_a"]
                    }]
                }],
                "visible": true
            }]
        })JSON",
        sr::wpscene::kSceneVersionUnknown);
    Check(document.has_value(), "self-composite regression parses its scene document");
    if (! document) return;

    wavsen::audio::SoundManager sound_manager;
    sr::WPSceneParser            parser;
    auto scene = parser.Parse("self-composite", *document, vfs, sound_manager);
    Check(scene != nullptr, "self-composite regression compiles its scene");
    if (! scene) return;

    const auto layer = sr::WallpaperLayerId { .value = 568 };
    auto       snapshot = sr::ExtractRenderSceneSnapshot(*scene);
    Check(! snapshot.HasLinkConsumer(layer),
          "an effect sampling its own layer composite stays local");
    Check(scene->renderTargets.count(sr::GenLinkTex(568)) == 0,
          "a self-composite does not allocate an external link target");
}

void TestDynamicCopySnapshotMatchesSourceRequest() {
    sr::Scene scene;
    scene.renderTargets["_rt_snapshot_source"] = {
        .width  = 640,
        .height = 360,
    };
    scene.renderTargets["_rt_snapshot_copy"] = {
        .width  = 1920,
        .height = 1080,
    };

    sr::vulkan::CopyPass pass(sr::vulkan::CopyPass::Desc {
        .src             = "_rt_snapshot_source",
        .dst             = "_rt_snapshot_copy",
        .dst_matches_src = true,
    });
    (void)pass.finalizeResourceRequests(scene);
    const auto diagnostics = pass.textureRequestDiagnostics();
    Check(diagnostics.size() == 2 && diagnostics[0].request && diagnostics[1].request,
          "dynamic snapshot copy resolves both resource requests");
    if (diagnostics.size() != 2 || ! diagnostics[0].request || ! diagnostics[1].request) return;

    const auto& src = *diagnostics[0].request;
    const auto& dst = *diagnostics[1].request;
    Check(src.cache_key && dst.cache_key && sr::vulkan::SameTextureKey(*src.cache_key, *dst.cache_key),
          "dynamic snapshot destination inherits the exact source allocation description");
    Check(dst.name == "_rt_snapshot_copy",
          "dynamic snapshot destination keeps its generated resource identity");
}

void TestFinalResolvePrecedesLinkPublication() {
    sr::Scene scene;
    scene.renderTargets[std::string(sr::SpecTex_Default)] = {
        .width  = 64,
        .height = 64,
        .bind   = { .enable = true, .screen = true },
    };
    const std::string pingpong_a = "_rt_effect_pingpong_a_final_publish_test";
    const std::string pingpong_b = "_rt_effect_pingpong_b_final_publish_test";
    scene.renderTargets[pingpong_a] = { .width = 64, .height = 64, .allowReuse = true };
    scene.renderTargets[pingpong_b] = { .width = 64, .height = 64, .allowReuse = true };
    scene.default_effect_mesh.Submeshes().emplace_back();

    auto source = rstd::sync::Arc<sr::SceneNode>::make();
    source->ID() = 42;
    source->SetCamera("final_publish_effect");
    auto source_mesh = std::make_shared<sr::SceneMesh>();
    source_mesh->Submeshes().emplace_back();
    sr::SceneMaterial source_material;
    source_material.name = "final_publish_source";
    source_mesh->AddMaterial(std::move(source_material));
    source->AddMesh(source_mesh);

    auto final_node = rstd::sync::Arc<sr::SceneNode>::make();
    auto final_mesh = std::make_shared<sr::SceneMesh>();
    final_mesh->Submeshes().emplace_back();
    sr::SceneMaterial final_material;
    final_material.name     = "final_publish_resolve";
    final_material.textures = { pingpong_a };
    final_mesh->AddMaterial(std::move(final_material));
    final_node->AddMesh(final_mesh);
    auto final_effect = std::make_shared<sr::SceneImageEffect>();
    final_effect->nodes.push_back(sr::SceneImageEffectNode {
        .output    = pingpong_b,
        .sceneNode = final_node.clone(),
    });

    auto effect_layer =
        std::make_shared<sr::SceneImageEffectLayer>(source.as_ptr(), 64.0f, 64.0f,
                                                    pingpong_a, pingpong_b);
    effect_layer->SetFullscreen(true);
    effect_layer->SetFinalResolveEffect(final_effect);
    auto effect_camera = std::make_shared<sr::SceneCamera>(
        sr::SceneCamera::MakeOrthographic(64.0, 64.0, -1.0, 1.0));
    effect_camera->AttatchImgEffect(effect_layer);
    scene.cameras.emplace("final_publish_effect", effect_camera);
    scene.cameras.emplace(
        "effect",
        std::make_shared<sr::SceneCamera>(
            sr::SceneCamera::MakeOrthographic(64.0, 64.0, -1.0, 1.0)));

    auto consumer = rstd::sync::Arc<sr::SceneNode>::make();
    consumer->ID() = 43;
    auto consumer_mesh = std::make_shared<sr::SceneMesh>();
    consumer_mesh->Submeshes().emplace_back();
    sr::SceneMaterial consumer_material;
    consumer_material.name     = "final_publish_consumer";
    consumer_material.textures = { sr::GenLinkTex(42) };
    consumer_mesh->AddMaterial(std::move(consumer_material));
    consumer->AddMesh(consumer_mesh);

    scene.RegisterNode(*source, sr::WallpaperLayerId { .value = 42 });
    scene.RegisterNode(*consumer, sr::WallpaperLayerId { .value = 43 });
    scene.sceneGraph->AppendChild(source.clone());
    scene.sceneGraph->AppendChild(consumer.clone());

    auto graph = sr::sceneToRenderGraph(scene);
    Check(graph != nullptr, "final-resolve publication regression builds a render graph");
    if (! graph) return;

    bool        saw_final_resolve = false;
    bool        consumer_reads_published_final = false;
    std::size_t final_order = std::numeric_limits<std::size_t>::max();
    std::size_t consumer_order = std::numeric_limits<std::size_t>::max();
    auto        order = graph->topologicalOrder();
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto state = graph->passState(order[i]);
        if (! state) continue;
        if (state->name == "final_publish_resolve") {
            saw_final_resolve = true;
            final_order       = i;
        }
        if (state->name != "final_publish_consumer") continue;
        consumer_order = i;
        auto* pass = static_cast<sr::vulkan::VulkanPass*>(graph->getPass(order[i]));
        if (pass == nullptr) continue;
        for (const auto& diagnostic : pass->textureRequestDiagnostics()) {
            if (diagnostic.role == "sampled" && diagnostic.slot == 0 &&
                diagnostic.name == sr::GenLinkTex(42)) {
                consumer_reads_published_final = true;
            }
        }
    }
    Check(saw_final_resolve, "explicit final-resolve is emitted into the render graph");
    Check(final_order < consumer_order,
          "explicit final-resolve executes before its external link consumer");
    Check(consumer_reads_published_final,
          "link consumer samples the version published after explicit final-resolve");
}

void TestUserPropertyIndexesOwnTheirTargets() {
    sr::Scene scene;
    std::weak_ptr<sr::SceneMaterial> material_weak;
    {
        auto node     = rstd::sync::Arc<sr::SceneNode>::make();
        auto material = std::make_shared<sr::SceneMaterial>();
        material_weak = material;
        scene.image_color_user_index["tint"].push_back(
            { node.clone(), { material } });
        scene.image_alpha_user_index["fade"].push_back(
            { node.clone(), { material } });
        scene.shader_user_var_index["strength"].push_back({ material, "g_Strength" });
        scene.material_texture_user_index["image"].push_back(
            { .material = material, .slot = 0, .fallback = "util/white" });
    }

    Check(! material_weak.expired(),
          "scene user-property indexes retain materials after parse-owned references are released");
    const auto& color_binding = scene.image_color_user_index.at("tint").front();
    Check(color_binding.node && color_binding.materials.front(),
          "image user-property indexes retain both node and material targets");
    color_binding.node->SetColor({ 0.2f, 0.4f, 0.6f });
    Check(Near(color_binding.node->Color().x(), 0.2f),
          "retained image user-property node remains writable");
}

void TestMdlv23MultiCurveMorphEvents() {
    const char* workshop_root = std::getenv("SCENERENDERER_WORKSHOP_DIR");
    if (workshop_root == nullptr || workshop_root[0] == '\0') return;

    const auto pkg_path = std::filesystem::path(workshop_root) / "3686252018" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) return;

    sr::fs::VFS vfs;
    auto pkg = sr::fs::WPPkgFs::CreatePkgFs(pkg_path.string());
    Check(pkg != nullptr && vfs.Mount("/assets", std::move(pkg)),
          "multi-curve MDL sample mounts as a scene package");
    if (! vfs.Open("/assets/models/sheet_puppet.mdl")) return;

    sr::WPMdl mdl;
    Check(sr::WPMdlParser::Parse("models/sheet_puppet.mdl", vfs, mdl),
          "MDLV23 multi-curve puppet parses");
    if (! mdl.puppet || mdl.puppet->anims.empty()) return;

    Check(mdl.mdla == 6, "multi-curve puppet uses MDLA version 6");
    Check(mdl.puppet->anims.size() == 17, "multi-curve puppet preserves all animations");
    const auto& left_eye = mdl.puppet->anims.front();
    Check(left_eye.name == "Left eye" && left_eye.v4_events.size() == 1,
          "multi-curve puppet preserves the Left eye morph event");
    if (left_eye.v4_events.empty()) return;

    const auto& event = left_eye.v4_events.front();
    Check(event.flags == 0 && event.curves.size() == 6,
          "MDLV23 morph event preserves all six curves");
    for (std::size_t i = 0; i < event.curves.size(); ++i) {
        const auto& curve = event.curves[i];
        Check(curve.id == i && curve.values.size() == 211 &&
                  ! curve.values.empty() && Near(curve.values.front(), 1.0f),
              "MDLV23 morph curve preserves id, sample count, and first value");
    }
    Check(mdl.morph_sections.size() == 1 &&
              Near(mdl.morph_sections.front().event_time, event.time) &&
              mdl.morph_sections.front().sections.size() == event.curves.size(),
          "MDMP morph sections align with the MDLA event curves");
}

void TestShaderHlslSemanticCompatibility() {
    sr::SceneShaderVariantDesc desc;
    desc.scene_id    = "hlsl-semantic-compatibility-test";
    desc.shader_name = "hlsl-semantic-compatibility-test";
    desc.stages.push_back(sr::SceneShaderVariantStage {
        .stage      = sr::ShaderType::VERTEX,
        .source_key = "/assets/shaders/hlsl-semantic-compatibility-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    float value = 2.0;
    float scaled = mul(1, value / 50.0);
    gl_Position = vec4(a_Position + vec3(scaled * 0.0), 1.0);
}
)",
    });
    desc.stages.push_back(sr::SceneShaderVariantStage {
        .stage      = sr::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/hlsl-semantic-compatibility-test.frag",
        .source     = R"(
float tint;
void main() {
    tint = 0.25;
    gl_FragColor = vec4(tint, tint, tint, 1.0);
}
)",
    });

    sr::fs::VFS vfs;
    const auto result = sr::WPShaderParser::CompileSceneShaderVariant(desc, vfs);
    Check(result.ok && result.shader && result.shader->codes.size() == 2,
          "leading integer scalar mul compiles through the HLSL path");
    if (! result.ok || ! result.shader) return;

    std::vector<sr::vulkan::Uni_ShaderSpv> reflected_spvs;
    sr::vulkan::ShaderReflected            reflection;
    Check(sr::vulkan::GenReflect(result.shader->codes, reflected_spvs, reflection),
          "private shader globals reflect successfully");
    Check(std::none_of(reflection.blocks.begin(), reflection.blocks.end(), [](const auto& block) {
              return block.name == "$Global";
          }),
          "file-scope GLSL variables do not become an HLSL $Global uniform block");
}

void TestMissingTexturePlaceholderSemantics() {
    auto image = sr::vulkan::MakeMissingTexturePlaceholder("missing/test");
    Check(image && image->key == "missing/test" && image->header.width == 2 &&
              image->header.height == 2 && image->header.format == sr::TextureFormat::RGBA8 &&
              image->slots.size() == 1 && image->slots.front().mipmaps.size() == 1,
          "missing texture placeholder has a complete 2x2 RGBA8 image layout");
    if (image && ! image->slots.empty() && ! image->slots.front().mipmaps.empty()) {
        const auto& mip = image->slots.front().mipmaps.front();
        bool opaque_magenta = mip.size == 16 && mip.data != nullptr;
        const auto* pixels = mip.data.get();
        for (std::size_t i = 0; opaque_magenta && i < 4; ++i) {
            opaque_magenta = pixels[i * 4 + 0] == 0xFF && pixels[i * 4 + 1] == 0x00 &&
                             pixels[i * 4 + 2] == 0xFF && pixels[i * 4 + 3] == 0xFF;
        }
        Check(opaque_magenta, "missing texture placeholder pixels are opaque magenta");
    }

    sr::RenderSceneSnapshot snapshot;
    ProbeImageParser        parser;
    sr::vulkan::SnapshotImportedTextureProvider provider(snapshot, &parser);
    sr::vulkan::TextureRequest request {
        .kind = sr::vulkan::TextureRequestKind::Imported,
        .name = "missing/test",
    };
    Check(provider.ParseImportedTexture(request) != nullptr,
          "absent scene texture is replaced with a placeholder");
    parser.contains = true;
    Check(provider.ParseImportedTexture(request) == nullptr,
          "an existing but undecodable texture remains a hard decode failure");
}

void TestParticleRuntimeState() {
    using SpawnType = sr::ParticleSubSystem::SpawnType;
    Check(sr::ParticleSubSystem::EffectiveInstanceCapacity(12, SpawnType::STATIC) == 1,
          "static particle systems allocate one persistent instance");
    Check(sr::ParticleSubSystem::EffectiveInstanceCapacity(12, SpawnType::EVENT_SPAWN) == 12,
          "event particle systems retain their authored instance pool");
    const auto capacity =
        sr::ParticleSubSystem::MaxParticleCapacity(256, 12, SpawnType::EVENT_SPAWN);
    Check(capacity.has_value() && *capacity == 3072,
          "particle mesh capacity includes every event instance");
    Check(! sr::ParticleSubSystem::MaxParticleCapacity(
               std::numeric_limits<uint32_t>::max(), 2, SpawnType::EVENT_SPAWN)
               .has_value(),
          "particle mesh capacity rejects integer overflow");

    sr::ParticleTrail trail;
    trail.positions.resize(3);
    trail.Initialize({ 1.0f, 2.0f, 3.0f });
    Check(trail.sample_count == 1 && trail.len == 3,
          "rope trail initialization exposes one real sample without uninitialized history");
    trail.Push({ 2.0f, 3.0f, 4.0f });
    trail.Push({ 3.0f, 4.0f, 5.0f });
    Check(trail.sample_count == 3 && trail.At(0).isApprox(Eigen::Vector3f(1.0f, 2.0f, 3.0f)) &&
              trail.At(2).isApprox(Eigen::Vector3f(3.0f, 4.0f, 5.0f)),
          "rope trail ring buffer preserves oldest-to-newest sample order");

    sr::SceneNode node;
    bool          playing = true;
    float         alpha   = 1.0f;
    node.SetLayerPropertyControl(
        [&alpha](std::string_view field) {
            return field == "alpha" ? std::vector<float> { alpha } : std::vector<float> {};
        },
        [&alpha](std::string_view field, std::span<const float> values) {
            if (field == "alpha" && ! values.empty()) alpha = values.front();
        });
    node.SetPlaybackControl(
        [&playing]() { playing = true; },
        [&playing]() { playing = false; },
        [&playing]() { playing = false; },
        [&playing]() { return playing; });
    const std::array<float, 1> changed_alpha { 0.25f };
    Check(node.ApplyControlledProperty("alpha", changed_alpha),
          "particle node accepts instance override writes");
    const auto read_alpha = node.ControlledProperty("alpha");
    Check(read_alpha.has_value() && Near(read_alpha->front(), 0.25f),
          "particle node returns the current instance override value");
    node.Pause();
    Check(! node.IsPlaying(), "particle playback control pauses the shared particle tree");
    node.Play();
    Check(node.IsPlaying(), "particle playback control resumes the shared particle tree");
}

void TestPlaybackSpeedAndAtomicCachePublication() {
    Check(sr::IsValidScenePlaybackSpeed(1.0f) && sr::IsValidScenePlaybackSpeed(4.0f),
          "positive finite scene playback speeds are accepted");
    Check(! sr::IsValidScenePlaybackSpeed(0.0f) &&
              ! sr::IsValidScenePlaybackSpeed(-1.0f) &&
              ! sr::IsValidScenePlaybackSpeed(std::numeric_limits<float>::infinity()) &&
              ! sr::IsValidScenePlaybackSpeed(std::numeric_limits<float>::quiet_NaN()),
          "non-positive and non-finite scene playback speeds are rejected");

    int marker = 0;
    const auto root = std::filesystem::temp_directory_path() /
                      ("scenerenderer-cache-publication-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(&marker)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    Check(! ec, "atomic cache regression creates its isolated temporary directory");
    if (ec) return;

    sr::fs::VFS vfs;
    Check(vfs.Mount("/cache", sr::fs::CreatePhysicalFs(root.string()), "cache"),
          "atomic cache regression mounts a writable cache directory");
    const auto publish = [&vfs](std::string_view value, bool commit) {
        return vfs.Publish("/cache/shaders/test.spv",
                           [value, commit](sr::fs::IBinaryStreamW& stream) {
                               return stream.WriteAll(value.data(), value.size()) && commit;
                           });
    };
    Check(publish("stable", true), "initial cache artifact publishes successfully");
    Check(! publish("partial", false), "failed cache publication is reported");
    auto after_failure = vfs.Open("/cache/shaders/test.spv");
    Check(after_failure && after_failure->ReadAllStr() == "stable",
          "failed cache publication leaves the previous artifact intact");
    Check(publish("replacement", true), "replacement cache artifact publishes successfully");
    auto after_success = vfs.Open("/cache/shaders/test.spv");
    Check(after_success && after_success->ReadAllStr() == "replacement",
          "successful cache publication atomically replaces the previous artifact");
    after_failure.reset();
    after_success.reset();
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    TestExplicitCameraFactories();
    TestPerspectiveFillModePreservesFov();
    TestOrthographicFillModeDerivesPerspectiveFov();
    TestAuthoredSceneZoom();
    TestAnimatedSceneZoom();
    TestAnimatedSceneZoomWithCameraPath();
    TestInvalidAnimatedSceneZoom();
    TestPointerUniformsIgnoreParallaxDelay();
    TestPlanarReflectionSemantics();
    TestWrappedAnimationCurves();
    TestFieldAnimationPlayback();
    TestSoundVisibilityAndVolume();
    TestCameraTransformControls();
    TestMaterialKeyAliases();
    TestLimitedStreamSeekSemantics();
    TestMaterialAndModelSchemaCompatibility();
    TestObjectSpaceRotation();
    TestShortShaderVectorShaping();
    TestAlphaToCoveragePipelineState();
    TestTextSurfaceSelection();
    TestDirectShapeLayerState();
    TestJsonArraysAndSceneDocumentMetadata();
    TestEffectSelfCompositeStaysLocal();
    TestDynamicCopySnapshotMatchesSourceRequest();
    TestFinalResolvePrecedesLinkPublication();
    TestUserPropertyIndexesOwnTheirTargets();
    TestMdlv23MultiCurveMorphEvents();
    TestShaderHlslSemanticCompatibility();
    TestMissingTexturePlaceholderSemantics();
    TestParticleRuntimeState();
    TestPlaybackSpeedAndAtomicCachePublication();
    if (g_failures == 0) std::cout << "RuntimeCompatibilityRegression: ok\n";
    return g_failures == 0 ? 0 : 1;
}
