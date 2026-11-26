$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, a_color0
$output v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_worldPos, v_viewPos, v_normal, v_tint

// Tyre smoke, ground dust and skid marks (#454). Positions are already in
// render world space: billboard corners built on the CPU, facing the camera.
// a_normal carries the lamp and headlight light the CPU summed at the
// particle, a_texcoord0 the corner (-1..1, or the mark's uv), a_texcoord1
// (radius, seed), a_texcoord2 (height of the centre above its ground, mode).
// v_tint.x is the sun's visibility at this corner: one tap of the sun
// cascades per vertex, interpolated across the puff, so smoke in a
// building's or a car's shadow darkens without a per-pixel shadow lookup.
#include <bgfx_shader.sh>

SAMPLER2DSHADOW(s_staticShadow, 3);
uniform vec4 u_staticShadowParams;  // enabled, texel size
uniform vec4 u_staticShadowEye;
uniform mat4 u_shadowCascades[4];
uniform vec4 u_shadowOrigin;
uniform vec4 u_shadowRanges;  // cascade radii; x = 0: no cascades

float sunTap(vec2 uv, float depth) {
	return shadow2D(s_staticShadow, vec3(uv, depth));
}

float sunVisibility(vec3 world) {
	if (u_staticShadowParams.x <= 0.0 || u_shadowRanges.x <= 0.0) return 1.0;
	float distanceToEye = length(world - u_staticShadowEye.xyz);
	vec4 w = vec4(world - u_shadowOrigin.xyz, 1.0);
	float s1 = step(u_shadowRanges.x, distanceToEye);
	float s2 = step(u_shadowRanges.y, distanceToEye);
	float s3 = step(u_shadowRanges.z, distanceToEye);
	if (distanceToEye >= u_shadowRanges.w) return 1.0;
	vec3 p = mul(u_shadowCascades[0], w).xyz;
	p = mix(p, mul(u_shadowCascades[1], w).xyz, s1);
	p = mix(p, mul(u_shadowCascades[2], w).xyz, s2);
	p = mix(p, mul(u_shadowCascades[3], w).xyz, s3);
	vec2 tile = mix(vec2(0.0, 0.0), vec2(0.5, 0.0), s1);
	tile = mix(tile, vec2(0.0, 0.5), s2);
	tile = mix(tile, vec2(0.5, 0.5), s3);
	float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
	if (edge <= u_staticShadowParams.y * 4.0 || p.z <= 0.0 || p.z >= 1.0) return 1.0;
	vec2 uv = p.xy * 0.5 + tile;
	// A smoke puff is no surface: a generous 10 cm bias, four taps a texel
	// apart.
	float range = mix(u_shadowRanges.x, u_shadowRanges.y, s1);
	range = mix(range, u_shadowRanges.z, s2);
	range = mix(range, u_shadowRanges.w, s3);
	float depth = p.z - 0.1 / (2.0 * (range * 1.1 + 768.0));
	vec2 t = vec2(u_staticShadowParams.y, u_staticShadowParams.y);
	float sum = sunTap(uv + vec2(-t.x, -t.y), depth) + sunTap(uv + vec2(t.x, -t.y), depth) +
	            sunTap(uv + vec2(-t.x, t.y), depth) + sunTap(uv + vec2(t.x, t.y), depth);
	return mix(1.0, sum * 0.25, u_staticShadowParams.x);
}

void main() {
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
	v_texcoord2 = a_texcoord2;
	v_color0 = a_color0;
	v_worldPos = a_position;
	v_viewPos = mul(u_view, vec4(a_position, 1.0)).xyz;
	v_normal = a_normal;
	v_tint = vec4(a_texcoord2.y > 1.5 ? 1.0 : sunVisibility(a_position), 0.0, 0.0, 1.0);
}
