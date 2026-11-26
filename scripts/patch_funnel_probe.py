#!/usr/bin/env python3
"""Patch only the upstream Vulkan demo, not the library or instance validation."""
import re
import sys
from pathlib import Path

MARK = '/* STUDIO_P02A1_DEVICE_LAYERS */'
READY = '/* STUDIO_P02A1_STREAM_READY */'

def patch(text):
    # Restrict the operation to the VkDeviceCreateInfo block, not the instance.
    start = 'VkDeviceCreateInfo createInfo = {0};'
    end = 'CHECK_VK_RESULT(vkCreateDevice(physDevice, &createInfo, NULL, &device));'
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('Unsupported demo layout: device creation anchors')
    a, b = text.index(start), text.index(end)
    if b <= a:
        raise ValueError('Invalid device creation anchors')
    block = text[a:b]
    if MARK not in block:
        # Drop enumeration of legacy device layers and its conditional assignment.
        pattern = r'uint32_t deviceLayerCount;\s*CHECK_VK_RESULT\(vkEnumerateDeviceLayerProperties\(.*?free\(layerProperties\);\s*if \(foundLayers >= sizeof\(layerNames\) / sizeof\(const char \*\)\) \{\s*createInfo\.enabledLayerCount\s*=\s*sizeof\(layerNames\) / sizeof\(const char \*\);\s*createInfo\.ppEnabledLayerNames = layerNames;\s*\}'
        replacement = MARK + '\n        createInfo.enabledLayerCount = 0;\n        createInfo.ppEnabledLayerNames = NULL;'
        block, n = re.subn(pattern, replacement, block, flags=re.S)
        if n != 1:
            # Newer upstream may already omit legacy layers; zero-initialized structure is sufficient.
            if 'enabledLayerCount' in block or 'vkEnumerateDeviceLayerProperties' in block:
                raise ValueError('Unsupported legacy device-layer block; inspect demo/test-vk.c')
            block += replacement + '\n        '
        text = text[:a] + block + text[b:]
    if READY not in text:
        pattern = r'(ret = funnel_stream_start\(stream\);\s*assert\(ret == 0\);)'
        # Use function replacement to avoid re.sub interpreting the C string's escape.
        text, n = re.subn(pattern, lambda m: m[0] + '\n    ' + READY + '\n    fprintf(stderr, "[studio-probe] Test Stream start returned success; verify PipeWire node and OBS frames.\\n");', text)
        if n != 1:
            raise ValueError('Unsupported demo layout: stream start')
    return text

if __name__ == '__main__':
    try:
        p = Path(sys.argv[1])
        old = p.read_text()
        new = patch(old)
        if old != new:
            p.write_text(new)
        print('[studio-probe] Demo patch ready (instance validation preserved)')
    except (OSError, ValueError, IndexError) as e:
        print(f'[studio-probe] {e}', file=sys.stderr)
        sys.exit(1)
