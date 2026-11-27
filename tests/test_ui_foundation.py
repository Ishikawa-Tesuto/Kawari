"""UI foundation: source contracts and optional headless Dear ImGui test."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Foundation(unittest.TestCase):
    def test_shared_window_and_toolbar(self):
        source = (ROOT / 'native/studio_ui.cpp').read_text()
        theme = (ROOT / 'native/studio_ui_theme.hpp').read_text()
        for title, state in [('Parameters###Parameter Editor','parameter_open'),
                             ('Physics','physics_open'), ('Animations','animations_open'),
                             ('Settings###Broadcast Settings','broadcast_open'), ('Integrations###Plugins','plugins_open'),
                             ('Expressions & Hotkeys','expression_open')]:
            self.assertIn(f'ui::begin_tool("{title}",{state},', source)
        self.assertIn('ImGui::BeginPopupModal(ui::tr("Model Library###model_library","Библиотека моделей###model_library"),&library_open,', source)
        self.assertIn('ImGui::Begin(label.c_str(),&open,', theme)
        self.assertIn('return visible&&open;', theme)
        self.assertIn('ui::tool_button("##nav_tool",*t.open,button_size)', source)
        self.assertIn('ImGui::Begin(id.c_str(),&open,', theme)
        self.assertIn('close_other_docks(i)',source)
        self.assertEqual(source.count('ImGui::Begin('),1)
        self.assertIn('ImGui::Begin("##dock_resize"',source)

    def test_closing_parameters_keeps_draft_and_releases_test(self):
        source = (ROOT / 'native/studio_ui.cpp').read_text()
        body = source.split('void parameter_popup(',1)[1].split('struct ExpressionRow',1)[0]
        self.assertIn('if(!parameter_open){if(preview_active)stop_preview();return;}', body)
        self.assertIn('if(preview_active)stop_preview();\n        ImGui::End();return;', body)
        self.assertEqual(body.count('if(ImGui::Button("Close")){stop_preview();parameter_open=false;parameter_focus=false;}'),2)
        self.assertNotIn('if(!parameter_open){\n            select_parameter',body)
        self.assertIn('parameter_draft_pending()||physics_dirty()',source)

    def test_staging_and_source_id(self):
        for file in ('scripts/build.sh','scripts/patch_sdk.py','scripts/source-id.sh'):
            self.assertIn('studio_ui_theme.hpp',(ROOT/file).read_text())
        subprocess.run(['bash',str(ROOT/'scripts/source-id.sh')],check=True,capture_output=True)

    def test_font_initialization_order(self):
        source = (ROOT / 'native/studio_ui.cpp').read_text()
        self.assertLess(source.index('ImGui::CreateContext()'), source.index('ui::load_ui_font()'))
        self.assertLess(source.index('ui::load_ui_font()'), source.index('ImGui_ImplVulkan_CreateFontsTexture()'))

    def test_headless_imgui(self):
        vendor = ROOT/'vendor/imgui'
        if not (vendor/'imgui.cpp').is_file():
            self.skipTest('Run after scripts/build.sh has fetched Dear ImGui')
        source = r'''
#include "studio_ui_theme.hpp"
#include <cassert>
#include <cmath>
int main(){
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,720);
    assert(studio::ui::load_ui_font());
    unsigned char* pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    assert(pixels && w>0 && h>0);
    assert(io.FontDefault->FindGlyphNoFallback(0x0401));
    assert(io.FontDefault->FindGlyphNoFallback(0x0451));
    for(ImWchar c=0x0410;c<=0x044f;++c)assert(io.FontDefault->FindGlyphNoFallback(c));
    studio::ui::apply_theme(); auto& s=ImGui::GetStyle();
    assert(s.WindowRounding==0 && s.FrameRounding==0 && s.ChildRounding==0);
    assert(s.WindowPadding.x==12 && s.WindowBorderSize==1);
    bool open=true;
    for(int i=0;i<3;++i){
        io.DeltaTime=1.f/60.f; ImGui::NewFrame();
        studio::ui::begin_chrome("##test_chrome",0,1280,32,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
        assert(std::abs(ImGui::GetWindowSize().y-32.f)<.01f);
        assert(s.WindowPadding.x==12 && s.WindowBorderSize==1);
        studio::ui::tool_button("Parameters",true);
        studio::ui::navigation_icon(1,true);
        if(studio::ui::begin_tool("Parameters",open,io.DisplaySize,32,720,650)){
            assert(ImGui::GetWindowSize().x<=1260 && ImGui::GetWindowSize().y<=644);
            ImGui::TextUnformatted("Test panel");
        }
        ImGui::End(); ImGui::End(); ImGui::Render();
    }
    studio::ui_workspace()=studio::ui_workspace_layout(1280,720,false,520);
    studio::ui::dock_title()="Physics";
    ImGui::NewFrame();
    assert(studio::ui::begin_tool("Physics",open,io.DisplaySize,36,520,550));
    const auto a=studio::ui_workspace();
    assert(std::abs(ImGui::GetWindowPos().x-(a.width-a.right+7.f))<.01f);
    assert(std::abs(ImGui::GetWindowPos().y-a.bar)<.01f);
    assert(std::abs(ImGui::GetWindowSize().x-(a.right-7.f))<.01f);
    assert(std::abs(ImGui::GetWindowSize().y-(a.height-2*a.bar))<.01f);
    ImGui::End(); ImGui::Render();
    assert(open);
    studio::ui::preferences().scale=130; studio::ui::apply_theme();
    assert(std::abs(io.FontGlobalScale-1.3f)<.01f && s.FrameBorderSize==1);
    io.DisplaySize=ImVec2(360,620);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(300,480));
    ImGui::Begin("Narrow panel");
    const float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
    studio::ui::primary_button("Activate");
    const auto first=ImGui::GetItemRectMin();
    studio::ui::next_action("Edit");ImGui::Button("Edit");
    assert(ImGui::GetItemRectMin().y==first.y);
    ImGui::Button("Import VTube Studio shortcuts");
    const auto wide_button=ImGui::GetItemRectMin();
    studio::ui::next_action("Delete expression");ImGui::Button("Delete expression");
    assert(ImGui::GetItemRectMin().y>wide_button.y);
    assert(ImGui::GetItemRectMax().x<=right);
    studio::ui::field_label("Navigation language");
    int language=0;
    ImGui::Combo("##language",&language,"English\0Русский\0");
    assert(ImGui::GetItemRectMax().x<=right);
    studio::ui::choice_row("A long expression name that extends beyond the list width", "Ctrl + Alt + S",true);
    assert(ImGui::GetItemRectSize().y>=28*studio::ui::scale());
    ImGui::End();ImGui::Render();
    ImGui::DestroyContext();
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d); (p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-I',str(vendor),'-I',str(ROOT/'native'),str(p/'test.cpp'),*[str(vendor/f) for f in ('imgui.cpp','imgui_draw.cpp','imgui_widgets.cpp','imgui_tables.cpp')],'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True,cwd=d)

if __name__=='__main__': unittest.main()
