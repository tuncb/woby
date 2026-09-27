$input v_texcoord0, v_markerId

#include <bgfx/bgfx_shader.sh>

uniform vec4 u_color;

void main()
{
    if (length(v_texcoord0) > 0.5 || u_color.a <= 0.000001) {
        discard;
    }

    gl_FragData[0] = u_color;
    uint id=uint(v_markerId.x)|(uint(v_markerId.y)<<16u);
    gl_FragData[1]=vec4(float(id&255u),float((id>>8u)&255u),float((id>>16u)&255u),float(id>>24u))/255.0;
}
