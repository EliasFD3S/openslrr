$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_lightmap, 1);
SAMPLER2D(s_meshMix, 2);
SAMPLER2D(s_staticShadow, 3);
SAMPLER2D(s_carDepth, 4);
uniform mat4 u_carDepthMtx;
uniform vec4 u_carDepthParams;
// x: strength, y: texel size, z: depth bias.
uniform vec4 u_staticShadowParams;
uniform vec4 u_staticShadowEye;
uniform vec4 u_sunAmb;
uniform vec4 u_sunDiff;
uniform vec4 u_localLight; // world position, radius
uniform vec4 u_sunDir;
// xyz: camera position, w: 0 textured, 1 clear, 2 volumetric.
uniform vec4 u_cloudSky;
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

float cloudHash(vec3 p)
{
	return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float cloudNoise(vec3 p)
{
	vec3 cell = floor(p);
	vec3 f = fract(p);
	f = f*f*(3.0 - 2.0*f);
	float a = mix(cloudHash(cell), cloudHash(cell + vec3(1.0, 0.0, 0.0)), f.x);
	float b = mix(cloudHash(cell + vec3(0.0, 1.0, 0.0)), cloudHash(cell + vec3(1.0, 1.0, 0.0)), f.x);
	float c = mix(cloudHash(cell + vec3(0.0, 0.0, 1.0)), cloudHash(cell + vec3(1.0, 0.0, 1.0)), f.x);
	float d = mix(cloudHash(cell + vec3(0.0, 1.0, 1.0)), cloudHash(cell + vec3(1.0, 1.0, 1.0)), f.x);
	return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}

float cloudDensity(vec3 p)
{
	vec3 samplePos = p * vec3(0.0014, 0.0025, 0.0014);
	float shape = cloudNoise(samplePos) * 0.72 + cloudNoise(samplePos * 2.7) * 0.28;
	return smoothstep(0.47, 0.68, shape) * sin(3.14159 * saturate((p.y - 650.0) / 480.0));
}

vec3 renderSky(vec3 ray, vec2 pixel)
{
	vec3 sun = u_sunDir.w > 0.5 ? normalize(u_sunDir.xyz) : normalize(vec3(0.3, 0.8, 0.2));
	float daylight = saturate(dot(u_sunDiff.rgb, vec3(0.30, 0.59, 0.11)) * 1.6);
	vec3 horizon = mix(vec3(0.07, 0.09, 0.14), vec3(0.57, 0.73, 0.86), daylight);
	vec3 zenith = mix(vec3(0.01, 0.02, 0.05), vec3(0.12, 0.33, 0.65), daylight);
	vec3 sky = mix(horizon, zenith, sqrt(saturate(ray.y)));
	float alignment = dot(ray, sun);
	float disc = smoothstep(0.99990, 0.99996, alignment);
	float glow = pow(saturate(alignment), 256.0);
	sky += u_sunDiff.rgb * (disc * 3.0 + glow * 0.18) * smoothstep(0.0, 0.08, sun.y);
	if (u_cloudSky.w < 1.5 || ray.y < 0.04) return sky;
	float start = max(0.0, (650.0 - u_cloudSky.y) / ray.y);
	float stop = min(12000.0, (1130.0 - u_cloudSky.y) / ray.y);
	// Shorten the march near the horizon and stagger neighbouring rays.
	float stepLength = min(85.0, (stop - start) / 48.0);
	float jitter = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
	if (stepLength <= 0.0) return sky;
	vec3 cloud = vec3(0.0, 0.0, 0.0);
	float transmission = 1.0;
	for (int i = 0; i < 48; ++i) {
		vec3 p = u_cloudSky.xyz + ray * (start + (float(i) + jitter) * stepLength);
		float density = cloudDensity(p);
		float alpha = 1.0 - exp(-density * stepLength * 0.009);
		float sunExposure = 1.0 - cloudDensity(p + sun * 120.0);
		vec3 light = u_sunAmb.rgb + u_sunDiff.rgb * (0.30 + 0.70 * sunExposure);
		cloud += light * alpha * transmission;
		transmission *= 1.0 - alpha;
		if (transmission < 0.015) break;
	}
	return mix(sky, cloud + sky * transmission, smoothstep(0.08, 0.24, ray.y));
}

void main()
{
	if (u_cloudSky.w > 0.5) {
		gl_FragColor = vec4(renderSky(normalize(v_worldPos - u_cloudSky.xyz), gl_FragCoord.xy), 1.0);
		return;
	}
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
	if (u_alphaTest.z > 1.5) {
		float cutoff = (-u_alphaTest.x * 255.0 - 1.0) / 255.0;
		float width = max(fwidth(tex.a), 0.06);
		outA = smoothstep(cutoff, cutoff + width, tex.a);
	}
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
			float texel = u_staticShadowParams.y;
			vec2 grid = p.xy / texel - vec2(0.5, 0.5);
			vec2 blend = fract(grid);
			vec2 uv = (floor(grid) + vec2(0.5, 0.5)) * texel;
			vec2 depthStep = vec2(0.0, 0.0);
			if (abs(det) > 1e-12) {
				depthStep = vec2(dx.z * dy.y - dy.z * dx.y, dx.x * dy.z - dy.x * dx.z)
				          * (texel / det);
				depthStep = clamp(depthStep, vec2(-0.004, -0.004), vec2(0.004, 0.004));
			}
			float depth = p.z - u_staticShadowParams.z - dot(depthStep, blend);
			vec2 x = vec2(texel, 0.0);
			vec2 y = vec2(0.0, texel);
			float a = step(depth, texture2D(s_staticShadow, uv).r);
			float b = step(depth + depthStep.x, texture2D(s_staticShadow, uv + x).r);
			float c = step(depth + depthStep.y, texture2D(s_staticShadow, uv + y).r);
			float d = step(depth + depthStep.x + depthStep.y, texture2D(s_staticShadow, uv + x + y).r);
			float visible = mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
			visibility -= (1.0 - visible) * u_staticShadowParams.x *
			              (1.0 - smoothstep(range * 0.65, range, distanceToEye)) *
			              smoothstep(0.01, 0.10, edge);
		}
	}
	float carVisibility = 1.0;
	if (u_carDepthParams.x > 0.5) {
		vec3 p = mul(u_carDepthMtx, vec4(v_worldPos, 1.0)).xyz;
		float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
		if (edge > u_carDepthParams.y && p.z > 0.0 && p.z < 1.0) {
			vec2 offset = vec2(u_carDepthParams.y, 0.0);
			float depth = p.z - u_carDepthParams.z;
			float samples = step(depth, texture2D(s_carDepth, p.xy + offset.xy).r)
			              + step(depth, texture2D(s_carDepth, p.xy - offset.xy).r)
			              + step(depth, texture2D(s_carDepth, p.xy + offset.yx).r)
			              + step(depth, texture2D(s_carDepth, p.xy - offset.yx).r);
			carVisibility -= (1.0 - samples * 0.25) * smoothstep(0.005, 0.06, edge);
		}
	}
	vec3 illumination = vec3(light, light, light) * visibility * mix(0.55, 1.0, carVisibility);
	if (u_sunDir.w > 0.5 && use < 0.5) {
		float direct = max(dot(n, normalize(u_sunDir.xyz)), 0.0);
		illumination = u_sunAmb.rgb + u_sunDiff.rgb * direct * min(visibility, carVisibility);
	}
	if (u_localLight.w > 0.0) {
		vec3 delta = u_localLight.xyz - v_worldPos;
		float d2 = dot(delta, delta);
		float fade = saturate(1.0 - d2 / (u_localLight.w * u_localLight.w));
		float facing = max(dot(n, delta * inversesqrt(max(d2, 0.001))), 0.0);
		illumination += vec3(1.0, 0.94, 0.80) * (fade * fade * facing * 3.0 / (1.0 + d2));
	}
	gl_FragColor = vec4(tex.rgb * bake * tint * illumination, outA * u_color.a);
}
