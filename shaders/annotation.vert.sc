$input a_position

#include <bgfx/bgfx_shader.sh>

void main()
{
    // The stroke is already projected and expanded to its pixel width.
    gl_Position = vec4(a_position, 1.0);
}
