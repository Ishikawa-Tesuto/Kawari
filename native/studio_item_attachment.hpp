#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace studio { namespace items {
struct Point {
    float x, y;
    Point(float x=0,float y=0) : x(x),y(y) {}
    Point operator+(Point p) const {return {x+p.x,y+p.y};}
    Point operator-(Point p) const {return {x-p.x,y-p.y};}
    Point operator*(float s) const {return {x*s,y*s};}
};
inline float dot(Point a,Point b){return a.x*b.x+a.y*b.y;}
inline float cross(Point a,Point b){return a.x*b.y-a.y*b.x;}
inline bool finite(Point p){return std::isfinite(p.x)&&std::isfinite(p.y);}

struct Triangle {Point vertices[3];};
struct Anchor {
    unsigned triangle=0, edge=0;
    float u=0,v=0,length=1;
    bool valid() const {
        return triangle<=0x7fffffffU/3&&edge<3&&u>=0.f&&v>=0.f&&u+v<=1.00001f&&
               length>=.00001f&&length<=100000.f;
    }
};
struct AttachmentFrame {
    Point origin,axis;
    float scale=1;
    float rotation()const{return std::atan2(axis.y,axis.x)*57.29577951f;}
    Point offset(Point local)const{
        return {(axis.x*local.x-axis.y*local.y)*scale,(axis.y*local.x+axis.x*local.y)*scale};
    }
    Point inverse(Point delta)const{
        return {dot(delta,axis)/scale,cross(axis,delta)/scale};
    }
};

inline bool project_vertex(const float* m,float aspect,Point vertex,Point& projected){
    const float w=m[3]*vertex.x+m[7]*vertex.y+m[15];
    if(!std::isfinite(w)||w<=.000001f)return false;
    projected={(m[0]*vertex.x+m[4]*vertex.y+m[12])*aspect/w,
               (m[1]*vertex.x+m[5]*vertex.y+m[13])/w};
    return finite(projected);
}

inline bool attachment_frame(const Triangle& triangle,const Anchor& anchor,AttachmentFrame& out){
    if(!anchor.valid())return false;
    const Point a=triangle.vertices[anchor.edge],b=triangle.vertices[(anchor.edge+1)%3],c=triangle.vertices[(anchor.edge+2)%3];
    const Point edge=b-a;
    const float length=std::hypot(edge.x,edge.y);
    if(!std::isfinite(length)||length<.00001f)return false;
    out.origin=a+edge*anchor.u+(c-a)*anchor.v;
    out.axis=edge*(1.f/length);
    out.scale=length/anchor.length;
    return finite(out.origin)&&std::isfinite(out.scale);
}

// Find a local anchor that stays inside the triangle as its vertices deform.
inline bool bind_triangle(const Triangle& triangle,Point point,bool inside_only,Anchor& anchor,float& distance){
    unsigned longest=0;
    float length2=0;
    for(unsigned i=0;i<3;++i){
        const Point side=triangle.vertices[(i+1)%3]-triangle.vertices[i];
        const float size=dot(side,side);
        if(size>length2){longest=i;length2=size;}
    }
    const Point a=triangle.vertices[longest],b=triangle.vertices[(longest+1)%3],c=triangle.vertices[(longest+2)%3];
    const float area=cross(b-a,c-a);
    if(!std::isfinite(area)||!std::isfinite(length2)||length2<1e-10f||std::abs(area)<length2*1e-5f)return false;
    float u=cross(point-a,c-a)/area, v=cross(b-a,point-a)/area;
    if(!std::isfinite(u)||!std::isfinite(v))return false;
    if(u<0.f||v<0.f||u+v>1.f){
        if(inside_only)return false;
        float best=std::numeric_limits<float>::infinity();
        const Point ordered[]={a,b,c};
        const float weight_u[]={0,1,0},weight_v[]={0,0,1};
        for(unsigned i=0;i<3;++i){
            const unsigned j=(i+1)%3;
            const Point side=ordered[j]-ordered[i];
            const float along=std::max(0.f,std::min(1.f,dot(point-ordered[i],side)/dot(side,side)));
            const Point delta=point-(ordered[i]+side*along);
            const float d=dot(delta,delta);
            if(d<best){
                best=d;
                u=weight_u[i]+(weight_u[j]-weight_u[i])*along;
                v=weight_v[i]+(weight_v[j]-weight_v[i])*along;
            }
        }
        distance=best;
    }else distance=0;
    anchor.edge=longest;anchor.u=u;anchor.v=v;anchor.length=std::sqrt(length2);
    return anchor.valid()&&std::isfinite(distance);
}
}}
