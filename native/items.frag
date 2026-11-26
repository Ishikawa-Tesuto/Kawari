#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(push_constant) uniform Transform {vec4 center_size; vec4 angle_alpha;} p;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main(){color=texture(image,uv)*p.angle_alpha.y;}
