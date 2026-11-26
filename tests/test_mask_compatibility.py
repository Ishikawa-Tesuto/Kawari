import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patch_sdk import MASK_POLICY_CPP, patch_mask_compatibility

class MaskCompatibility(unittest.TestCase):
    def test_policy_compiled(self):
        if not shutil.which('g++'):
            self.skipTest('g++ unavailable')
        source = MASK_POLICY_CPP + r'''
#include <cassert>
int main() {
    using namespace studio_mask;
    int a[]={0,1}, b[]={1,0}, c[]={1,2}, d[]={3};
    int counts[]={2,2,2,1,0};
    const int* masks[]={a,b,c,d,nullptr};
    auto groups=collect(5,counts,masks);
    assert(groups.size()==3);
    assert(reused_sources(groups,{})==1);
    assert(reused_sources({{0,1}},{{1,2}})==1);
    assert(reused_sources({{0,1}},{{2,3}})==0);
    assert(choose(36,0,0,1,nullptr).buffers==1);
    assert(choose(37,0,0,1,nullptr).buffers==2);
    assert(choose(64,0,0,1,nullptr).buffers==2);
    assert(choose(1,65,0,1,nullptr).buffers==3);
    assert(!choose(32,0,1,1,nullptr).high);
    assert(choose(44,0,1,1,nullptr).buffers==2);
    assert(!choose(44,0,1,1,nullptr).high);
    std::set<int> used{0,1};
    assert(needs_split(used,c,2));
    assert(!needs_split(used,d,1));
    assert(choose(257,0,0,1,nullptr).high);
    assert(choose(5,0,0,1,"high").high);
    assert(!choose(50,0,1,1,"legacy").high);
    assert(choose(50,0,1,1,"legacy").buffers==1);
    assert(choose(0,0,0,1,nullptr).buffers==1);
    assert(collect<int>(0,nullptr,nullptr).empty());
}
'''
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'policy.cpp'; binary=Path(d)/'policy'
            path.write_text(source)
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_patch_rejects_unknown_layout(self):
        with self.assertRaises(ValueError): patch_mask_compatibility('')
    def test_hooks_and_logs(self):
        source='''#include "CubismRenderer_Vulkan.hpp"
    CubismRenderer::Initialize(model, maskBufferCount); // original
    SetupLayoutBounds(usingClipCount);
        _tmpBoundsOnModel.SetRect(allClippedDrawRect);
            _offscreenClippingManager->SetupClippingContext(*GetModel(), drawCommandBuffer, updateCommandBuffer, this, _commandBufferCurrent, DrawableObjectType_Offscreen);
'''
        patched=patch_mask_compatibility(source)
        self.assertIn('maskBufferCount = studioPolicy.buffers;',patched)
        self.assertIn('UseHighPrecisionMask(studioPolicy.high);',patched)
        self.assertIn('shared_sources=%d',patched)
        self.assertIn('SetupLayoutBounds(static_cast<csmInt32>(_clippingContextListForMask.GetSize()));',patched)
        self.assertIn('allClippedDrawRect->Width <= 0.0f',patched)
        self.assertLess(patched.index('maskBufferCount = studioPolicy.buffers;'),patched.index('CubismRenderer::Initialize(model, maskBufferCount);'))
        with self.assertRaises(ValueError): patch_mask_compatibility(patched)

if __name__=='__main__': unittest.main()
