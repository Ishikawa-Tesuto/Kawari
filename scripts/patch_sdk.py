#!/usr/bin/env python3
"""Apply deliberately version-specific hooks to a private copy of Cubism 5-r.5."""
import sys
from pathlib import Path
import shutil
import re

MARKER = "// KAWARI_P02C1"

def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise ValueError(f"SDK layout mismatch: {label} (expected exactly one anchor)")
    return text.replace(old, new, 1)

def guard_preview_submit(text):
    """Guard only the WSI clear/first submit, not the model update or export.

    SDK distributions may add blank lines/comments or move SetViewMatrix.
    Locate a unique begin and model update, then a unique first-submit call
    within that interval. Fail closed if the expected structure is absent.
    """
    label = 'background skip preview submit'
    begin = re.compile(r'(?m)^(?P<indent>[ \t]*)VkCommandBuffer[ \t]+commandBuffer[ \t]*=[ \t]*vkManager->BeginSingleTimeCommands\(\);[ \t]*\n[ \t]*ChangeBeginLayout\(commandBuffer\);')
    update = re.compile(r'(?m)^[ \t]*live2DManager->OnUpdate\(\);')
    begins, updates = list(begin.finditer(text)), list(update.finditer(text))
    if len(begins) != 1 or len(updates) != 1 or begins[0].end() > updates[0].start():
        raise ValueError(f'SDK layout mismatch: {label} (expected one preview begin before one model update)')
    b, u = begins[0], updates[0]
    # First submit consumes the acquire semaphore. It must be inside the guard.
    submit = re.compile(r'(?m)^[ \t]*vkManager->SubmitCommand[ \t]*\([ \t]*commandBuffer[ \t]*,[ \t]*true[ \t]*\)[ \t]*;[^\n]*')
    matches = list(submit.finditer(text, b.end(), u.start()))
    if len(matches) != 1:
        raise ValueError(f'SDK layout mismatch: {label} (found {len(matches)} first submits between preview begin and model update)')
    m = matches[0]
    indent = b.group('indent')
    # Keep the declaration in outer scope for the later preview composition.
    opening = (f'{indent}VkCommandBuffer commandBuffer = VK_NULL_HANDLE;\n'
               f'{indent}if (!studio::background_frame()) {{\n'
               f'{indent}    commandBuffer = vkManager->BeginSingleTimeCommands();\n'
               f'{indent}    ChangeBeginLayout(commandBuffer);')
    return (text[:b.start()] + opening + text[b.end():m.end()] +
            '\n' + indent + '}' + text[m.end():])


# Injected into the local SDK copy, not distributed as SDK source.
MASK_POLICY_CPP = r"""
#include <algorithm>
#include <set>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>
namespace studio_mask {
using Group = std::vector<int>;
using Groups = std::set<Group>;
// A clipping context is identified by its set of source ArtMeshes, not the
// number of clipped meshes. Count drawable and offscreen contexts separately.
template<class Int>
Groups collect(int n, const Int* counts, const Int* const* masks) {
    Groups result;
    if (!counts || !masks) return result;
    for (int i=0; i<n; ++i) {
        if (counts[i]<=0 || !masks[i]) continue;
        Group g(masks[i], masks[i]+counts[i]);
        std::sort(g.begin(),g.end());
        result.insert(g);
    }
    return result;
}
inline int reused_sources(const Groups& draw, const Groups& offscreen) {
    std::set<int> seen, reused;
    const Groups* lists[] = {&draw,&offscreen};
    for (const Groups* groups: lists) for (const auto& g: *groups) {
        std::set<int> unique(g.begin(),g.end());
        for (int id: unique) if (!seen.insert(id).second) reused.insert(id);
    }
    return static_cast<int>(reused.size());
}
// A source can be written once per GPU submission. Split only when a
// subsequent context needs to overwrite a source already in that submission.
inline bool needs_split(const std::set<int>& used, const int* ids, int count) {
    for (int i=0;i<count;++i) if (used.count(ids[i])) return true;
    return false;
}
struct Policy { int buffers; bool high; const char* reason; };
inline Policy choose(int drawable, int offscreen, int reused, int requested, const char* mode) {
    const int groups=std::max(drawable,offscreen);
    if (mode && !std::strcmp(mode,"legacy")) return {std::max(1,requested),false,"legacy override"};
    if (mode && !std::strcmp(mode,"high")) return {1,true,"high override"};
    // R5: one atlas has 36 slots; with >=2 atlases each has 32.
    const int needed=groups<=36 ? 1 : 1+(groups-1)/32;
    const int buffers=std::max(needed,std::max(1,requested));
    // Avoid unbounded allocations for unusually large models.
    if (buffers>8) return {1,true,"atlas budget"};
    return {buffers,false,reused>0 ? "batched shared sources" : "automatic atlas"};
}
} // namespace studio_mask
"""


