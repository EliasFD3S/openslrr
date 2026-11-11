$input v_texcoord0, v_texcoord1, v_worldPos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
// x: shadow texel size. y: caster centre Y. w: 0.25 / radius (stock height ramp).
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

float shadow_coverage(vec2 uv)
{
	float texel = u_shadowRamp.x;
	float border = 2.0 * texel;
	if (uv.x < border || uv.x > 1.0 - border ||
		uv.y < border || uv.y > 1.0 - border)
		return 0.0;
	// Four bilinear taps soften the silhouette without weakening its interior.
	vec2 offset = vec2(0.75 * texel, 0.75 * texel);
	return 0.25 * (texture2D(s_texColor, uv + offset).r
		+ texture2D(s_texColor, uv - offset).r
		+ texture2D(s_texColor, uv + vec2(offset.x, -offset.y)).r
		+ texture2D(s_texColor, uv + vec2(-offset.x, offset.y)).r);
}

void main()
{
	// Use the actual triangle plane, not smoothed vertex normals: pit walls
	// must not receive the floor's shadow at a shared edge. Keep ramps up to 45 degrees.
	vec3 plane = cross(dFdx(v_worldPos), dFdy(v_worldPos));
	if (plane.y * plane.y < 0.5 * dot(plane, plane))
		discard;
	vec2 uv = v_texcoord0;
	float s = shadow_coverage(uv);
	float ramp = height_ramp((v_texcoord1.x - u_shadowRamp.y) * u_shadowRamp.w + 0.5);
	// Stock multiplies the ground by the map. White in the map is black ink.
	s *= ramp * u_shadowRamp.z;
	gl_FragColor = vec4(s, s, s, 1.0);
}
