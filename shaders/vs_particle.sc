$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, a_color0
$output v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_worldPos, v_viewPos, v_normal, v_tint

// Tyre smoke, ground dust and skid marks (#454). Positions are already in
// render world space: billboard corners built on the CPU, facing the camera.
// a_texcoord0 is the corner (-1..1, or the mark's uv), a_texcoord1 (radius,
// seed), a_texcoord2 (height of the centre above its ground, mode).
//
// Lighting is evaluated per vertex and interpolated across the puff, never
// per pixel of dozens of overlapping puffs:
// - v_tint.x: the sun's visibility, four taps of the sun cascades (or of the
//   single fitted field indoors). shadow2D is a level-zero compare here.
// - v_normal: the scenery lamps and car lights (headlights included), with
//   their shadow tiles, by the lamp law of scene_shadow.sh without its
//   facing term (smoke scatters every way), weighted by Henyey-Greenstein
//   g 0.55 toward the camera so a beam seen through smoke glows.
#include <bgfx_shader.sh>

SAMPLER2DSHADOW(s_staticShadow, 3);
SAMPLER2DSHADOW(s_fixtureShadow, 7);
SAMPLER2D(s_lightData, 11);
SAMPLER2DSHADOW(s_headlightShadow, 12);
uniform vec4 u_staticShadowParams;  // enabled, texel size
uniform vec4 u_staticShadowEye;     // eye, fitted field distance
uniform mat4 u_staticShadowMtx;
uniform mat4 u_shadowCascades[4];
uniform vec4 u_shadowOrigin;
uniform vec4 u_shadowRanges;        // cascade radii; x = 0: the single field
uniform vec4 u_pointShadowParams;
uniform vec4 u_lampParams;          // lamp count
uniform vec4 u_lampShadowParams;    // shadowed lamps, inverse atlas size
uniform vec4 u_headlightShadow;     // atlas texel size
uniform vec4 u_particleCam[4];      // right, up, forward, eye

vec4 lightData(int x, int y) { return texelFetch(s_lightData, ivec2(x, y), 0); }
mat4 lightMatrix(int x, int y) {
	return mtxFromCols(lightData(x, y), lightData(x + 1, y), lightData(x + 2, y), lightData(x + 3, y));
}

float sunTaps(vec2 uv, float depth, float texel) {
	return 0.25 * (shadow2D(s_staticShadow, vec3(uv + vec2(-texel, -texel), depth)) +
	               shadow2D(s_staticShadow, vec3(uv + vec2(texel, -texel), depth)) +
	               shadow2D(s_staticShadow, vec3(uv + vec2(-texel, texel), depth)) +
	               shadow2D(s_staticShadow, vec3(uv + vec2(texel, texel), depth)));
}

float sunVisibility(vec3 world) {
	if (u_staticShadowParams.x <= 0.0 || u_pointShadowParams.x > 0.5) return 1.0;
	float distanceToEye = length(world - u_staticShadowEye.xyz);
	float texel = u_staticShadowParams.y;
	if (u_shadowRanges.x <= 0.0) {
		// One fitted field (interiors, small maps): its own matrix and fade.
		vec3 q = mul(u_staticShadowMtx, vec4(world, 1.0)).xyz;
		float qEdge = min(min(q.x, 1.0 - q.x), min(q.y, 1.0 - q.y));
		if (qEdge <= texel * 2.0 || q.z <= 0.0 || q.z >= 1.0) return 1.0;
		float fade = 1.0;
		if (u_staticShadowEye.w > 0.0)
			fade = (1.0 - smoothstep(u_staticShadowEye.w * 0.2, u_staticShadowEye.w, distanceToEye)) *
			       smoothstep(0.0, 0.22, qEdge);
		return mix(1.0, sunTaps(q.xy, q.z - 0.002, texel), u_staticShadowParams.x * fade);
	}
	if (distanceToEye >= u_shadowRanges.w) return 1.0;
	vec4 w = vec4(world - u_shadowOrigin.xyz, 1.0);
	float s1 = step(u_shadowRanges.x, distanceToEye);
	float s2 = step(u_shadowRanges.y, distanceToEye);
	float s3 = step(u_shadowRanges.z, distanceToEye);
	vec3 p = mul(u_shadowCascades[0], w).xyz;
	p = mix(p, mul(u_shadowCascades[1], w).xyz, s1);
	p = mix(p, mul(u_shadowCascades[2], w).xyz, s2);
	p = mix(p, mul(u_shadowCascades[3], w).xyz, s3);
	vec2 tile = mix(vec2(0.0, 0.0), vec2(0.5, 0.0), s1);
	tile = mix(tile, vec2(0.0, 0.5), s2);
	tile = mix(tile, vec2(0.5, 0.5), s3);
	float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
	if (edge <= texel * 4.0 || p.z <= 0.0 || p.z >= 1.0) return 1.0;
	// A smoke puff is no surface: a generous 10 cm bias.
	float range = mix(u_shadowRanges.x, u_shadowRanges.y, s1);
	range = mix(range, u_shadowRanges.z, s2);
	range = mix(range, u_shadowRanges.w, s3);
	float depth = p.z - 0.1 / (2.0 * (range * 1.1 + 768.0));
	return mix(1.0, sunTaps(p.xy * 0.5 + tile, depth, texel), u_staticShadowParams.x);
}

