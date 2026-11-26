#pragma once
#include "studio_physics_settings.hpp"
#include <Physics/CubismPhysics.hpp>
#include <Utils/CubismJson.hpp>
#include <Motion/ICubismUpdater.hpp>

namespace studio {
const std::string& active_original_model();
std::string physics_profile_path();
namespace physics {
namespace cub = Live2D::Cubism::Framework;
class Editor;
inline Editor*& active(){static Editor* value=nullptr;return value;}
class Editor {
    struct Output {unsigned index;float scale;};
    std::vector<std::vector<Output>> baseline_;
    cub::CubismPhysics* solver_=nullptr;
    Settings saved_, applied_;
    bool loaded_=false, applied_valid_=false, suspended_=false;
    std::string path_,model_;
public:
    Settings current;
    float original_fps=0;
    std::string status;
    ~Editor(){if(active()==this)active()=nullptr;}
    Editor()=default;
    Editor(const Editor&)=delete;
    Editor& operator=(const Editor&)=delete;
    bool available()const{return solver_!=nullptr;}
    bool dirty()const{return available()&&!same(current,saved_);}
    bool can_save()const{return available()&&!path_.empty();}
    bool has_angles(size_t i)const{return i<baseline_.size()&&!baseline_[i].empty();}
    // Called during candidate setup. Do not publish or read active profile here.
    void bind(cub::CubismPhysics* solver,const cub::csmByte* data,cub::csmSizeInt size){
        if(!solver)return;
        auto* rig=solver->StudioPhysicsRig();if(!rig)return;
        auto* json=cub::Utils::CubismJson::Create(data,size);if(!json)return;
        auto& settings=json->GetRoot()["PhysicsSettings"];
        auto& dictionary=json->GetRoot()["Meta"]["PhysicsDictionary"];
        bool valid=settings.IsArray()&&settings.GetSize()==rig->SubRigCount;
        for(int i=0;valid&&i<rig->SubRigCount;++i){
            auto& value=settings[i]["Id"];std::string id=value.IsString()?value.GetRawString():"";
            if(!safe_field(id)||std::any_of(current.groups.begin(),current.groups.end(),[&](const Group& g){return g.id==id;})){valid=false;break;}
            std::string name=id;
            if(dictionary.IsArray())for(int j=0;j<dictionary.GetSize();++j){auto& entry=dictionary[j];if(entry["Id"].IsString()&&id==entry["Id"].GetRawString()&&entry["Name"].IsString()){name=entry["Name"].GetRawString();break;}}
            current.groups.emplace_back(id,name);baseline_.emplace_back();
            const auto& group=rig->Settings[i];
            for(int j=0;j<group.OutputCount;++j){const unsigned index=unsigned(group.BaseOutputIndex+j);const auto& out=rig->Outputs[index];if(out.Type==cub::CubismPhysicsSource_Angle)baseline_.back().push_back({index,out.AngleScale});}
        }
        cub::Utils::CubismJson::Delete(json);
        if(!valid){current.groups.clear();baseline_.clear();status="Invalid physics group metadata.";return;}
        original_fps=rig->Fps;solver_=solver;saved_=current;
    }
    // Called only from the active model's Update(), after a committed switch.
    void activate(){
        active()=this;if(loaded_)return;loaded_=true;
        if(!available())return;
        path_=physics_profile_path();model_=active_original_model();
        load(path_,model_,current,status);saved_=current;
    }
    void apply(){
        if(!available()||(applied_valid_&&same(current,applied_)))return;
        auto* rig=solver_->StudioPhysicsRig();
        for(size_t i=0;i<baseline_.size();++i)for(const auto& out:baseline_[i])rig->Outputs[out.index].AngleScale=out.scale*current.strength*current.groups[i].factor;
        solver_->StudioSetPhysicsFps(current.fps?float(current.fps):original_fps);
        applied_=current;applied_valid_=true;
    }
    // Same scheduler order as the original SDK physics updater. A metadata
    // failure leaves normal physics intact, without exposing unsafe edits.
    void step(cub::CubismPhysics& solver,cub::CubismModel* model,float dt){
        if(!model)return;
        apply();if(available()&&!current.enabled){suspended_=true;return;}
        if(suspended_){suspended_=false;solver.Stabilization(model);return;}
        solver.Evaluate(model,dt);
    }
    void revert(){current=saved_;status.clear();}
    void reset(){current.reset();status.clear();}
    bool store(){if(!save(path_,model_,current,status))return false;saved_=current;status="Saved";return true;}
};
class Updater final:public cub::ICubismUpdater {
    cub::CubismPhysics& solver_;Editor& editor_;
public:
    Updater(cub::CubismPhysics& solver,Editor& editor):cub::ICubismUpdater(cub::CubismUpdateOrder_Physics),solver_(solver),editor_(editor){}
    void OnLateUpdate(cub::CubismModel* model,float dt)override{editor_.step(solver_,model,dt);}
};
}
}