def patch_mask_compatibility(renderer):
    # The standard renderer stores one mask UBO per source drawable, so when
    # two contexts reuse it in the batched path the last matrix can win.
    # Keep atlas rendering and split submissions only at a conflicting context.
    renderer = replace_once(renderer, '#include "CubismRenderer_Vulkan.hpp"',
        '#include "CubismRenderer_Vulkan.hpp"\n' + MASK_POLICY_CPP, 'mask policy helper')
    renderer = replace_once(renderer,
        '    CubismRenderer::Initialize(model, maskBufferCount);', r'''    const auto studioDrawMasks = studio_mask::collect(model->GetDrawableCount(),
        model->GetDrawableMaskCounts(), model->GetDrawableMasks());
    const auto studioOffscreenMasks = studio_mask::collect(model->GetOffscreenCount(),
        model->GetOffscreenMaskCounts(), model->GetOffscreenMasks());
    const int studioSharedMasks = studio_mask::reused_sources(studioDrawMasks, studioOffscreenMasks);
    const auto studioPolicy = studio_mask::choose(static_cast<int>(studioDrawMasks.size()),
        static_cast<int>(studioOffscreenMasks.size()), studioSharedMasks, maskBufferCount,
        std::getenv("STUDIO_MASK_MODE"));
    maskBufferCount = studioPolicy.buffers;
    CubismRenderer::Initialize(model, maskBufferCount);
    UseHighPrecisionMask(studioPolicy.high);
    std::fprintf(stdout, "[mask] drawable=%zu offscreen=%zu shared_sources=%d buffers=%d mode=%s (%s)\n",
        studioDrawMasks.size(), studioOffscreenMasks.size(), studioSharedMasks,
        maskBufferCount, studioPolicy.high ? "high" : "atlas", studioPolicy.reason);
    std::fprintf(stdout, "[model] drawables=%d offscreens=%d advanced_blend=%d\n",
        static_cast<int>(model->GetDrawableCount()), static_cast<int>(model->GetOffscreenCount()),
        static_cast<int>(model->IsBlendModeEnabled()));
    std::fflush(stdout);''', 'mask policy initialization')
    # Layout slots must not shift when inactive/empty contexts change. Vulkan
    # descriptors cache a specific mask image. Use all registered contexts for
    # layout; still clear the atlas before skipping invalid/degenerate bounds.
    renderer = replace_once(renderer, '    SetupLayoutBounds(usingClipCount);',
        '    SetupLayoutBounds(static_cast<csmInt32>(_clippingContextListForMask.GetSize()));', 'stable mask atlas slots')
    renderer = replace_once(renderer, '        _tmpBoundsOnModel.SetRect(allClippedDrawRect);',
        '''        if (!clipContext->_isUsing || allClippedDrawRect->Width <= 0.0f || allClippedDrawRect->Height <= 0.0f)
        {
            continue;
        }
        _tmpBoundsOnModel.SetRect(allClippedDrawRect);''', 'skip empty mask bounds')
    renderer = replace_once(renderer,
        '    SetupLayoutBounds(static_cast<csmInt32>(_clippingContextListForMask.GetSize()));',
        '    SetupLayoutBounds(static_cast<csmInt32>(_clippingContextListForMask.GetSize()));\n    std::set<int> studioUsedMaskSources;', 'batch source tracking')
    renderer = replace_once(renderer, '        _tmpBoundsOnModel.SetRect(allClippedDrawRect);', r'''        // At this point the correct atlas is active. Preserve its earlier
        // contents when resuming after a conflicting uniform-buffer write.
        if (studio_mask::needs_split(studioUsedMaskSources, clipContext->_clippingIdList, clipContext->_clippingIdCount))
        {
            _currentMaskBuffer->EndDraw(commandBuffer);
            const auto semaphore = renderer->_updateFinishedSemaphores[commandBufferCurrent];
            renderer->SubmitCommand(updateCommandBuffer, semaphore);
            renderer->SubmitCommand(commandBuffer, VK_NULL_HANDLE, semaphore);
            VkCommandBufferBeginInfo studioBegin{};
            studioBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(updateCommandBuffer, &studioBegin);
            vkBeginCommandBuffer(commandBuffer, &studioBegin);
            _currentMaskBuffer->BeginDraw(commandBuffer, 1.0f, 1.0f, 1.0f, 1.0f, false);
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &rect);
            studioUsedMaskSources.clear();
        }
        studioUsedMaskSources.insert(clipContext->_clippingIdList,
            clipContext->_clippingIdList + clipContext->_clippingIdCount);
        _tmpBoundsOnModel.SetRect(allClippedDrawRect);''', 'split conflicting mask batches')
    # Drawable and offscreen managers share the mask source descriptors.
    # Finish the former before the latter can overwrite those uniforms.
    renderer = replace_once(renderer,
        '            _offscreenClippingManager->SetupClippingContext(*GetModel(), drawCommandBuffer, updateCommandBuffer, this, _commandBufferCurrent, DrawableObjectType_Offscreen);', r'''            if (_drawableClippingManager != NULL)
            {
                SubmitCommand(updateCommandBuffer, _updateFinishedSemaphores[_commandBufferCurrent]);
                SubmitCommand(drawCommandBuffer, VK_NULL_HANDLE, _updateFinishedSemaphores[_commandBufferCurrent]);
                vkBeginCommandBuffer(updateCommandBuffer, &beginInfo);
                vkBeginCommandBuffer(drawCommandBuffer, &beginInfo);
            }
            _offscreenClippingManager->SetupClippingContext(*GetModel(), drawCommandBuffer, updateCommandBuffer, this, _commandBufferCurrent, DrawableObjectType_Offscreen);''', 'separate offscreen mask batch')
    return renderer


