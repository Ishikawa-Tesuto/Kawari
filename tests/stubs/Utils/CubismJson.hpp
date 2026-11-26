#pragma once
#include <Physics/CubismPhysics.hpp>
#include <map>
#include <string>
#include <vector>
namespace Live2D { namespace Cubism { namespace Framework { namespace Utils {
struct Value {
    enum Kind{Null,Array,String,Object} kind=Null;
    std::vector<Value> array;std::map<std::string,Value> object;std::string string;
    bool IsArray()const{return kind==Array;}
    bool IsString()const{return kind==String;}
    int GetSize()const{return int(array.size());}
    const char* GetRawString()const{return string.c_str();}
    Value& operator[](const char* key){return object[key];}
    Value& operator[](int index){return array.at(size_t(index));}
    static Value text(const char* text){Value v;v.kind=String;v.string=text;return v;}
    static Value list(){Value v;v.kind=Array;return v;}
};
struct CubismJson {
    Value root;
    static CubismJson* Create(const csmByte* data,csmSizeInt size){
        if(!data||size!=1)return nullptr;
        auto* json=new CubismJson;auto& groups=json->root["PhysicsSettings"];groups=Value::list();
        auto& dictionary=json->root["Meta"]["PhysicsDictionary"];dictionary=Value::list();
        const int count=*data;
        for(int i=0;i<count;++i){Value g;const char* id=i==0?"Hair":"Tail";g["Id"]=Value::text(id);groups.array.push_back(g);g["Name"]=Value::text(i==0?"Hair swing":"Tail motion");dictionary.array.push_back(g);}
        return json;
    }
    static void Delete(CubismJson* json){delete json;}
    Value& GetRoot(){return root;}
};
}}}}
