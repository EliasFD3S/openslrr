$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_texColor, 0);
uniform vec4 u_color;
void main()
{
    // Font atlas: white RGB and straight coverage alpha, same as the old path.
    gl_FragColor = texture2D(s_texColor, v_texcoord0) * u_color;
}
