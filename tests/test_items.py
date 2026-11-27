from pathlib import Path
import tempfile, subprocess, unittest, base64
ROOT=Path(__file__).resolve().parents[1]
PNG='iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGM8kWLUwMDAwMDEAAUAHawB4t1+dKgAAAAASUVORK5CYII='
GIF='R0lGODlhBAAEAIEAAAAAAP8AAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQJCAAAACwAAAAABAAEAAAICQADABhIsKDBgAAh+QQJDwAAACwDAAMAAQABAIEAAAAA/wAAAAAAAAAIBAADBAQAOw=='
class Items(unittest.TestCase):
    def test_images_scene_and_attachment(self):
        source=r'''
#include "studio_items.hpp"
#include "studio_ui_preferences.hpp"
#include <cassert>
using namespace studio::items;
int main(int argc,char** argv){
 assert(argc==2);std::string d=argv[1],err;setenv("XDG_DATA_HOME",d.c_str(),1);assert(mkdir((d+"/kawari").c_str(),0700)==0);
 Pixels p;assert(load_pixels(d+"/тест.png",p,err));assert(p.w==2&&p.h==2&&p.frames.size()==1);auto& b=p.frames[0];assert(b[0]==100&&b[1]==50&&b[2]==25&&b[3]==128);
 assert(load_pixels(d+"/test.gif",p,err));assert(p.w==4&&p.h==4&&p.frames.size()==2);assert(p.delay[0]==80&&p.delay[1]==150);assert(frame_at(p,79)==0&&frame_at(p,80)==1&&frame_at(p,229)==1&&frame_at(p,230)==0);assert(p.frames[0][0]==255&&p.frames[0][3]==255);assert(p.frames[1][3]==0&&p.frames[1][61]==255&&p.frames[1][63]==255);
 assert(!load_pixels(d+"/bad.gif",p,err));assert(!load_pixels(d+"/missing.png",p,err));
 Item i;assert(import_file(d+"/тест.png",i,err));assert(i.path.find(d+"/kawari/items/asset-")==0);assert(unlink((d+"/тест.png").c_str())==0);assert(load_pixels(i.path,p,err));
 i.x=.25;i.y=-.5;i.size=.6;i.angle=45;i.behind=true;auto before=pose(i,2,1,-1);attach(i,true,2,1,-1);auto after=pose(i,2,1,-1);assert(std::abs(after.x-before.x)<1e-6&&std::abs(after.y-before.y)<1e-6&&std::abs(after.size-before.size)<1e-6);assert(pose(i,4,2,-2).size>after.size);attach(i,false,2,1,-1);assert(std::abs(i.x-before.x)<1e-6);
 std::vector<Item> rows{i},other;auto path=d+"/kawari/items.conf";assert(save(path,rows));assert(read(path,other)&&encode(rows)==encode(other));assert(!decode(encode(rows)+"bad\n",other));assert(encode(rows)==encode(other));struct stat st{};assert(stat(path.c_str(),&st)==0&&(st.st_mode&0777)==0600);assert(chmod(path.c_str(),0644)==0);assert(!read(path,other)&&!save(path,rows));assert(unlink(path.c_str())==0);assert(symlink("/dev/null",path.c_str())==0);assert(!save(path,rows));
 studio::ui::Preferences prefs;std::string old="kawari-ui-1\n100 0 0 0\n";for(int j=0;j<6;++j)old+="520 0 0 0 0 0 0\n";assert(studio::ui::decode_preferences(old,prefs));assert(prefs.tools.size()==7&&!prefs.tools[6].open&&prefs.tools[6].width==440);prefs.tools[6].open=true;assert(studio::ui::decode_preferences(studio::ui::encode_preferences(prefs),prefs)&&prefs.tools[6].open);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'тест.png').write_bytes(base64.b64decode(PNG));(p/'test.gif').write_bytes(base64.b64decode(GIF));(p/'bad.gif').write_bytes(b'GIF89a')
            (p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),str(ROOT/'native/studio_items_image.cpp'),'-lpng','-lgif','-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test'),d],check=True)
    def test_artmesh_geometry_and_migration(self):
        source=r'''
#include "studio_items.hpp"
#include <cassert>
using namespace studio::items;

