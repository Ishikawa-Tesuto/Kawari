#pragma once
#include <Model/CubismModel.hpp>
namespace Live2D { namespace Cubism { namespace Framework {
constexpr int CubismUpdateOrder_Physics=100;
struct ICubismUpdater { explicit ICubismUpdater(int){} virtual ~ICubismUpdater()=default;virtual void OnLateUpdate(CubismModel*,float)=0;};
}}}
