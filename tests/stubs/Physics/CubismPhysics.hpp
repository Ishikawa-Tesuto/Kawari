#pragma once
#include <vector>
#include <Model/CubismModel.hpp>
namespace Live2D { namespace Cubism { namespace Framework {
using csmByte=unsigned char;using csmSizeInt=int;
enum CubismPhysicsSource {CubismPhysicsSource_X,CubismPhysicsSource_Y,CubismPhysicsSource_Angle};
struct CubismPhysicsOutput {CubismPhysicsSource Type;float AngleScale;};
struct CubismPhysicsSubRig {int BaseOutputIndex,OutputCount;};
struct CubismPhysicsRig {int SubRigCount=0;float Fps=60;std::vector<CubismPhysicsOutput> Outputs;std::vector<CubismPhysicsSubRig> Settings;};
struct CubismPhysics {
    CubismPhysicsRig rig;int evaluated=0,stabilized=0,rate_changed=0;
    CubismPhysicsRig* StudioPhysicsRig(){return &rig;}
    void StudioSetPhysicsFps(float fps){if(rig.Fps!=fps){rig.Fps=fps;++rate_changed;}}
    void Evaluate(CubismModel*,float){++evaluated;}
    void Stabilization(CubismModel*){++stabilized;}
};
}}}
