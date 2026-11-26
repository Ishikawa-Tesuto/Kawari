#include "studio_physics.hpp"
#include <cassert>
namespace studio {
static std::string model="/models/First.model3.json",path;
const std::string& active_original_model(){return model;}
std::string physics_profile_path(){return path;}
}
using namespace studio::physics;
int main(){
    cub::CubismPhysics solver;solver.rig.SubRigCount=2;solver.rig.Settings={{0,2},{2,1}};
    solver.rig.Outputs={{cub::CubismPhysicsSource_Angle,2.f},{cub::CubismPhysicsSource_X,7.f},{cub::CubismPhysicsSource_Angle,-3.f}};
    const cub::csmByte data=2;
    cub::CubismModel model;
    char dir[]="/tmp/kawari-physics-runtime-XXXXXX";assert(mkdtemp(dir));studio::path=std::string(dir)+"/settings";
    {
        Editor editor;editor.bind(&solver,&data,1);assert(editor.available());assert(!active());editor.activate();assert(active()==&editor);
        assert(editor.current.groups.size()==2&&editor.current.groups[0].name=="Hair swing");
        Updater updater(solver,editor);updater.OnLateUpdate(&model,.016f);assert(solver.evaluated==1);
        assert(solver.rig.Outputs[0].AngleScale==2&&solver.rig.Outputs[2].AngleScale==-3);
        editor.current.strength=1.5f;editor.current.groups[0].factor=2;editor.current.groups[1].factor=.5f;editor.current.fps=120;
        updater.OnLateUpdate(&model,.016f);assert(solver.rig.Outputs[0].AngleScale==6&&solver.rig.Outputs[2].AngleScale==-2.25f&&solver.rig.Outputs[1].AngleScale==7);
        assert(solver.rig.Fps==120&&solver.rate_changed==1);assert(editor.dirty());assert(editor.store()&&!editor.dirty());
        updater.OnLateUpdate(&model,.016f);assert(solver.rig.Outputs[0].AngleScale==6&&solver.rate_changed==1);
        editor.current.enabled=false;int evaluated=solver.evaluated;updater.OnLateUpdate(&model,.016f);assert(solver.evaluated==evaluated);
        editor.current.enabled=true;updater.OnLateUpdate(&model,.016f);assert(solver.stabilized==1&&solver.evaluated==evaluated);
        updater.OnLateUpdate(&model,.016f);assert(solver.evaluated==evaluated+1);
        editor.reset();updater.OnLateUpdate(&model,.016f);assert(solver.rig.Outputs[0].AngleScale==2&&solver.rig.Outputs[2].AngleScale==-3&&solver.rig.Fps==60);
        editor.revert();updater.OnLateUpdate(&model,.016f);assert(solver.rig.Outputs[0].AngleScale==6&&solver.rig.Fps==120);
        {Editor candidate;candidate.bind(&solver,&data,1);assert(active()==&editor);}assert(active()==&editor);
        // Return original SDK baseline before simulating a fresh model load.
        editor.reset();editor.apply();
    }
    assert(!active());
    {Editor reloaded;reloaded.bind(&solver,&data,1);reloaded.activate();assert(reloaded.current.strength==1.5f);reloaded.apply();assert(solver.rig.Outputs[0].AngleScale==6);reloaded.reset();reloaded.apply();}
    assert(!active());
    studio::model="/models/Second.model3.json";
    {Editor other;other.bind(&solver,&data,1);other.activate();assert(other.current.strength==1.f&&!other.status.empty());other.apply();assert(solver.rig.Outputs[0].AngleScale==2);}
    {Editor missing;missing.activate();assert(!missing.available());}
    assert(unlink(studio::path.c_str())==0);assert(rmdir(dir)==0);
}
