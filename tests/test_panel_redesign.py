"""Panel navigation contracts and native model display/filter logic."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class Panels(unittest.TestCase):
    def test_model_labels(self):
        code=r'''
#include "studio_ui_layout.hpp"
#include <cassert>
using namespace studio;
int main(){
    assert(ui_model_label("/Models/TesutoXXI/TesutoXXI.model3.json")=="TesutoXXI");
    assert(ui_model_label("/Models/TesutoModelX.model3.json")=="TesutoModelX");
    assert(ui_model_label("/Models/model.json")=="model.json");
    assert(ui_model_label("/Models/not-a.model3.json.backup")=="not-a.model3.json.backup");
    assert(ui_parameter_matches(ui_model_label("/Models/HotomiXXI.model3.json"),"HOTOMI"));
    assert(!ui_parameter_matches(ui_model_label("/Models/TesutoXXI.model3.json"),"Hotomi"));
    assert(ui_parameter_matches("/Models/Folder A/TesutoXXI.model3.json","folder a"));
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_library_preserves_original_identity_and_guard(self):
        s=(ROOT/'native/studio_ui.cpp').read_text()
        body=s.split('void library_popup(',1)[1].split('int parameter_selected=',1)[0]
        self.assertIn('path==current',body)
        self.assertIn('path==model_path',body)
        self.assertIn('ImGui::PushID(path.c_str())',body)
        self.assertIn('request_model(dir,model_path)',body)
        self.assertIn('Discard unsaved model settings',body)
        self.assertIn('Search model name or path',body)
        self.assertIn('if(library_show_paths)',body)

    def test_tabs_keep_original_mailboxes(self):
        s=(ROOT/'native/studio_ui.cpp').read_text()
        settings=s.split('void broadcast_popup(',1)[1].split('void physics_popup(',1)[0]
        for name in ('Rendering','Broadcast','General'):
            self.assertIn(f'BeginTabItem(ui::tr("{name}###',settings)
        self.assertIn('ui_write_broadcast(d)',settings)
        self.assertIn('broadcast-save.tsv',settings)
        self.assertIn('apply_live_broadcast_settings',settings)
        self.assertLess(settings.index('ImGui::EndTabBar()'),settings.index('ui::primary_button("Save")'))
        integrations=s.split('void plugins_popup(',1)[1].split('void expressions_popup(',1)[0]
        for name in ('Plugins','Twitch'):
            self.assertIn(f'BeginTabItem(ui::tr("{name}###',integrations)
        self.assertIn('plugins-request',integrations)
        self.assertIn('twitch_panel();',integrations)
        self.assertNotIn('oauth.json',integrations)
        expressions=s.split('void expressions_popup(',1)[1].split('bool init(',1)[0]
        self.assertIn('BeginTabItem(ui::tr("Expressions###',expressions)
        self.assertIn('BeginTabItem(ui::tr("Hotkeys###',expressions)
        self.assertIn('expression_list(false,expression_filter,selected_expression)',expressions)
        self.assertIn('expression_list(true,hotkey_filter,selected_hotkey)',expressions)
        self.assertLess(expressions.index('shortcut_events.clear();\n    if(!expression_open)return;'),expressions.index('BeginTabBar'))
        controls=s.split('const ExpressionRow* expression_list(',1)[1].split('bool init(',1)[0]
        for action in ('bind','toggle','trigger','global','import-vts'):
            self.assertIn(f'expression_request("{action}"',controls)

if __name__=='__main__': unittest.main()
