import pathlib
import subprocess
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'native/studio_broadcast.cpp').read_text()

class BroadcastQuality(unittest.TestCase):
    def test_actual_aspect_fit_cpp(self):
        # Compile the actual geometry routine without requiring Vulkan headers.
        a=SOURCE.index('struct Fit {'); b=SOURCE.index('void stats()',a)
        program='#include <algorithm>\n#include <cstdint>\n#include <cassert>\n'+SOURCE[a:b]+'''
int main() {
    auto a=aspect_fit(1920,1080,1920,1080); assert(a.w==1920 && a.h==1080 && a.x==0 && a.y==0);
    auto b=aspect_fit(800,600,1920,1080); assert(b.w==1440 && b.h==1080 && b.x==240 && b.y==0);
    auto c=aspect_fit(1080,1920,1920,1080); assert(c.w==607 && c.h==1080 && c.x==656 && c.y==0);
    auto d=aspect_fit(3840,2160,1920,1080); assert(d.w==1920 && d.h==1080);
    auto e=aspect_fit(1920,1080,1080,1920); assert(e.w==1080 && e.h==607 && e.y==656);
    auto f=aspect_fit(320,7680,7680,320); assert(f.w>=1 && f.h==320);
    auto g=aspect_fit(7680,320,320,7680); assert(g.w==320 && g.h>=1);
}
'''
        with tempfile.TemporaryDirectory() as t:
            p=pathlib.Path(t); (p/'test.cpp').write_text(program)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_no_srgb_blit(self):
        calls=[line for line in SOURCE.splitlines() if 'vkCmdBlitImage(' in line]
        self.assertEqual(len(calls),1)
        self.assertIn('resize_image',calls[0]); self.assertNotIn(',dest,',calls[0])
        self.assertIn('vkCmdCopyImage(cmd,copy_source',SOURCE)
        self.assertIn('resize_filter = VK_FILTER_LINEAR',SOURCE)
    def test_fixed_size_scratch(self):
        self.assertIn('ensure_resize_image(dw,dh,dst_unorm)',SOURCE)
        self.assertIn('resize_w==w && resize_h==h && resize_format==format',SOURCE)
        self.assertIn('destroy_resize_image();',SOURCE[SOURCE.index('void broadcast_shutdown()'):])
    def test_diagnostics(self):
        self.assertIn('source %ux%u -> content %ux%u -> stream %ux%u',SOURCE)
        self.assertIn('UPSCALING:',SOURCE)
        self.assertIn('exact byte copy',SOURCE)
if __name__=='__main__': unittest.main()
