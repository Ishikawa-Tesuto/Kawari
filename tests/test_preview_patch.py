import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patch_sdk import guard_preview_submit

BEGIN = '''    VkCommandBuffer commandBuffer = vkManager->BeginSingleTimeCommands();
    ChangeBeginLayout(commandBuffer);
    draw_preview(commandBuffer);
'''
SUBMIT = '    vkManager->SubmitCommand(commandBuffer, true);\n'
UPDATE = '    live2DManager->OnUpdate();\n    studio::broadcast_frame();\n'

class PreviewPatch(unittest.TestCase):
    def test_sdk_variants(self):
        for middle in ('    live2DManager->SetViewMatrix(_viewMatrix);\n',
                       '\n    // comment\n    live2DManager->SetViewMatrix(_viewMatrix);\n',
                       '\n', '\tupdate_matrix();\n'):
            with self.subTest(middle=middle):
                patched = guard_preview_submit(BEGIN + SUBMIT + middle + UPDATE)
                self.assertIn('if (!studio::background_frame()) {', patched)
                self.assertIn(SUBMIT + '    }\n' + middle + UPDATE, patched)
    def test_spacing_and_trailing_comment(self):
        submit = '\t vkManager->SubmitCommand ( commandBuffer , true ) ; // submit\n'
        patched = guard_preview_submit(BEGIN + submit + UPDATE)
        self.assertIn(submit + '    }\n' + UPDATE, patched)
    def test_missing_or_ambiguous_submit(self):
        for code in (BEGIN + UPDATE, BEGIN + SUBMIT + SUBMIT + UPDATE,
                     UPDATE + BEGIN + SUBMIT, BEGIN + SUBMIT + UPDATE + UPDATE):
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, 'background skip preview submit'):
                guard_preview_submit(code)
    def test_later_submit_not_touched(self):
        code = BEGIN + SUBMIT + UPDATE + SUBMIT
        self.assertTrue(guard_preview_submit(code).endswith(UPDATE + SUBMIT))

if __name__ == '__main__': unittest.main()