// A shadowed lamp's tile (scenery or car-only), four taps, no receiver
// gradient (none exists for a volume).
float lampTile(mat4 sampleMatrix, vec4 tileInfo, vec2 size, vec3 world, vec3 toward) {
	if (tileInfo.z <= 0.0) return 1.0;
	vec4 clip = mul(sampleMatrix, vec4(world + toward * 0.1, 1.0));
	if (clip.w <= 0.0) return 1.0;
	vec3 p = clip.xyz / clip.w;
	if (p.x <= 0.0 || p.y <= 0.0 || p.z <= 0.0 || p.x >= 1.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;
	vec2 texel = u_lampShadowParams.yz;
	vec2 uv = clamp(tileInfo.xy + p.xy * size, tileInfo.xy + texel * 1.5, tileInfo.xy + size - texel * 1.5);
	float depth = p.z - 0.0002;
	float shadow = 0.25 * (shadow2D(s_fixtureShadow, vec3(uv + vec2(-texel.x, -texel.y), depth)) +
	                       shadow2D(s_fixtureShadow, vec3(uv + vec2(texel.x, -texel.y), depth)) +
	                       shadow2D(s_fixtureShadow, vec3(uv + vec2(-texel.x, texel.y), depth)) +
	                       shadow2D(s_fixtureShadow, vec3(uv + vec2(texel.x, texel.y), depth)));
	return mix(1.0, shadow, tileInfo.z);
}

float headlightTile(int t, vec3 world, vec3 toward) {
	vec4 clip = mul(lightMatrix(96 + 4 * t, 2), vec4(world + toward * 0.1, 1.0));
	if (clip.w <= 0.0) return 1.0;
	vec3 p = clip.xyz / clip.w;
	if (p.x <= 0.0 || p.y <= 0.0 || p.z <= 0.0 || p.x >= 1.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;
	vec4 rect = lightData(120 + t, 2);
	vec2 texel = u_headlightShadow.xy;
	vec2 uv = clamp(rect.xy + p.xy * rect.zw, rect.xy + texel * 1.5, rect.xy + rect.zw - texel * 1.5);
	float depth = p.z - 0.0005;
	return 0.25 * (shadow2D(s_headlightShadow, vec3(uv + vec2(-texel.x, -texel.y), depth)) +
	               shadow2D(s_headlightShadow, vec3(uv + vec2(texel.x, -texel.y), depth)) +
	               shadow2D(s_headlightShadow, vec3(uv + vec2(-texel.x, texel.y), depth)) +
	               shadow2D(s_headlightShadow, vec3(uv + vec2(texel.x, texel.y), depth)));
}

vec3 lampLight(vec3 world) {
	vec3 sum = vec3(0.0, 0.0, 0.0);
	vec3 toCamera = normalize(u_particleCam[3].xyz - world + vec3(0.0, 1e-4, 0.0));
	for (int i = 0; i < 64; ++i) {
		if (float(i) >= u_lampParams.x) break;
		vec4 lamp = lightData(i, 0);
		vec3 delta = lamp.xyz - world;
		float r2 = lamp.w * lamp.w;
		float d2 = dot(delta, delta);
		if (d2 >= r2) continue;
		// A puff is no point on a bulb: it spreads over a metre or more.
		d2 = max(d2, 1.0);
		float inv = inversesqrt(d2);
		vec3 toward = delta * inv;
		vec4 spot = lightData(192 + i, 0);
		vec4 attenuation = lightData(128 + i, 0);
		float cone = 1.0;
		if (spot.w >= -1.5) {
			cone = smoothstep(spot.w, attenuation.w, dot(-toward, spot.xyz));
			cone *= 1.0 - lightData(128 + i, 2).x * smoothstep(0.02, 0.06, -toward.y - spot.y);
		}
		if (cone <= 0.0) continue;
		vec4 colour = lightData(64 + i, 0);
		float visibility = 1.0;
		if (float(i) < u_lampShadowParams.x) {
			visibility *= lampTile(lightMatrix(4 * i, 1), lightData(96 + i, 1),
			                       vec2(0.25, 1.0 / 3.0), world, toward);
			visibility *= lampTile(lightMatrix(120 + 4 * i, 1), lightData(216 + i, 1),
			                       vec2(0.125, 1.0 / 6.0), world, toward);
		} else if (colour.w > 0.5) {
			visibility = headlightTile(int(colour.w - 0.5), world, toward);
		}
		float q = d2 / r2;
		float window = saturate(1.0 - q * q);
		float falloff = max(attenuation.x + attenuation.y * d2 * inv + attenuation.z * d2, 0.25);
		// The light travels away from the lamp (-toward); half of it scatters
		// evenly, half by the phase toward the camera (capped at 3x).
		float g = 0.55;
		float cosine = -dot(toward, toCamera);
		float hg = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-3), 1.5);
		sum += colour.rgb * (window * window * cone * visibility / falloff * (0.5 + 0.5 * min(hg, 3.0)) * 0.4);
	}
	// Below the noon sun on a white surface: a camera inside a lit cloud
	// sees dozens of puffs stacked.
	return min(sum, vec3(0.8, 0.8, 0.8));
}

void main() {
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
	v_texcoord2 = a_texcoord2;
	v_color0 = a_color0;
	v_worldPos = a_position;
	v_viewPos = mul(u_view, vec4(a_position, 1.0)).xyz;
	bool mark = a_texcoord2.y > 1.5;
	v_normal = mark ? vec3(0.0, 0.0, 0.0) : lampLight(a_position);
	v_tint = vec4(mark ? 1.0 : sunVisibility(a_position), 0.0, 0.0, 1.0);
}
