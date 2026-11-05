$input v_texcoord0, v_texcoord1

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
// y: caster centre Y. w: 0.25 / radius (stock height ramp).
uniform vec4 u_shadowRamp;

float height_ramp(float u)
{
	float i = clamp(u, 0.0, 1.0) * 255.0;
	float r = 0.0;
	if (i < 64.0)
		r = i * 4.0;
	else if (i < 128.0)
		r = 255.0;
	else if (i < 144.0)
		r = (143.0 - i) * 16.0;
	return clamp(r, 0.0, 255.0) / 255.0;
}

void main()
{
	vec2 uv = v_texcoord0;
	float s = texture2D(s_texColor, uv).r;
	if (uv.x < 0.004 || uv.x > 0.996 || uv.y < 0.004 || uv.y > 0.996)
		s = 0.0;
	float ramp = height_ramp((v_texcoord1.x - u_shadowRamp.y) * u_shadowRamp.w + 0.5);
	// Full white in the map is half ink, so the floor stays readable.
	s *= ramp * 0.5;
	gl_FragColor = vec4(s, s, s, 1.0);
}