def patch(sdk, native):
    proj = sdk / "Samples/Vulkan/Demo/proj.linux.cmake"
    src = proj / "src"
    files = {}
    def read(name):
        p = src / name
        return p.read_text(encoding="utf-8-sig")
    def rep(text, old, new, label):
        return replace_once(text, old, new, label)
    manager = read("LAppLive2DManager.cpp")
    if '#include "studio_controls.hpp"' in manager:
        raise ValueError("SDK already patched. Remove staged SDK and rebuild.")
    manager = rep(manager, '#include "LAppLive2DManager.hpp"', '#include "LAppLive2DManager.hpp"\n#include <string>\n#include <cstdlib>\n#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>\n#include "studio_controls.hpp"\n' + MARKER, 'manager includes')
    manager = rep(manager, 'void LAppLive2DManager::SetUpModel()\n{', '''void LAppLive2DManager::SetUpModel()
{
    if (const char* chosen = std::getenv("STUDIO_MODEL")) {
        if (*chosen) {
            _modelDir.Clear();
            _modelDir.PushBack(csmString("Custom"));
            return;
        }
    }''', 'model discovery')
    manager = rep(manager, 'modelJsonName += ".model3.json";', '''modelJsonName += ".model3.json";
    if (const char* chosen = std::getenv("STUDIO_MODEL")) {
        std::string full(chosen);
        const auto slash = full.find_last_of('/');
        if (slash != std::string::npos) {
            modelPath = csmString(full.substr(0, slash + 1).c_str());
            modelJsonName = csmString(full.substr(slash + 1).c_str());
        }
    }''', 'model load')
    manager = rep(manager, '    ReleaseAllModel();\n    _models.PushBack(new LAppModel());', '    ReleaseAllModel();\n    studio::reset_controls();\n    _models.PushBack(new LAppModel());', 'model change')
    manager = rep(manager, '        LAppDelegate::GetInstance()->GetView()->PreModelDraw(*model);', '        studio::apply_camera(projection.GetArray());\n        LAppDelegate::GetInstance()->GetView()->PreModelDraw(*model);', 'camera transform')
    manager = rep(manager, '    _models[0]->LoadAssets(', '    if (studio::offscreen()) Csm::Rendering::CubismRenderer_Vulkan::EnableChangeRenderTarget();\n    _models[0]->LoadAssets(', 'offscreen enable')
    manager = rep(manager, '        LAppDelegate::GetInstance()->GetView()->SwitchRenderingTarget(useRenderTarget);', '        if (studio::offscreen()) useRenderTarget = LAppView::SelectTarget_ModelFrameBuffer;\n        LAppDelegate::GetInstance()->GetView()->SwitchRenderingTarget(useRenderTarget);', 'offscreen target')
    files[src / 'LAppLive2DManager.cpp'] = manager
    view = read('LAppView.cpp')
    view = rep(view, '#include "LAppView.hpp"', '#include "LAppView.hpp"\n#include "studio_controls.hpp"', 'view includes')
    view = rep(view, '    BeginRendering(commandBuffer, 0.0, 0.0, 0.0, 1.0, true);', '    const float bg = studio::background();\n    BeginRendering(commandBuffer, bg, bg, bg, 1.0, true);', 'preview background')
    view = rep(view, '    if(_spritePipeline != VK_NULL_HANDLE && _spritePipelineLayout != VK_NULL_HANDLE)', '    if(studio::demo_visible() && _spritePipeline != VK_NULL_HANDLE && _spritePipelineLayout != VK_NULL_HANDLE)', 'demo chrome')
    view = rep(view, '        live2DManager->OnTap(x, y);', '        if (studio::camera_is_default()) live2DManager->OnTap(x, y);\n        if (!studio::demo_visible()) return;', 'hidden button hit testing')
    view = rep(view, '                LAppModel* model = live2DManager->GetModel(i);\n                commandBuffer', '                LAppModel* model = live2DManager->GetModel(i);\n                if (!model || !model->GetModel() || !model->GetRenderBuffer().IsValid()) continue;\n                commandBuffer', 'offscreen valid image')
    files[src / 'LAppView.cpp'] = view
    model = read('LAppModel.cpp')
    model = rep(model, '#include "LAppModel.hpp"', '#include "LAppModel.hpp"\n#include "studio_controls.hpp"', 'model includes')
    model = rep(model, '#include "studio_controls.hpp"', '#include "studio_controls.hpp"\n#include <Motion/ICubismUpdater.hpp>', 'parameter updater include')
    model = rep(model, 'void LAppModel::SetupModel(ICubismModelSetting* setting)', """namespace {
class StudioParameterUpdater final : public Live2D::Cubism::Framework::ICubismUpdater {
public:
    StudioParameterUpdater() : Live2D::Cubism::Framework::ICubismUpdater(Live2D::Cubism::Framework::CubismUpdateOrder_Physics - 1) {}
    void OnLateUpdate(Live2D::Cubism::Framework::CubismModel* model, float) override {
        studio::update_controls(model, LAppDelegate::GetInstance()->GetWindow());
    }
};
}
void LAppModel::SetupModel(ICubismModelSetting* setting)""", 'parameter updater class')
    model = rep(model, '    _updateScheduler.SortUpdatableList();', '    _updateScheduler.AddUpdatableList(CSM_NEW StudioParameterUpdater());\n    _updateScheduler.SortUpdatableList();', 'register parameter updater')
    model = rep(model, '    _model->Update();\n}', '    studio::apply_manual_overrides(_model);\n    _model->Update();\n}', 'final manual override')
    # P0.5b3: honor Repeat flags from MOC3 before the first motion or physics update.
    model = rep(model, '        LoadModel(buffer, size);', '        LoadModel(buffer, size);\n        if (_model) _model->SetOverrideFlagForModelParameterRepeat(false);', 'enable model repeat')
    model = rep(model, '#include <Motion/ICubismUpdater.hpp>', '#include <Motion/ICubismUpdater.hpp>\n#include <Motion/CubismMotionJson.hpp>', 'idle loop include')
    model = rep(model, '            tmpMotion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);', r'''            // A single explicitly looping Idle with cyclic curves is continuous,
            // rather than a new motion (and fade) at each revolution.
            if (strcmp(group, MotionGroupIdle) == 0 && count == 1 && _model)
            {
                CubismMotionJson idleJson(buffer, size);
                if (idleJson.IsValid() && idleJson.IsMotionLoop() && idleJson.GetMotionDuration() > 0.0f)
                {
                    bool cyclic = false;
                    for (csmInt32 c = 0; c < idleJson.GetMotionCurveCount(); ++c)
                    {
                        if (strcmp(idleJson.GetMotionCurveTarget(c), "Parameter") != 0) continue;
                        const auto curveId = idleJson.GetMotionCurveId(c);
                        for (csmInt32 p = 0; p < _model->GetParameterCount(); ++p)
                            if (_model->GetParameterId(p) == curveId && _model->IsRepeat(p)) { cyclic = true; break; }
                        if (cyclic) break;
                    }
                    if (cyclic)
                    {
                        // V2 adds a last-to-first interpolation frame, which can
                        // reverse a cyclic 0..period curve at the seam. V1 does not.
                        tmpMotion->SetMotionBehavior(CubismMotion::MotionBehavior_V1);
                        tmpMotion->SetLoop(true);
                        tmpMotion->SetLoopFadeIn(false);
                        LAppPal::PrintLogLn("[repeat] continuous cyclic Idle");
                    }
                }
            }
            tmpMotion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);''', 'cyclic idle loop')
    files[src / 'LAppModel.cpp'] = model
    delegate = read('LAppDelegate.cpp')
    delegate = rep(delegate, '#include "LAppDelegate.hpp"', '#include "LAppDelegate.hpp"\n#include "studio_app_icon.hpp"\n#include "studio_controls.hpp"', 'delegate includes')
    delegate = rep(delegate, '"SAMPLE"', '"Kawari — Vulkan"', 'window title')
    # A GLFW_NO_API window has no OpenGL context; these calls are invalid and unnecessary.
    delegate = rep(delegate, '    glfwMakeContextCurrent(_window);\n    glfwSwapInterval(1);', '', 'no GL context')
    delegate = rep(delegate, '        glfwPollEvents();', '        studio::begin_frame();\n        glfwPollEvents();\n        if (glfwWindowShouldClose(_window)) break;\n        studio::update_view(_window);\n        if (glfwWindowShouldClose(_window)) break;', 'frame begin')
    delegate = rep(delegate, '        VulkanManager::GetInstance()->PostDraw();\n        RecreateSwapchain();', '        VulkanManager::GetInstance()->PostDraw();\n        RecreateSwapchain();\n        studio::pace_frame();', 'frame cap')
    delegate = rep(delegate, '    _window = glfwCreateWindow(RenderTargetWidth, RenderTargetHeight,', '    _window = studio::app_icon::create_window(studio::window_width(RenderTargetWidth), studio::window_height(RenderTargetHeight),', 'window dimensions and icon')
    files[src / 'LAppDelegate.cpp'] = delegate
    cmake = (src / 'CMakeLists.txt').read_text()
    cmake += '\n# KAWARI_P01C\ntarget_sources(${APP_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/studio_controls.cpp)\n'
    files[src / 'CMakeLists.txt'] = cmake
    parent = (proj / 'CMakeLists.txt').read_text()
    parent += '\nfind_package(Threads REQUIRED)\ntarget_link_libraries(${APP_NAME} Threads::Threads)\n'
    files[proj / 'CMakeLists.txt'] = parent
    # Keep validation enabled; remove the obsolete device-layer configuration only.
    vm = read('VulkanManager.cpp')
    vm = rep(vm, """    if (_enableValidationLayers)
    {
        createInfo.enabledLayerCount = sizeof(validationLayers) / sizeof(validationLayers[0]);
        createInfo.ppEnabledLayerNames = validationLayers;
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }""", "    createInfo.enabledLayerCount = 0;\n    createInfo.ppEnabledLayerNames = nullptr;", 'device layers')
    files[src / 'VulkanManager.cpp'] = vm
    renderer_path = sdk / 'Framework/src/Rendering/Vulkan/CubismRenderer_Vulkan.cpp'
    renderer = renderer_path.read_text(encoding='utf-8-sig')
    renderer = rep(renderer, '    VkRenderingAttachmentInfoKHR depthStencilAttachment{};', """    // Studio: this attachment is cleared on every begin; discard old depth.
    // Order earlier depth writes before the new layout transition and clear.
    VkImageMemoryBarrier depthBarrier{};
    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = _depthImage.GetImage();
    depthBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(drawCommandBuffer, depthStages, depthStages, 0,
                         0, nullptr, 0, nullptr, 1, &depthBarrier);
    VkRenderingAttachmentInfoKHR depthStencilAttachment{};""", 'main depth attachment transition')
    files[renderer_path] = renderer
    # P0.2b: optional PipeWire output using the very same Cubism device/queue.
    view = files[src / 'LAppView.cpp']
    view = rep(view, '#include "studio_controls.hpp"', '#include "studio_controls.hpp"\n#include "studio_broadcast.hpp"\n#include "studio_items_vk.hpp"', 'broadcast view include')
    view = rep(view, '    live2DManager->OnUpdate();', '''    live2DManager->OnUpdate();
    if (_renderTarget == SelectTarget_ModelFrameBuffer && live2DManager->GetModelNum() > 0)
    {
        LAppModel* model = live2DManager->GetModel(0);
        if (model && model->GetModel() && model->GetRenderBuffer().IsValid())
        {
            const auto& target = model->GetRenderBuffer();
            studio::items::render(target.GetTextureImage(), vkManager->GetImageFormat(), target.GetBufferWidth(), target.GetBufferHeight());
            studio::broadcast_frame(target.GetTextureImage(), vkManager->GetImageFormat(),
                                    target.GetBufferWidth(), target.GetBufferHeight());
        }
    }''', 'broadcast model texture')
    files[src / 'LAppView.cpp'] = view
    vm = files[src / 'VulkanManager.cpp']
    vm = rep(vm, '#include "VulkanManager.hpp"', '#include "VulkanManager.hpp"\n#include "studio_broadcast.hpp"\n#include "studio_items_vk.hpp"\n#include <vector>\n#include <cstdlib>\n#include <cstdio>\n#include <cstring>', 'broadcast device include')
    vm = rep(vm, '    return indices.IsComplete() && swapChainAdequate && supportedFeatures.samplerAnisotropy;', '    return indices.IsComplete() && swapChainAdequate && supportedFeatures.samplerAnisotropy && (!studio::broadcast_requested() || studio::broadcast_extensions(device));', 'broadcast device suitability')
    vm = rep(vm, '    createInfo.enabledExtensionCount = sizeof(deviceExtensions) / sizeof(deviceExtensions[0]);\n    createInfo.ppEnabledExtensionNames = deviceExtensions;', '''    std::vector<const char*> extensions(deviceExtensions, deviceExtensions + sizeof(deviceExtensions)/sizeof(deviceExtensions[0]));
    if (studio::broadcast_requested()) {
        const char* extras[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME };
        for (auto name : extras) extensions.push_back(name);
    }
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();''', 'broadcast extensions enable')
    vm = rep(vm, '        LAppPal::PrintLogLn("failed to find a suitable GPU!");', '        LAppPal::PrintLogLn("failed to find a suitable GPU! Check broadcast extensions or build with --with-broadcast.");\n        std::exit(EXIT_FAILURE);', 'fail cleanly if GPU unsuitable')
    vm = rep(vm, '        LAppPal::PrintLogLn("failed to create logical _device!");', '        LAppPal::PrintLogLn("failed to create logical _device!");\n        std::exit(EXIT_FAILURE);', 'fail cleanly if device creation fails')
    vm = rep(vm, '    CreateSyncObjects();', '    CreateSyncObjects();\n    studio::broadcast_init(_instance, _physicalDevice, _device, _graphicQueue, static_cast<uint32_t>(indices.graphicsFamily));\n    studio::items::init(_physicalDevice, _device, _graphicQueue, static_cast<uint32_t>(indices.graphicsFamily));', 'broadcast lifecycle start')
    vm = rep(vm, 'void VulkanManager::Destroy()\n{', 'void VulkanManager::Destroy()\n{\n    studio::items::shutdown();\n    studio::broadcast_shutdown();', 'broadcast lifecycle stop')
    files[src / 'VulkanManager.cpp'] = vm
    target_path = sdk / 'Framework/src/Rendering/Vulkan/CubismRenderTarget_Vulkan.cpp'
    target = target_path.read_text(encoding='utf-8-sig')
    target = rep(target, 'VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |\n                             VK_IMAGE_USAGE_TRANSFER_DST_BIT', 'VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |\n                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT', 'model target transfer source usage')
    files[target_path] = target
    cmake = files[src / 'CMakeLists.txt']
    cmake += '\ntarget_sources(${APP_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/studio_broadcast.cpp ${CMAKE_CURRENT_SOURCE_DIR}/studio_items_image.cpp ${CMAKE_CURRENT_SOURCE_DIR}/studio_items_vk.cpp)\nfind_package(PNG REQUIRED)\nfind_library(STUDIO_GIF_LIBRARY NAMES gif)\nif(NOT STUDIO_GIF_LIBRARY)\n    message(FATAL_ERROR "giflib is required")\nendif()\ntarget_link_libraries(${APP_NAME} PNG::PNG ${STUDIO_GIF_LIBRARY})\n'
    files[src / 'CMakeLists.txt'] = cmake
    parent = files[proj / 'CMakeLists.txt']
    parent += '''
# Optional native PipeWire output. Paths refer to the locally tested Meson build.
if(STUDIO_FUNNEL_BUILD)
    target_compile_definitions(${APP_NAME} PRIVATE STUDIO_WITH_FUNNEL=1)
    target_include_directories(${APP_NAME} PRIVATE "${STUDIO_FUNNEL_INCLUDE}")
    target_link_libraries(${APP_NAME} "${STUDIO_FUNNEL_BUILD}/libfunnel-vk.so" "${STUDIO_FUNNEL_BUILD}/libfunnel.so")
    set_property(TARGET ${APP_NAME} APPEND PROPERTY BUILD_RPATH "${STUDIO_FUNNEL_BUILD}")
endif()
'''
    files[proj / 'CMakeLists.txt'] = parent
    # P0.2b2: fix the model canvas to broadcast dimensions, independent of WSI.
    manager = files[src / 'LAppLive2DManager.cpp']
    manager = rep(manager, '#include "studio_controls.hpp"', '#include "studio_controls.hpp"\n#include "studio_render_size.hpp"', 'master manager include')
    manager = rep(manager, 'void LAppLive2DManager::OnUpdate() const\n{\n    int width, height;\n    glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &width, &height);',
        'void LAppLive2DManager::OnUpdate() const\n{\n    int width, height;\n    glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &width, &height);\n    studio::apply_render_size(width, height);\n    if (width <= 0 || height <= 0) return;', 'master projection dimensions')
    manager = rep(manager, '    for(csmUint32 i = 0; i < _models.GetSize(); i++)\n    {\n        LAppModel* model = GetModel(i);\n\n        model->SetRenderTargetSize(width, height);',
        '    const auto canvas = studio::render_size(width, height);\n    for(csmUint32 i = 0; i < _models.GetSize(); i++)\n    {\n        LAppModel* model = GetModel(i);\n\n        model->SetRenderTargetSize(canvas.width, canvas.height);', 'master internal resize')
    # Keep window-dependent view matrices out of the fixed output. The studio camera is applied separately.
    manager = rep(manager, '        if (_viewMatrix != NULL)', '        if (_viewMatrix != NULL && !studio::fixed_canvas())', 'master stable camera')
    files[src / 'LAppLive2DManager.cpp'] = manager
    model = files[src / 'LAppModel.cpp']
    model = rep(model, '#include "studio_controls.hpp"', '#include "studio_controls.hpp"\n#include "studio_render_size.hpp"', 'master model include')
    model = rep(model, '    CreateRenderer(LAppDelegate::GetInstance()->GetWindowWidth(), LAppDelegate::GetInstance()->GetWindowHeight());',
        '    const auto canvas = studio::render_size(LAppDelegate::GetInstance()->GetWindowWidth(), LAppDelegate::GetInstance()->GetWindowHeight());\n    CreateRenderer(canvas.width, canvas.height);', 'master renderer creation')
    model = rep(model, '    CreateRenderer(LAppDelegate::GetInstance()->GetWindowHeight(), LAppDelegate::GetInstance()->GetWindowHeight());',
        '    const auto canvas = studio::render_size(LAppDelegate::GetInstance()->GetWindowWidth(), LAppDelegate::GetInstance()->GetWindowHeight());\n    CreateRenderer(canvas.width, canvas.height);', 'master renderer reload')
    files[src / 'LAppModel.cpp'] = model
    view = files[src / 'LAppView.cpp']
    view = rep(view, '#include "studio_broadcast.hpp"', '#include "studio_broadcast.hpp"\n#include "studio_render_size.hpp"', 'master view include')
    view = rep(view, '            glfwGetFramebufferSize(LAppDelegate::GetInstance()->GetWindow(), &bufWidth, &bufHeight);',
        '            glfwGetFramebufferSize(LAppDelegate::GetInstance()->GetWindow(), &bufWidth, &bufHeight);\n            studio::apply_render_size(bufWidth, bufHeight);', 'master texture dimensions')
    view = rep(view, '                    _renderSprite->Render(commandBuffer, _modelSpritePipelineLayout, vkManager, width, height);',
        '                    if (studio::fixed_canvas()) {\n                        const auto canvas = studio::render_size(width, height);\n                        const auto rect = studio::preview_rect(width, height, canvas);\n                        _renderSprite->ResetRect(rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f, rect.w, rect.h);\n                    }\n                    _renderSprite->Render(commandBuffer, _modelSpritePipelineLayout, vkManager, width, height);', 'master aspect fit preview')
    view = rep(view, 'void LAppView::OnTouchesMoved(float px, float py) const\n{', '''void LAppView::OnTouchesMoved(float px, float py) const
{
    if (studio::fixed_canvas()) {
        _touchManager->TouchesMoved(px, py);
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager->GetModel(0);
        int w,h;
        glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &w,&h);
        float x=0.f,y=0.f;
        if (model && model->GetModel() && studio::camera_is_default() &&
            studio::preview_point(px,py,w,h,studio::render_size(w,h),model->GetModel()->GetCanvasWidth()>1.f,x,y))
            manager->OnDrag(x,y);
        else manager->OnDrag(0.f,0.f);
        return;
    }''', 'master preview drag')
    view = rep(view, '    live2DManager->OnDrag(0.0f, 0.0f);', '''    live2DManager->OnDrag(0.0f, 0.0f);
    if (studio::fixed_canvas()) {
        auto* model = live2DManager->GetModel(0);
        int w,h;
        glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &w,&h);
        float x=0.f,y=0.f;
        if (model && model->GetModel() && studio::camera_is_default() &&
            studio::preview_point(px,py,w,h,studio::render_size(w,h),model->GetModel()->GetCanvasWidth()>1.f,x,y))
            live2DManager->OnTap(x,y);
        return;
    }''', 'master preview tap')
    files[src / 'LAppView.cpp'] = view
    delegate = files[src / 'LAppDelegate.cpp']
    delegate = rep(delegate, '#include "studio_controls.hpp"', '#include "studio_controls.hpp"\n#include "studio_render_size.hpp"', 'master delegate include')
    # Initial allocation is already sized in LAppModel; no resize of model resources when WSI changes.
    delegate = rep(delegate, '        LAppLive2DManager::GetInstance()->SetRenderTargetSize(width, height);',
        '        if (!studio::fixed_canvas()) LAppLive2DManager::GetInstance()->SetRenderTargetSize(width, height);', 'master keep model resources on window resize')
    files[src / 'LAppDelegate.cpp'] = delegate
    # P0.2c: bypass WSI presentation when broadcasting without preview.
    view = files[src / 'LAppView.cpp']
    view = rep(view, '#include "studio_render_size.hpp"', '#include "studio_render_size.hpp"\n#include "studio_background.hpp"', 'background view include')
    view = guard_preview_submit(view)
    view = rep(view, '    if (_renderTarget == SelectTarget_ModelFrameBuffer && _renderSprite)', '    if (studio::background_frame()) return;\n    if (_renderTarget == SelectTarget_ModelFrameBuffer && _renderSprite)', 'background skip preview composite')
    view = rep(view, 'void LAppView::PostModelDraw(LAppModel &refModel)\n{', 'void LAppView::PostModelDraw(LAppModel &refModel)\n{\n    if (studio::background_frame()) return;', 'background avoid restoring WSI target')
    files[src / 'LAppView.cpp'] = view
    vm = files[src / 'VulkanManager.cpp']
    vm = rep(vm, 'void VulkanManager::UpdateDrawFrame()', 'bool VulkanManager::UpdateDrawFrame()', 'acquire result')
    vm = rep(vm, '    VkResult result = vkAcquireNextImageKHR(_device, _swapchainManager->GetSwapchain(), UINT64_MAX,', '    const uint64_t timeout = studio::broadcast_requested() ? 1000000ULL : UINT64_MAX;\n    VkResult result = vkAcquireNextImageKHR(_device, _swapchainManager->GetSwapchain(), timeout,', 'bounded WSI acquire')
    vm = rep(vm, '''    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _isSwapchainInvalid = true;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        LAppPal::PrintLogLn("failed to acquire swap chain image!");
    }''', '''    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        _isSwapchainInvalid = true;
        return false;
    }
    if (result == VK_TIMEOUT || result == VK_NOT_READY) return false;
    if (result == VK_SUBOPTIMAL_KHR) {
        // The semaphore was signalled. Consume it by drawing and presenting first.
        _isSwapchainInvalid = true;
        return true;
    }
    if (result != VK_SUCCESS) {
        std::fprintf(stderr,"[studio] swapchain acquire failed: %d\\n",int(result));
        std::exit(EXIT_FAILURE);
    }
    return true;''', 'acquire status handling')
    vm = rep(vm, '    _swapchainManager->Cleanup(_device);\n    _swapchainManager->CreateSwapchain', '    _swapchainManager->Cleanup(_device);\n    _imageIndex = 0;\n    _swapchainManager->CreateSwapchain', 'reset swapchain index')
    files[src / 'VulkanManager.cpp'] = vm
    files[src / 'VulkanManager.hpp'] = rep(read('VulkanManager.hpp'), '    void UpdateDrawFrame();', '    bool UpdateDrawFrame();', 'acquire declaration')
    delegate = files[src / 'LAppDelegate.cpp']
    delegate = rep(delegate, '#include "studio_render_size.hpp"', '#include "studio_render_size.hpp"\n#include "studio_background.hpp"\n#include "studio_broadcast.hpp"', 'background delegate include')
    delegate = rep(delegate, 'void LAppDelegate::Run()\n{', '''void LAppDelegate::Run()
{
    studio::install_stop_signals();
    const bool noPreview = studio::broadcast_requested() && studio::no_preview_requested();
    if (noPreview) {
        glfwHideWindow(_window);
        std::fprintf(stdout,"[studio] no-preview mode; Ctrl+C to stop\\n");
        std::fflush(stdout);
    }''', 'no preview startup')
    delegate = rep(delegate, '    while (glfwWindowShouldClose(_window) == GL_FALSE && !_isEnd)', '    while (glfwWindowShouldClose(_window) == GL_FALSE && !_isEnd && !studio::stop_requested())', 'graceful stop')
    delegate = rep(delegate, '        while (width == 0 || height == 0)', '        if (studio::broadcast_requested() && (width == 0 || height == 0)) return false;\n        while (width == 0 || height == 0)', 'no blocking zero-size wait while broadcasting')
    delegate = rep(delegate, '        _view->DestroyRenderTarget();', '        if (!studio::fixed_canvas()) _view->DestroyRenderTarget();', 'preserve master target on swapchain rebuild')
    delegate = rep(delegate, '        VulkanManager::GetInstance()->UpdateDrawFrame();\n\n        if (RecreateSwapchain())\n        {\n            continue;\n        }', '''        auto* vk = VulkanManager::GetInstance();
        if (studio::broadcast_requested()) {
            int fbw=0, fbh=0;
            glfwGetFramebufferSize(_window,&fbw,&fbh);
            bool preview=studio::can_preview(noPreview,
                glfwGetWindowAttrib(_window,GLFW_VISIBLE)==GLFW_TRUE,
                glfwGetWindowAttrib(_window,GLFW_ICONIFIED)==GLFW_TRUE,fbw,fbh);
            if (preview && vk->GetIsWindowSizeChanged()) {
                RecreateSwapchain();
                preview = !vk->GetIsWindowSizeChanged();
            }
            // A timeout/out-of-date acquire leaves no image to present.
            if (preview) preview=vk->UpdateDrawFrame();
            studio::set_background_frame(!preview);
            _view->Render();
            if (preview) vk->PostDraw();
            studio::pace_frame();
            continue;
        }
        // Non-broadcast viewer still waits for an ordinary window frame.
        if (vk->GetIsWindowSizeChanged()) RecreateSwapchain();
        if (!vk->UpdateDrawFrame()) { studio::pace_frame(); continue; }''', 'independent broadcast frame loop')
    files[src / 'LAppDelegate.cpp'] = delegate
    # P0.5a: native immediate-mode workspace, composited only on the WSI image.
    view = files[src / 'LAppView.cpp']
    view = rep(view, '#include "studio_background.hpp"', '#include "studio_background.hpp"\n#include "studio_ui.hpp"', 'workspace include')
    view = rep(view, '    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;', """    bool workspace = false;
    if (!studio::background_frame()) {
        auto* model = live2DManager->GetModelNum() > 0 ? live2DManager->GetModel(0) : nullptr;
        workspace = studio::ui_begin(LAppDelegate::GetInstance()->GetWindow(),
            vkManager->GetStudioInstance(), vkManager->GetPhysicalDevice(), vkManager->GetDevice(),
            vkManager->GetGraphicQueue(), vkManager->indices.graphicsFamily,
            vkManager->GetSwapchainManager()->GetImageCount(), vkManager->GetSwapchainManager()->GetSwapchainImageFormat(),
            vkManager->GetSwapchainManager()->GetExtent(),
            model && model->GetModel() ? model->GetModel()->GetParameterCount() : 0);
    }
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;""", 'workspace begin')
    view = rep(view, '    ChangeEndLayout(commandBuffer);', """    if (workspace) {
        // Order previous WSI color writes before LOAD and blending in the UI pass.
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        BeginRendering(commandBuffer, 0.f, 0.f, 0.f, 1.f, false);
        studio::ui_draw(commandBuffer);
        EndRendering(commandBuffer);
    }
    ChangeEndLayout(commandBuffer);""", 'workspace final composition')
    files[src / 'LAppView.cpp'] = view
    delegate = files[src / 'LAppDelegate.cpp']
    delegate = rep(delegate, '#include "studio_background.hpp"', '#include "studio_background.hpp"\n#include "studio_ui.hpp"\n#include "studio_ui_layout.hpp"', 'workspace lifecycle include')
    delegate = rep(delegate, 'void LAppDelegate::Release()\n{', 'void LAppDelegate::Release()\n{\n    studio::ui_shutdown();', 'workspace teardown before device')
    files[src / 'LAppDelegate.cpp'] = delegate
    h = files[src / 'VulkanManager.hpp']
    h = rep(h, '    bool UpdateDrawFrame();', '    bool UpdateDrawFrame();\n    VkInstance GetStudioInstance() const { return _instance; }', 'workspace instance accessor')
    files[src / 'VulkanManager.hpp'] = h
    cmake = files[src / 'CMakeLists.txt']
    cmake += """
# KAWARI_P05A: pinned upstream Dear ImGui, built with existing GLFW/Vulkan.
if(NOT EXISTS "${STUDIO_IMGUI_ROOT}/imgui.cpp")
    message(FATAL_ERROR "STUDIO_IMGUI_ROOT missing; run scripts/build.sh")
endif()
target_sources(${APP_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/studio_ui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/studio_app_icon.cpp
    "${STUDIO_IMGUI_ROOT}/imgui.cpp"
    "${STUDIO_IMGUI_ROOT}/imgui_draw.cpp"
    "${STUDIO_IMGUI_ROOT}/imgui_tables.cpp"
    "${STUDIO_IMGUI_ROOT}/imgui_widgets.cpp"
    "${STUDIO_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${STUDIO_IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp")
target_include_directories(${APP_NAME} PRIVATE "${STUDIO_IMGUI_ROOT}" "${STUDIO_IMGUI_ROOT}/backends")
"""
    files[src / 'CMakeLists.txt'] = cmake
    # P0.5a1: allocate larger masks from the start, before descriptor creation.
    renderer = files[renderer_path]
    renderer = rep(renderer, '#include "CubismRenderer_Vulkan.hpp"',
        '#include "CubismRenderer_Vulkan.hpp"\n#include <cstdlib>\n#include <cstring>\n#include <cstdio>', 'quality includes')
    renderer = rep(renderer, '    if (model->IsUsingMasking())', """    // Keep the same atlas layout; increase precision before GPU allocation.
    const char* requestedMask = std::getenv("STUDIO_MASK_SIZE");
    int studioMaskSize = 1024;
    if (requestedMask) {
        if (!std::strcmp(requestedMask, "256")) studioMaskSize = 256;
        else if (!std::strcmp(requestedMask, "512")) studioMaskSize = 512;
        else if (!std::strcmp(requestedMask, "2048")) studioMaskSize = 2048;
    }
    VkPhysicalDeviceProperties studioMaskProps{};
    vkGetPhysicalDeviceProperties(s_physicalDevice, &studioMaskProps);
    while (studioMaskSize > 256 && static_cast<unsigned>(studioMaskSize) > studioMaskProps.limits.maxImageDimension2D)
        studioMaskSize /= 2;
    std::fprintf(stdout, "[quality] clipping mask %dx%d; linear target sampling\\n", studioMaskSize, studioMaskSize);
    if (model->IsUsingMasking())""", 'quality mask size')
    for manager_name in ('_drawableClippingManager', '_offscreenClippingManager'):
        old = f'        {manager_name} = CSM_NEW CubismClippingManager_Vulkan();\n        {manager_name}->Initialize('
        new = (f'        {manager_name} = CSM_NEW CubismClippingManager_Vulkan();\n'
               f'        {manager_name}->SetClippingMaskBufferSize(studioMaskSize, studioMaskSize);\n'
               f'        {manager_name}->Initialize(')
        renderer = rep(renderer, old, new, 'quality ' + manager_name)
    renderer = patch_mask_compatibility(renderer)
    files[renderer_path] = renderer
    target = files[target_path]
    # Single-level color targets (mask + preview). Do not touch atlas textures or export blit.
    sampler = re.compile(r'(_colorImage->CreateSampler\(\s*device\s*,\s*VK_SAMPLER_ADDRESS_MODE_REPEAT\s*,\s*)VK_FILTER_NEAREST\s*,\s*VK_FILTER_NEAREST(\s*,\s*VK_SAMPLER_MIPMAP_MODE_NEAREST\s*,\s*)1\.0(\s*,\s*1\s*\);)')
    target, count = sampler.subn(r'\g<1>VK_FILTER_LINEAR, VK_FILTER_LINEAR\g<2>0.0\g<3>', target)
    if count != 1:
        raise ValueError('SDK layout mismatch: quality render target sampler (expected one anchor)')
    files[target_path] = target
    # The workspace handles the mouse itself, rather than driving the sample's gaze/taps.
    view = files[src / 'LAppView.cpp']
    for method in ('OnTouchesBegan', 'OnTouchesMoved', 'OnTouchesEnded'):
        opening = f'void LAppView::{method}(float px, float py) const\n{{'
        view = rep(view, opening, opening + '\n    if (studio::ui_requested()) return;', 'workspace mouse ' + method)
    files[src / 'LAppView.cpp'] = view
    # P0.5d0: experimental in-process replacement at a frame boundary, before WSI acquire.
    manager = files[src / 'LAppLive2DManager.cpp']
    manager = rep(manager, '#include "studio_controls.hpp"', '#include "studio_controls.hpp"\n#include "studio_ui.hpp"\n#include <cstdio>\n#include <memory>', 'hot model includes')
    manager += r'''
void LAppLive2DManager::StudioPollModelSwitch()
{
    studio::ModelSelection selection;
    if (!studio::take_model_selection(selection)) return;
    auto* vk = VulkanManager::GetInstance();
    if (_models.GetSize() != 1 || vkDeviceWaitIdle(vk->GetDevice()) != VK_SUCCESS)
    {
        std::puts("[model-switch] rejected: renderer not ready");std::fflush(stdout);return;
    }
    const auto slash = selection.runtime.find_last_of('/');
    const auto directory = selection.runtime.substr(0, slash + 1);
    const auto filename = selection.runtime.substr(slash + 1);
    auto* old = _models[0];
    std::unique_ptr<LAppModel> candidate(new LAppModel());
    // Keep the old model alive until the new assets have loaded. This uses
    // extra VRAM during the transition, but a rejected model leaves it intact.
    candidate->LoadAssets(vk->GetDevice(), vk->GetTextureFormat(), directory.c_str(), filename.c_str());
    // Do not feed model loading time into motion/physics on the next frame.
    LAppPal::UpdateTime();
    if (!candidate->StudioComplete())
    {
        vkDeviceWaitIdle(vk->GetDevice());
        candidate->StudioReleaseTexturesExcept(old);
        candidate.reset();
        std::puts("[model-switch] rejected: model or texture load failed");std::fflush(stdout);return;
    }
    const auto canvas = studio::render_size(LAppDelegate::GetInstance()->GetWindowWidth(), LAppDelegate::GetInstance()->GetWindowHeight());
    candidate->SetRenderTargetSize(canvas.width, canvas.height);
    vkDeviceWaitIdle(vk->GetDevice());
    studio::ui_model_changed();
    old->StudioReleaseTexturesExcept(candidate.get());
    delete old;
    _models[0] = candidate.release();
    studio::commit_model_selection(selection);
    std::puts("[model-switch] committed; window and PipeWire retained");std::fflush(stdout);
}
'''
    files[src / 'LAppLive2DManager.cpp'] = manager
    h = read('LAppLive2DManager.hpp')
    h = rep(h, '    void NextScene();', '    void NextScene();\n    void StudioPollModelSwitch();', 'hot model manager declaration')
    files[src / 'LAppLive2DManager.hpp'] = h
    model = files[src / 'LAppModel.cpp']
    model = rep(model, '        if (_model) _model->SetOverrideFlagForModelParameterRepeat(false);\n        DeleteBuffer(buffer, path.GetRawString());', '        if (_model) _model->SetOverrideFlagForModelParameterRepeat(false);\n        DeleteBuffer(buffer, path.GetRawString());\n        if (!_model) return;', 'reject invalid moc before setup')
    model += r'''
bool LAppModel::StudioComplete()
{
    if (!_model || !_modelSetting || !_initialized || _model->GetParameterCount() <= 0) return false;
    Csm::csmUint32 expected = 0;
    for (Csm::csmInt32 i = 0; i < _modelSetting->GetTextureCount(); ++i)
        if (strcmp(_modelSetting->GetTextureFileName(i), "") != 0) ++expected;
    return expected > 0 && _bindTextureId.GetSize() == expected;
}
void LAppModel::StudioReleaseTexturesExcept(LAppModel* keep)
{
    DeleteRenderer();
    auto* manager = LAppDelegate::GetInstance()->GetTextureManager();
    for (Csm::csmUint32 i = 0; i < _bindTextureId.GetSize(); ++i)
    {
        bool shared = false;
        if (keep) for (Csm::csmUint32 j = 0; j < keep->_bindTextureId.GetSize(); ++j)
            if (_bindTextureId[i] == keep->_bindTextureId[j]) { shared = true; break; }
        if (!shared) manager->ReleaseTexture(_bindTextureId[i]);
    }
    _bindTextureId.Clear();
}
'''
    files[src / 'LAppModel.cpp'] = model
    h = read('LAppModel.hpp')
    h = rep(h, '    virtual ~LAppModel();', '    virtual ~LAppModel();\n    bool StudioComplete();\n    void StudioReleaseTexturesExcept(LAppModel* keep);', 'hot model asset declaration')
    files[src / 'LAppModel.hpp'] = h
    tex = read('LAppTextureManager.cpp')
    tex = rep(tex, '        _sequenceId--;', '        // Keep IDs monotonic while other textures remain alive.', 'texture id reuse')
    tex = rep(tex, '        STBI_rgb_alpha);', '        STBI_rgb_alpha);\n    if (!png) { LAppPal::ReleaseBytes(address); return nullptr; }', 'reject failed png decode')
    files[src / 'LAppTextureManager.cpp'] = tex
    delegate = files[src / 'LAppDelegate.cpp']
    delegate = rep(delegate, '        studio::update_view(_window);\n        if (glfwWindowShouldClose(_window)) break;', '        studio::update_view(_window);\n        if (glfwWindowShouldClose(_window)) break;\n        LAppLive2DManager::GetInstance()->StudioPollModelSwitch();', 'hot model frame boundary')
    # Keep the texture manager alive until sprites and models have been destroyed.
    delegate = rep(delegate, '    delete _textureManager;\n    delete _view;', '    delete _view;', 'texture manager lifetime begin')
    delegate = rep(delegate, '    LAppLive2DManager::ReleaseInstance();', '    LAppLive2DManager::ReleaseInstance();\n    delete _textureManager;', 'texture manager lifetime end')
    files[src / 'LAppDelegate.cpp'] = delegate
    # P0.5e1: expose only the active solver rig and timestep setter in the
    # local staged SDK. No new public SDK dependency or source redistribution.
    physics_header = sdk / 'Framework/src/Physics/CubismPhysics.hpp'
    ph = physics_header.read_text()
    ph = rep(ph, '    const Options& GetOptions() const;', r'''    const Options& GetOptions() const;
    CubismPhysicsRig* StudioPhysicsRig() { return _physicsRig; }
    void StudioSetPhysicsFps(csmFloat32 fps) {
        if (_physicsRig && _physicsRig->Fps != fps) {
            _physicsRig->Fps = fps;
            _currentRemainTime = 0.0f;
        }
    }''', 'physics runtime access')
    files[physics_header] = ph
    h = files[src / 'LAppModel.hpp']
    h = rep(h, '#pragma once', '#pragma once\n#include "studio_physics.hpp"', 'physics editor include')
    h = rep(h, '    bool StudioComplete();', '    studio::physics::Editor _studioPhysicsEditor;\n    bool StudioComplete();', 'per-model physics editor')
    files[src / 'LAppModel.hpp'] = h
    model = files[src / 'LAppModel.cpp']
    model = rep(model, '            CubismPhysicsUpdater* physics = CSM_NEW CubismPhysicsUpdater(*_physics);', '            _studioPhysicsEditor.bind(_physics, buffer, size);\n            auto* physics = CSM_NEW studio::physics::Updater(*_physics, _studioPhysicsEditor);', 'physics editor updater')
    model = rep(model, '    _updateScheduler.OnLateUpdate(_model, deltaTimeSeconds);', '    _studioPhysicsEditor.activate();\n    _updateScheduler.OnLateUpdate(_model, deltaTimeSeconds);', 'activate current physics profile')
    files[src / 'LAppModel.cpp'] = model
    # P0.5e2: native motion catalog and model-scoped playback policy.
    h = files[src / 'LAppModel.hpp']
    h = rep(h, '#include "studio_physics.hpp"', '#include "studio_physics.hpp"\n#include "studio_animations.hpp"', 'animation editor include')
    h = rep(h, '    studio::physics::Editor _studioPhysicsEditor;', '    studio::physics::Editor _studioPhysicsEditor;\n    studio::animation::Editor _studioAnimationEditor;\n    std::vector<float> _studioBaseParameters, _studioBaseParts;\n    float _studioBaseOpacity = 1.f;', 'per-model animation editor')
    files[src / 'LAppModel.hpp'] = h
    model = files[src / 'LAppModel.cpp']
    model = rep(model, '    _model->SaveParameters();\n\n    for (csmInt32 i = 0;', r'''    _model->SaveParameters();
    _studioBaseParameters.clear(); _studioBaseParts.clear();
    for (csmInt32 i=0;i<_model->GetParameterCount();++i) _studioBaseParameters.push_back(_model->GetParameterValue(i));
    for (csmInt32 i=0;i<_model->GetPartCount();++i) _studioBaseParts.push_back(_model->GetPartOpacity(i));
    _studioBaseOpacity = _model->GetModelOpacity();

    for (csmInt32 i = 0;''', 'animation base pose')
    model = rep(model, '            _motions[name] = tmpMotion;', r'''            _motions[name] = tmpMotion;
            _studioAnimationEditor.rows.emplace_back(group, i, _modelSetting->GetMotionFileName(group,i),
                tmpMotion->GetLoopDuration(), tmpMotion->GetDuration()<0.f,
                strcmp(_modelSetting->GetMotionSoundFileName(group,i), "")!=0);''', 'animation catalog')
    start = '    if (_motionManager->IsFinished())'
    end = '    _model->SaveParameters();'
    # Unique block in Update, with exact SDK R5 anchors.
    update = model.index('void LAppModel::Update()')
    a = model.index(start, update); b = model.index(end, a)
    old = model[a:b]
    if old.count('StartRandomMotion(MotionGroupIdle, PriorityIdle);') != 1 or old.count('_motionManager->UpdateMotion(_model, deltaTimeSeconds)') != 1:
        raise ValueError('SDK layout mismatch: motion update')
    model = model[:a] + r'''    _studioAnimationEditor.activate();
    const auto plan = _studioAnimationEditor.next(_motionManager->IsFinished());
    if (plan.restart)
    {
        _motionManager->StopAllMotions();
        // Restore the pre-motion base pose before returning to Idle or starting a new preview.
        for (size_t i=0;i<_studioBaseParameters.size();++i) _model->SetParameterValue(static_cast<csmInt32>(i),_studioBaseParameters[i]);
        for (size_t i=0;i<_studioBaseParts.size();++i) _model->SetPartOpacity(static_cast<csmInt32>(i),_studioBaseParts[i]);
        _model->SetModelOpacity(_studioBaseOpacity);
    }
    if (plan.motion >= 0)
    {
        const auto& row = _studioAnimationEditor.rows[static_cast<size_t>(plan.motion)];
        const auto handle = StartMotion(row.group.c_str(),row.number,PriorityForce);
        _studioAnimationEditor.started(plan.motion,handle!=InvalidMotionQueueEntryHandleValue);
    }
    else if (plan.motion == studio::animation::model_idle)
    {
        // Same random selection as the SDK sample; record which motion is running.
        const csmInt32 count = _modelSetting->GetMotionCount(MotionGroupIdle);
        if (count > 0)
        {
            const csmInt32 index = rand() % count;
            const auto handle = StartMotion(MotionGroupIdle,index,plan.restart?PriorityForce:PriorityIdle);
            _studioAnimationEditor.started_default(MotionGroupIdle,index,handle!=InvalidMotionQueueEntryHandleValue);
        }
        else _studioAnimationEditor.started(plan.motion,false);
    }
    else if (plan.motion == studio::animation::no_idle) _studioAnimationEditor.started(plan.motion,true);
    if (plan.motion == studio::animation::keep && !_motionManager->IsFinished())
        _motionUpdated = _motionManager->UpdateMotion(_model, deltaTimeSeconds * _studioAnimationEditor.speed());
''' + model[b:]
    files[src / 'LAppModel.cpp'] = model
    # Validate all anchors before writing any files.
    for p, contents in files.items():
        p.write_text(contents, encoding='utf-8')
    for name in ('studio_controls.hpp', 'studio_controls.cpp', 'studio_broadcast.hpp', 'studio_broadcast.cpp', 'studio_render_size.hpp', 'studio_background.hpp', 'studio_parameters.hpp', 'studio_parameter_socket.hpp', 'studio_ui_layout.hpp', 'studio_ui.hpp', 'studio_ui.cpp', 'studio_ui_theme.hpp', 'studio_ui_preferences.hpp', 'studio_physics_settings.hpp', 'studio_physics.hpp', 'studio_animations.hpp', 'studio_items.hpp', 'studio_items_image.cpp', 'studio_items_vk.hpp', 'studio_items_vk.cpp', 'studio_app_icon.hpp', 'studio_app_icon.cpp'):
        shutil.copyfile(native / name, src / name)
    print('[studio] SDK hooks applied')

if __name__ == '__main__':
    try:
        patch(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, IndexError) as e:
        print(f'[studio] {e}', file=sys.stderr)
        sys.exit(1)
