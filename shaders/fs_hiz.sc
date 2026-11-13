$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
// xy: size of one texel in the source, in uv.
uniform vec4 u_hizParams;

void main()
{
	// One output texel covers a 2x2 block. Reversed-Z stores the far plane at
	// 0, so the farthest depth in the block is the smallest value.
	vec2 texel = u_hizParams.xy;
	vec2 base = v_texcoord0 - texel * 0.5;
	float a = texture2D(s_texColor, base).r;
	float b = texture2D(s_texColor, base + vec2(texel.x, 0.0)).r;
	float c = texture2D(s_texColor, base + vec2(0.0, texel.y)).r;
	float d = texture2D(s_texColor, base + vec2(texel.x, texel.y)).r;
	float farthest = min(min(a, b), min(c, d));
	gl_FragColor = vec4(farthest, 0.0, 0.0, 1.0);
}
