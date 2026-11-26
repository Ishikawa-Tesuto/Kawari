import contextlib
import io
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patch_funnel_probe import patch, MARK, READY
from check_pipewire_video import report

# Minimal fixture with the upstream device block's relevant statements.
SOURCE = '''VkInstanceCreateInfo createInfo = {0};
createInfo.enabledLayerCount = 1;
VkDeviceCreateInfo createInfo = {0};
createInfo.ppEnabledExtensionNames = deviceExtensionNames;
uint32_t deviceLayerCount;
CHECK_VK_RESULT(vkEnumerateDeviceLayerProperties(
    physDevice, &deviceLayerCount, NULL));
VkLayerProperties *layerProperties = malloc(deviceLayerCount * sizeof(VkLayerProperties));
CHECK_VK_RESULT(vkEnumerateDeviceLayerProperties(physDevice, &deviceLayerCount, layerProperties));
size_t foundLayers = 0;
for (uint32_t i = 0; i < deviceLayerCount; i++) { foundLayers++; }
free(layerProperties);
if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
    createInfo.enabledLayerCount =
        sizeof(layerNames) / sizeof(const char *);
    createInfo.ppEnabledLayerNames = layerNames;
}
CHECK_VK_RESULT(vkCreateDevice(physDevice, &createInfo, NULL, &device));
ret = funnel_stream_start(stream);
assert(ret == 0);
'''
class TestProbe(unittest.TestCase):
    def test_patch(self):
        result = patch(SOURCE)
        self.assertIn('VkInstanceCreateInfo createInfo = {0};\ncreateInfo.enabledLayerCount = 1;', result)
        self.assertNotIn('vkEnumerateDeviceLayerProperties', result)
        self.assertIn('createInfo.enabledLayerCount = 0;', result)
        self.assertIn(MARK, result)
        self.assertIn(READY, result)
        self.assertIn('frames.\\n");', result)
    def test_idempotent(self):
        self.assertEqual(patch(patch(SOURCE)), patch(SOURCE))
    def test_mismatch(self):
        with self.assertRaises(ValueError): patch('wrong source')
    def test_node_and_link(self):
        nodes = [dict(id=42,type='PipeWire:Interface:Node',info={'props':{'node.name':'Test Stream','media.class':'Stream/Output/Video'},'state':'running'}),dict(id=5,type='PipeWire:Interface:Node',info={'props':{'node.name':'OBS','media.class':'Stream/Input/Video'}}),dict(id=45,type='PipeWire:Interface:Link',info={'output-node-id':42,'input-node-id':5,'state':'active'})]
        out=io.StringIO()
        with contextlib.redirect_stdout(out): self.assertEqual(report(nodes),0)
        self.assertIn('Test Stream -> OBS (active)',out.getvalue())
    def test_absent(self):
        with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(report([]),1)
if __name__ == '__main__': unittest.main()
