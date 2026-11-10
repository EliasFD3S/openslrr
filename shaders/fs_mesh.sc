$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_lightmap, 1);
SAMPLER2D(s_meshMix, 2);
SAMPLER2D(s_staticShadow, 3);
// x: strength, y: texel size, z: depth bias.
uniform vec4 u_staticShadowParams;
uniform vec4 u_staticShadowEye;
uniform vec4 u_sunAmb;
uniform vec4 u_sunDiff;
uniform vec4 u_sunDir;
// x: legacy cutout when positive, or -(reference + 1) / 255 for SCX4
// y: unlit (skip ndl — OSD icons)
// z: write texture alpha (blend) instead of opaque 1
uniform vec4 u_alphaTest;
// Text.changeColor modulate (ARGB → rgba 0..1). Default white.
uniform vec4 u_color;
// x: baked lightmap on. y: diffuse uv set. z: lightmap uv set.
// w: second diffuse uv set + 1, or zero when absent.
uniform vec4 u_lightmap;
// scale.xy, offset.zw. Identity leaves the uv unchanged.
uniform vec4 u_lightmapUv;

vec2 uvSet(float channel, vec2 uv0, vec2 uv1, vec2 uv2)
{
	if (channel > 1.5) return uv2;
	if (channel > 0.5) return uv1;
	return uv0;
}

void main()
{
	vec3 n = normalize(v_normal);
	float ndl = max(abs(dot(n, normalize(vec3(0.35, 0.85, 0.45)))), 0.22);
	vec4 tex = texture2D(s_texColor, uvSet(u_lightmap.y, v_texcoord0, v_texcoord1, v_texcoord2));
	if (u_lightmap.w > 0.5) {
		vec4 layer = texture2D(s_meshMix, uvSet(u_lightmap.w - 1.0, v_texcoord0, v_texcoord1, v_texcoord2));
		tex.rgb = mix(tex.rgb, layer.rgb, layer.a);
	}
	if ((u_alphaTest.x > 0.5 && tex.a < 0.5)
		|| (u_alphaTest.x < 0.0 && tex.a * 255.0 <= -u_alphaTest.x * 255.0 - 1.0))
		discard;
	float light = (u_alphaTest.y > 0.75) ? 1.0 : ((u_alphaTest.y > 0.25) ? 0.65 + 0.35 * ndl : ndl);
	float outA = (u_alphaTest.z > 0.5) ? tex.a : 1.0;
	vec3 tint = u_color.rgb;
	if (u_alphaTest.w < -2.5) {
		tex.rgb *= v_color0.rgb;
	} else if (u_alphaTest.w < -1.5) {
		tex.rgb = mix(tint, tex.rgb, tex.a);
		tint = vec3(1.0, 1.0, 1.0);
	} else if (u_alphaTest.w < -0.5) {
		// City window textures provide the mix weight for the vertex-coloured wall.
		tex.rgb = mix(v_color0.rgb, tex.rgb, tex.a);
	} else if (u_alphaTest.w > 1.5) {
		// ColorMixTex over white diffuse, in premultiplied alpha space.
		tex.rgb = mix(vec3(tex.a, tex.a, tex.a), tex.rgb, tex.a);
	} else if (u_alphaTest.w > 0.5) {
		tint = mix(u_color.rgb, vec3(1.0, 1.0, 1.0), tex.a);
	}
	// Stock maps store the sun and the shadows in a second texture. The
	// exe multiplies the diffuse by it and does not add a dynamic light.
	vec2 raw = uvSet(u_lightmap.z, v_texcoord0, v_texcoord1, v_texcoord2);
	vec2 luv = vec2(((raw.x - 0.5) - u_lightmapUv.z) * u_lightmapUv.x + 0.5,
	                0.5 - ((0.5 - raw.y) - u_lightmapUv.w) * u_lightmapUv.y);
	vec3 bake = texture2D(s_lightmap, luv).rgb;
	float use = step(0.5, u_lightmap.x);
	light = mix(light, 1.0, use);
	bake = mix(vec3(1.0, 1.0, 1.0), bake, use);
	float visibility = 1.0;
	// Convert screen-space derivatives to depth change per shadow UV.
	// Each tap then compares against the same receiver plane.
	vec3 dx = dFdx(v_staticShadow);
	vec3 dy = dFdy(v_staticShadow);
	float det = dx.x * dy.y - dx.y * dy.x;
	if (u_staticShadowParams.x > 0.0 && use < 0.5) {
		vec3 p = v_staticShadow;
		float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
		float range = u_staticShadowEye.w;
		float distanceToEye = length(v_worldPos - u_staticShadowEye.xyz);
		if (distanceToEye < range && edge > u_staticShadowParams.y && p.z > 0.0 && p.z < 1.0) {
			vec2 t = vec2(u_staticShadowParams.y, 0.0);
			float depth = p.z - u_staticShadowParams.z;
			vec2 depthStep = vec2(0.0, 0.0);
			if (abs(det) > 1e-12) {
				depthStep = vec2(dx.z * dy.y - dy.z * dx.y, dx.x * dy.z - dy.x * dx.z)
				          * (t.x / det);
				depthStep = clamp(depthStep, vec2(-0.004, -0.004), vec2(0.004, 0.004));
				// Point sampling reads the texel centre, which can differ
				// from the projected fragment by up to half a texel.
				vec2 texelOffset = vec2(0.5, 0.5) - fract(p.xy / t.x);
				depth += dot(depthStep, texelOffset);
			}
			float visible = step(depth + depthStep.x, texture2D(s_staticShadow, p.xy + t.xy).r)
			              + step(depth - depthStep.x, texture2D(s_staticShadow, p.xy - t.xy).r)
			              + step(depth + depthStep.y, texture2D(s_staticShadow, p.xy + t.yx).r)
			              + step(depth - depthStep.y, texture2D(s_staticShadow, p.xy - t.yx).r);
			visibility -= (1.0 - visible * 0.25) * u_staticShadowParams.x *
			              (1.0 - smoothstep(range * 0.65, range, distanceToEye)) *
			              smoothstep(0.01, 0.10, edge);
		}
	}
	vec3 illumination = vec3(light, light, light) * visibility;
	if (u_sunDir.w > 0.5 && use < 0.5) {
		float direct = max(dot(n, normalize(u_sunDir.xyz)), 0.0);
		illumination = u_sunAmb.rgb + u_sunDiff.rgb * direct * visibility;
	}
	gl_FragColor = vec4(tex.rgb * bake * tint * illumination, outA * u_color.a);
}
