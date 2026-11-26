import json
import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from check_model import validate
from patch_sdk import patch, replace_once

class ModelTest(unittest.TestCase):
    def test_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            for file in ('body.moc3', 'tex.png', 'physics.json'):
                (p/file).touch()
            manifest = p/'custom.model3.json'
            manifest.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'body.moc3','Textures':['tex.png'],'Physics':'physics.json'}}))
            self.assertEqual(len(validate(manifest)),3)
            (p/'tex.png').unlink()
            with self.assertRaisesRegex(ValueError,'Missing Texture'):
                validate(manifest)
    def test_non_relative(self):
        with tempfile.TemporaryDirectory() as d:
            manifest = Path(d)/'x.model3.json'
            manifest.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'/tmp/x','Textures':['tex.png']}}))
            with self.assertRaisesRegex(ValueError,'relative path'):
                validate(manifest)
    def test_anchor(self):
        self.assertEqual(replace_once('a b','a','x','test'),'x b')
        for text in ('b','a a'):
            with self.assertRaises(ValueError): replace_once(text,'a','x','test')
    def test_patch_fixture(self):
        # Synthetic SDK layout tests the patch machinery, not compilation against Live2D.
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); proj=root/'Samples/Vulkan/Demo/proj.linux.cmake'; src=proj/'src'; src.mkdir(parents=True)
            (proj/'CMakeLists.txt').write_text('project(Demo)\n')
            (src/'CMakeLists.txt').write_text('add_executable(Demo)\n')
            (src/'LAppLive2DManager.cpp').write_text('#include "LAppLive2DManager.hpp"\nvoid LAppLive2DManager::SetUpModel()\n{\n}\nmodelJsonName += ".model3.json";\n    ReleaseAllModel();\n    _models.PushBack(new LAppModel());\n        LAppDelegate::GetInstance()->GetView()->PreModelDraw(*model);\n    _models[0]->LoadAssets(\n        LAppDelegate::GetInstance()->GetView()->SwitchRenderingTarget(useRenderTarget);\n')
            (src/'LAppModel.cpp').write_text('#include "LAppModel.hpp"\nvoid LAppModel::SetupModel(ICubismModelSetting* setting)\n{\n        LoadModel(buffer, size);\n        DeleteBuffer(buffer, path.GetRawString());\n    _updateScheduler.SortUpdatableList();\n}\n            tmpMotion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);\n    _model->Update();\n}\n')
            (src/'LAppDelegate.cpp').write_text('#include "LAppDelegate.hpp"\n    _window = glfwCreateWindow(RenderTargetWidth, RenderTargetHeight, "SAMPLE"\n    glfwMakeContextCurrent(_window);\n    glfwSwapInterval(1);\n        glfwPollEvents();\n        VulkanManager::GetInstance()->PostDraw();\n        RecreateSwapchain();\n')
            (src/'LAppView.cpp').write_text('#include "LAppView.hpp"\n    BeginRendering(commandBuffer, 0.0, 0.0, 0.0, 1.0, true);\n    if(_spritePipeline != VK_NULL_HANDLE && _spritePipelineLayout != VK_NULL_HANDLE)\n        live2DManager->OnTap(x, y);\n                LAppModel* model = live2DManager->GetModel(i);\n                commandBuffer\n')
            (src/'VulkanManager.cpp').write_text("""#include "VulkanManager.hpp"
    return indices.IsComplete() && swapChainAdequate && supportedFeatures.samplerAnisotropy;
    createInfo.enabledExtensionCount = sizeof(deviceExtensions) / sizeof(deviceExtensions[0]);
    createInfo.ppEnabledExtensionNames = deviceExtensions;
        LAppPal::PrintLogLn("failed to find a suitable GPU!");
        LAppPal::PrintLogLn("failed to create logical _device!");
    CreateSyncObjects();
void VulkanManager::Destroy()
{
    if (_enableValidationLayers)
    {
        createInfo.enabledLayerCount = sizeof(validationLayers) / sizeof(validationLayers[0]);
        createInfo.ppEnabledLayerNames = validationLayers;
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }""")
            renderer = root/'Framework/src/Rendering/Vulkan/CubismRenderer_Vulkan.cpp'
            renderer.parent.mkdir(parents=True)
            renderer.write_text('''#include "CubismRenderer_Vulkan.hpp"
    VkRenderingAttachmentInfoKHR depthStencilAttachment{};
    CubismRenderer::Initialize(model, maskBufferCount);
    SetupLayoutBounds(usingClipCount);
        _tmpBoundsOnModel.SetRect(allClippedDrawRect);
            _offscreenClippingManager->SetupClippingContext(*GetModel(), drawCommandBuffer, updateCommandBuffer, this, _commandBufferCurrent, DrawableObjectType_Offscreen);
    if (model->IsUsingMasking())
        _drawableClippingManager = CSM_NEW CubismClippingManager_Vulkan();
        _drawableClippingManager->Initialize(
        _offscreenClippingManager = CSM_NEW CubismClippingManager_Vulkan();
        _offscreenClippingManager->Initialize(
''')
            target = renderer.parent/'CubismRenderTarget_Vulkan.cpp'
            target.write_text('VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |\n                             VK_IMAGE_USAGE_TRANSFER_DST_BIT\n    _colorImage->CreateSampler(device, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, 1.0, 1);')
            # Additional R5 anchors for independent canvas sizing.
            extra = {
                'LAppLive2DManager.cpp': """void LAppLive2DManager::OnUpdate() const
{
    int width, height;
    glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &width, &height);
        if (_viewMatrix != NULL)
}
    for(csmUint32 i = 0; i < _models.GetSize(); i++)
    {
        LAppModel* model = GetModel(i);

        model->SetRenderTargetSize(width, height);
""",
                'LAppModel.cpp': """    CreateRenderer(LAppDelegate::GetInstance()->GetWindowWidth(), LAppDelegate::GetInstance()->GetWindowHeight());
    CreateRenderer(LAppDelegate::GetInstance()->GetWindowHeight(), LAppDelegate::GetInstance()->GetWindowHeight());
""",
                'LAppView.cpp': """            glfwGetFramebufferSize(LAppDelegate::GetInstance()->GetWindow(), &bufWidth, &bufHeight);
                    _renderSprite->Render(commandBuffer, _modelSpritePipelineLayout, vkManager, width, height);
void LAppView::OnTouchesBegan(float px, float py) const
{
}
void LAppView::OnTouchesEnded(float px, float py) const
{
}
void LAppView::OnTouchesMoved(float px, float py) const
{
    live2DManager->OnDrag(0.0f, 0.0f);
""",
                'LAppDelegate.cpp': """    LAppLive2DManager::GetInstance()->SetRenderTargetSize(width, height);
        LAppLive2DManager::GetInstance()->SetRenderTargetSize(width, height);
""",
            }
            for name, text in extra.items():
                with (src/name).open('a') as fp:
                    fp.write('\n'+text)
            background_anchors = {
                'LAppView.cpp': """    VkCommandBuffer commandBuffer = vkManager->BeginSingleTimeCommands();
    ChangeBeginLayout(commandBuffer);
    vkManager->SubmitCommand(commandBuffer, true);

    // SDK variant: comment and spacing between statements.
    live2DManager->SetViewMatrix(_viewMatrix);
    live2DManager->OnUpdate();
    ChangeEndLayout(commandBuffer);
    if (_renderTarget == SelectTarget_ModelFrameBuffer && _renderSprite)
void LAppView::PostModelDraw(LAppModel &refModel)
{
""",
                'LAppDelegate.cpp': """void LAppDelegate::Release()
{
    delete _textureManager;
    delete _view;

    // リソースを解放
    LAppLive2DManager::ReleaseInstance();
}
void LAppDelegate::Run()
{
    while (glfwWindowShouldClose(_window) == GL_FALSE && !_isEnd)
        while (width == 0 || height == 0)
        _view->DestroyRenderTarget();
        VulkanManager::GetInstance()->UpdateDrawFrame();

        if (RecreateSwapchain())
        {
            continue;
        }
""",
                'VulkanManager.hpp': '    void UpdateDrawFrame();',
                'VulkanManager.cpp': """void VulkanManager::UpdateDrawFrame()
    VkResult result = vkAcquireNextImageKHR(_device, _swapchainManager->GetSwapchain(), UINT64_MAX,
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _isSwapchainInvalid = true;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        LAppPal::PrintLogLn("failed to acquire swap chain image!");
    }
    _swapchainManager->Cleanup(_device);
    _swapchainManager->CreateSwapchain
""",
            }
            for name,text in background_anchors.items():
                with (src/name).open('a') as fp: fp.write('\n'+text)
            (src/'LAppLive2DManager.hpp').write_text('    void NextScene();')
            (src/'LAppModel.hpp').write_text('#pragma once\n    virtual ~LAppModel();')
            ph=root/'Framework/src/Physics/CubismPhysics.hpp';ph.parent.mkdir(parents=True)
            ph.write_text('    const Options& GetOptions() const;')
            with (src/'LAppModel.cpp').open('a') as fp:
                fp.write('\n            CubismPhysicsUpdater* physics = CSM_NEW CubismPhysicsUpdater(*_physics);\n    _updateScheduler.OnLateUpdate(_model, deltaTimeSeconds);\n')
            with (src/'LAppModel.cpp').open('a') as fp:
                fp.write("""
    _model->SaveParameters();

    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); ++i) {}
            _motions[name] = tmpMotion;
void LAppModel::Update()
{
    _model->LoadParameters();
    if (_motionManager->IsFinished())
    {
        StartRandomMotion(MotionGroupIdle, PriorityIdle);
    }
    else
    {
        _motionUpdated = _motionManager->UpdateMotion(_model, deltaTimeSeconds);
    }
    _model->SaveParameters();
}
""")
            (src/'LAppTextureManager.cpp').write_text('        _sequenceId--;\n        STBI_rgb_alpha);')
            patch(root, Path(__file__).resolve().parents[1]/'native')
            v=(src/'LAppView.cpp').read_text(); d=(src/'LAppDelegate.cpp').read_text(); vk=(src/'VulkanManager.cpp').read_text()
            self.assertIn('if (!studio::background_frame())',v)
            self.assertIn('studio::ui_begin',v)
            self.assertEqual(v.count('if (studio::ui_requested()) return;'),3)
            self.assertEqual(renderer.read_text().count('->SetClippingMaskBufferSize(studioMaskSize, studioMaskSize);'),2)
            self.assertIn('UseHighPrecisionMask(studioPolicy.high)',renderer.read_text())
            self.assertIn('SetupLayoutBounds(static_cast<csmInt32>(_clippingContextListForMask.GetSize()))',renderer.read_text())
            self.assertIn('VK_FILTER_LINEAR, VK_FILTER_LINEAR',target.read_text())
            self.assertIn('clipping mask %dx%d; linear target sampling\\n',renderer.read_text())
            self.assertIn('studio::ui_draw(commandBuffer)',v)
            self.assertIn('studio::ui_shutdown()',d)
            self.assertIn('StudioPollModelSwitch();',d)
            self.assertIn('studio::commit_model_selection(selection)',(src/'LAppLive2DManager.cpp').read_text())
            self.assertNotIn('_sequenceId--;',(src/'LAppTextureManager.cpp').read_text())
            self.assertTrue((src/'studio_ui.cpp').is_file())
            self.assertTrue((src/'studio_physics.hpp').is_file())
            self.assertTrue((src/'studio_physics_settings.hpp').is_file())
            self.assertTrue((src/'studio_animations.hpp').is_file())
            self.assertIn('_studioAnimationEditor.next(_motionManager->IsFinished())',(src/'LAppModel.cpp').read_text())
            self.assertIn('deltaTimeSeconds * _studioAnimationEditor.speed()',(src/'LAppModel.cpp').read_text())
            self.assertIn('_studioAnimationEditor.rows.emplace_back',(src/'LAppModel.cpp').read_text())
            self.assertIn('StudioSetPhysicsFps',ph.read_text())
            patched_model=(src/'LAppModel.cpp').read_text()
            self.assertIn('_studioPhysicsEditor.bind(_physics, buffer, size)',patched_model)
            self.assertIn('_studioPhysicsEditor.activate();\n    _updateScheduler.OnLateUpdate',patched_model)
            self.assertIn('if (studio::background_frame()) return;',v)
            self.assertIn('if (preview) vk->PostDraw();',d)
            self.assertIn('if (preview) preview=vk->UpdateDrawFrame();',d)
            self.assertIn('VK_TIMEOUT || result == VK_NOT_READY',vk)
            self.assertIn('bool UpdateDrawFrame();',(src/'VulkanManager.hpp').read_text())
            self.assertIn('if (!studio::fixed_canvas()) _view->DestroyRenderTarget();',d)
            self.assertIn('failed: %d\\n",int(result)',vk)
            self.assertIn('Ctrl+C to stop\\n");',d)
            self.assertIn('studio::apply_render_size(bufWidth, bufHeight)',(src/'LAppView.cpp').read_text())
            self.assertIn('studio::preview_rect(width, height, canvas)',(src/'LAppView.cpp').read_text())
            self.assertIn('studio::apply_render_size(width, height)',(src/'LAppLive2DManager.cpp').read_text())
            self.assertIn('if (!studio::fixed_canvas()) LAppLive2DManager::GetInstance()->SetRenderTargetSize', (src/'LAppDelegate.cpp').read_text())
            self.assertEqual((src/'LAppModel.cpp').read_text().count('CreateRenderer(canvas.width, canvas.height)'),2)
            self.assertTrue((src/'studio_render_size.hpp').is_file())
            self.assertIn('SetOverrideFlagForModelParameterRepeat(false)', (src/'LAppModel.cpp').read_text())
            self.assertIn('tmpMotion->SetLoop(true)', (src/'LAppModel.cpp').read_text())
            self.assertIn('tmpMotion->SetLoopFadeIn(false)', (src/'LAppModel.cpp').read_text())
            self.assertIn('CubismUpdateOrder_Physics - 1', (src/'LAppModel.cpp').read_text())
            self.assertIn('AddUpdatableList(CSM_NEW StudioParameterUpdater())', (src/'LAppModel.cpp').read_text())
            self.assertTrue((src/'studio_parameters.hpp').is_file())
            self.assertIn('VK_IMAGE_USAGE_TRANSFER_SRC_BIT', target.read_text())
            self.assertIn('studio::broadcast_frame', (src/'LAppView.cpp').read_text())
            self.assertIn('studio::broadcast_init', (src/'VulkanManager.cpp').read_text())
            self.assertIn('studio::broadcast_shutdown', (src/'VulkanManager.cpp').read_text())
            self.assertTrue((src/'studio_broadcast.cpp').is_file())
            self.assertIn('STUDIO_WITH_FUNNEL', (proj/'CMakeLists.txt').read_text())
            self.assertIn('depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL', renderer.read_text())
            self.assertIn('createInfo.ppEnabledLayerNames = nullptr;', (src/'VulkanManager.cpp').read_text())
            self.assertIn('STUDIO_MODEL',(src/'LAppLive2DManager.cpp').read_text())
            self.assertIn('studio::update_controls',(src/'LAppModel.cpp').read_text())
            self.assertTrue((src/'studio_controls.cpp').is_file())
            self.assertIn('studio::offscreen()', (src/'LAppLive2DManager.cpp').read_text())
            self.assertIn('studio::background()', (src/'LAppView.cpp').read_text())
            self.assertIn('studio::update_view', (src/'LAppDelegate.cpp').read_text())
            with self.assertRaisesRegex(ValueError,'already patched'): patch(root, Path(__file__).resolve().parents[1]/'native')
if __name__=='__main__': unittest.main()
