$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, a_color0
$output v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint

#include <bgfx_shader.sh>

uniform mat4 u_staticShadowMtx;

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
	v_texcoord2 = a_texcoord2;
	v_color0 = a_color0;
	v_tint = vec4(1.0, 1.0, 1.0, 1.0);
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	// Pull the receiver out of its own surface. A few centimetres stops the
	// cornice and the wall from shadowing themselves in stripes.
	float nl = dot(v_normal, v_normal);
	if (nl > 1e-8)
		world.xyz += v_normal * (0.05 / sqrt(nl));
	v_staticShadow = mul(u_staticShadowMtx, world).xyz;
	v_worldPos = world.xyz;
}
