#version 450
layout(push_constant) uniform Transform {vec4 center_size; vec4 angle_alpha;} p;
layout(location=0) out vec2 uv;
void main(){
 const vec2 corners[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
 uv=corners[gl_VertexIndex]; vec2 q=(uv-.5)*p.center_size.zw;
 float c=cos(p.angle_alpha.x),s=sin(p.angle_alpha.x),aspect=p.angle_alpha.z;
 q=vec2(c*q.x-s*q.y/aspect,s*q.x*aspect+c*q.y);
 gl_Position=vec4(p.center_size.xy+q,0,1);
}
