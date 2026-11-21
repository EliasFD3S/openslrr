$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, a_color0
$output v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint

#include <bgfx_shader.sh>

uniform mat4 u_staticShadowMtx;
uniform vec4 u_alphaTest;

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
	// Night detail and signal glass are authored coplanar with the surface
	// they light (#281). A relative depth bias wins the depth ties without
	// moving the geometry: with reverse Z, scaling clip z by 1 + 1e-4 brings
	// the overlay 1e-4 x its distance nearer in depth only (0.5 mm at 5 m;
	// 1e-5 lost ties on a slanted wall in the harness).
	// The former 2 mm push along the normal made glass float off its body
	// and showed glass authored behind it.
	if (u_alphaTest.y > 1.5)
		gl_Position.z += gl_Position.z * 1e-4;
    v_worldPos = world.xyz; // Physical position for local light and reflection sampling.
	// Pull the receiver out of its own surface. A few centimetres stops the
	// cornice and the wall from shadowing themselves in stripes.
	float nl = dot(v_normal, v_normal);
	if (nl > 1e-8)
		world.xyz += v_normal * (0.05 / sqrt(nl));
	v_staticShadow = mul(u_staticShadowMtx, world).xyz;
}
