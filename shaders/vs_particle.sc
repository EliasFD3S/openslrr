$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, a_color0
$output v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_worldPos, v_viewPos, v_normal

// Tyre smoke, ground dust and skid marks (#454). Positions are already in
// render world space: billboard corners built on the CPU, facing the camera.
// a_normal carries the particle's centre, a_texcoord0 the corner (-1..1, or
// the mark's uv), a_texcoord1 (radius, seed), a_texcoord2 (height of the
// centre above its ground, mode).
#include <bgfx_shader.sh>

void main() {
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
	v_texcoord2 = a_texcoord2;
	v_color0 = a_color0;
	v_worldPos = a_position;
	v_viewPos = mul(u_view, vec4(a_position, 1.0)).xyz;
	v_normal = a_normal;
}
