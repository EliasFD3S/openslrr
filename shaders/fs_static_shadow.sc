$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint
// bgfx links a program only when this $input list equals the vertex shader's
// $output list (vs_mesh, vs_mesh_inst). Keep the two in step.

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
// y: alpha reference, z: diffuse UV channel.
uniform vec4 u_staticShadowParams;

void main()
{
	if (u_staticShadowParams.y >= 0.0) {
		vec2 uv = u_staticShadowParams.z > 1.5 ? v_texcoord2
		        : u_staticShadowParams.z > 0.5 ? v_texcoord1 : v_texcoord0;
		if (texture2D(s_texColor, uv).a <= u_staticShadowParams.y)
			discard;
	}
	gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
}
