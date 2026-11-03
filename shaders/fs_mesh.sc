$input v_normal, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
// x: cutout (discard a<0.5)
// y: unlit (skip ndl — OSD icons)
// z: write texture alpha (blend) instead of opaque 1
uniform vec4 u_alphaTest;
// Text.changeColor modulate (ARGB → rgba 0..1). Default white.
uniform vec4 u_color;

void main()
{
	vec3 n = normalize(v_normal);
	float ndl = max(abs(dot(n, normalize(vec3(0.35, 0.85, 0.45)))), 0.22);
	vec4 tex = texture2D(s_texColor, v_texcoord0);
	if (u_alphaTest.x > 0.5 && tex.a < 0.5)
		discard;
	float light = (u_alphaTest.y > 0.5) ? 1.0 : ndl;
	float outA = (u_alphaTest.z > 0.5) ? tex.a : 1.0;
	gl_FragColor = vec4(tex.rgb * light * u_color.rgb, outA * u_color.a);
}
