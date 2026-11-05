$input a_position
$output v_texcoord0, v_texcoord1

#include <bgfx_shader.sh>

uniform mat4 u_shadowMtx;

void main()
{
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	vec4 clip = mul(u_shadowMtx, world);
	float w = abs(clip.w) < 0.000001 ? 1.0 : clip.w;
	v_texcoord0 = clip.xy / w;
	v_texcoord1 = vec2(world.y, 0.0);
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
