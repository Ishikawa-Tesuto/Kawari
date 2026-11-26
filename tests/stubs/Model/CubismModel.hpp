#pragma once
namespace Live2D { namespace Cubism { namespace Framework {
struct FakeString { const char* GetRawString() const { return "ParamTest"; } };
struct FakeId { FakeString GetString() const {return {}; } };
struct CubismModel {
    FakeId id; float value=0; bool repeat=false;
    int GetParameterCount() {return 1;}
    FakeId* GetParameterId(int) {return &id;}
    float GetParameterMinimumValue(int) {return -1;}
    float GetParameterMaximumValue(int) {return 1;}
    float GetParameterDefaultValue(int) {return 0;}
    float GetParameterValue(int) {return value;}
    bool IsRepeat(int) const {return repeat;}
    void SetParameterValue(int,float v) {value=v;}
};
}}}