int main(){
    const Triangle base={{{0,0},{2,0},{0,1}}};
    Anchor anchor;
    float distance=0;
    assert(bind_triangle(base,{.4f,.3f},true,anchor,distance)&&distance==0);
    AttachmentFrame original;
    assert(attachment_frame(base,anchor,original));
    auto near=[](float actual,float expected){assert(std::abs(actual-expected)<.0001f);};
    near(original.origin.x,.4f);near(original.origin.y,.3f);

    Item item;
    item.path="/assets/asset-aB12cD/帽子.png";
    item.mesh={"/models/My Model.model3.json","ArtMesh_Hair",anchor};
    const Pose visible={.4f,.1f,.35f,20};
    fit_mesh(item,visible,original,2);
    auto same=mesh_pose(item,original,2);
    near(same.x,visible.x);near(same.y,visible.y);near(same.size,visible.size);near(same.angle,visible.angle);

    const Triangle rotated={{{1,2},{1,6},{-1,2}}};
    AttachmentFrame after;
    assert(attachment_frame(rotated,anchor,after));
    auto moved=mesh_pose(item,after,2);
    near(moved.x,(1-2*visible.y)/2);near(moved.y,2+4*visible.x);
    near(moved.size,visible.size*2);near(std::remainder(moved.angle,360.f),-70);
    const auto offset=after.inverse({.5f,-.25f});
    const Point delta=after.offset(offset);
    near(delta.x,.5f);near(delta.y,-.25f);

    const Triangle stretched={{{0,0},{2,0},{.4f,1.5f}}};
    assert(attachment_frame(stretched,anchor,after));
    near(after.origin.x,.52f);near(after.origin.y,.45f);
    Anchor outside;
    assert(!bind_triangle(base,{-.3f,.5f},true,outside,distance));
    assert(bind_triangle(base,{-.3f,.5f},false,outside,distance));
    near(distance,.09f);
    assert(attachment_frame(base,outside,after));
    near(after.origin.x,0);near(after.origin.y,.5f);
    const Triangle collapsed={{{0,0},{0,0},{0,0}}};
    assert(!bind_triangle(collapsed,{0,0},false,outside,distance));
    assert(!attachment_frame(collapsed,anchor,after));

    const float m[]={2,0,0,0,0,3,0,0,0,0,1,0,1,-1,0,2};
    Point projected;
    assert(project_vertex(m,2,{.25f,.5f},projected));
    near(projected.x,1.5f);near(projected.y,.25f);

    Item old;old.path="/assets/root.png";old.attached=true;
    std::vector<Item> decoded;
    const std::string legacy=encode({old});
    assert(legacy.find("kawari-items-1\n")==0&&decode(legacy,decoded)&&decoded[0].attached&&!decoded[0].mesh);
    const std::string modern=encode({old,item});
    assert(modern.find("kawari-items-2\n")==0&&decode(modern,decoded)&&decoded.size()==2);
    assert(decoded[0].attached&&decoded[1].mesh.id=="ArtMesh_Hair"&&decoded[1].mesh.model==item.mesh.model);
    assert(encode(decoded)==modern);
    item.mesh.anchor.edge=3;
    assert(!decode(encode({item}),decoded));
    assert(encode(decoded)==modern);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_stage_order(self):
        patch=(ROOT/'scripts/patch_sdk.py').read_text()
        self.assertLess(patch.index('studio::items::begin_mesh_frame();'),patch.index('studio::items::render(target'))
        self.assertLess(patch.index('studio::items::render(target'),patch.index('studio::broadcast_frame(target'))
        self.assertIn('studio::items::capture_mesh_frame(*_model, matrix.GetArray()',patch)
        self.assertIn('studio::items::forget_mesh_model(_model)',patch)
        self.assertIn('studio::items::shutdown()',patch)
        for f in ['scripts/build.sh','scripts/source-id.sh','scripts/patch_sdk.py']:
            self.assertIn('studio_items_vk.cpp',(ROOT/f).read_text())
        ui=(ROOT/'native/studio_ui.cpp').read_text()
        self.assertIn('items_popup(sz)',ui)
        self.assertNotIn('items::scene()',ui[ui.index('void ui_model_changed()'):ui.index('void ui_shutdown()')])
if __name__=='__main__':unittest.main()
