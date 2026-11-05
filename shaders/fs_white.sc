$input v_texcoord0

#include <bgfx_shader.sh>

void main()
{
	gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);
	gl_FragColor.a += v_texcoord0.x * 0.0;
}
