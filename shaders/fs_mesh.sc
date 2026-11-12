$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_lightmap, 1);
SAMPLER2D(s_meshMix, 2);
SAMPLER2DSHADOW(s_staticShadow, 3);
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
uniform vec4 u_cloudWind;
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
// Diffuse and mix maps. scale 0,0 means the draw did not set a transform.
uniform vec4 u_meshBaseUv;
uniform vec4 u_meshMixUv;
// rgb: horizon colour. a > 0.5 fades world meshes between u_fogRange.xy.
uniform vec4 u_fogColor;
uniform vec4 u_fogRange;
uniform vec4 u_fogEye;

vec2 uvSet(float channel, vec2 uv0, vec2 uv1, vec2 uv2)
{
	if (channel > 1.5) return uv2;
	if (channel > 0.5) return uv1;
	return uv0;
}

vec2 mapUv(vec2 uv, vec4 xf)
{
	vec2 scale = xf.xy;
	if (scale.x == 0.0 && scale.y == 0.0) scale = vec2(1.0, 1.0);
	return uv * scale + xf.zw;
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

// Perez luminance distribution, as used by bgfx example 36-sky.
float daylightDistribution(float cosZenith, float cosSun)
{
	const float turbidity = 2.4;
	float a = 0.1787 * turbidity - 1.4630;
	float b = -0.3554 * turbidity + 0.4275;
	float c = -0.0227 * turbidity + 5.3251;
	float d = 0.1206 * turbidity - 2.5771;
	float e = -0.0670 * turbidity + 0.3703;
	float angle = acos(clamp(cosSun, -1.0, 1.0));
	return (1.0 + a * exp(b / max(cosZenith, 0.04))) *
	       (1.0 + c * exp(d * angle) + e * cosSun * cosSun);
}

float cloudDensity(vec3 p)
{
	vec3 samplePos = (p - vec3(u_cloudWind.x, 0.0, u_cloudWind.y)) * vec3(0.0014, 0.0025, 0.0014);
	float shape = cloudNoise(samplePos) * 0.72 + cloudNoise(samplePos * 2.7) * 0.28;
	return smoothstep(0.47, 0.68, shape) * sin(3.14159 * saturate((p.y - 650.0) / 480.0));
}

// Two grids whose scales are not multiples, with a drift that leaves the cell,
// so the points do not sit on rows.
float starField(vec2 sphere, vec2 scale, float seed)
{
	vec2 grid = sphere * scale;
	vec2 cell = floor(grid);
	vec2 jitter = vec2(cloudHash(vec3(cell, seed + 3.1)), cloudHash(vec3(cell, seed + 8.7)));
	vec2 drift = vec2(cloudHash(vec3(cell, seed + 17.0)),
	                  cloudHash(vec3(cell, seed + 29.0))) - vec2(0.5, 0.5);
	float dist = length(fract(grid) - (jitter + drift * 1.15));
	float size = mix(0.012, 0.07, cloudHash(vec3(cell, seed + 4.0)));
	float spark = 1.0 - smoothstep(size * 0.2, size, dist);
	float bright = mix(0.2, 1.0, cloudHash(vec3(cell, seed + 13.0)));
	return spark * step(0.994, cloudHash(vec3(cell, seed))) * bright;
}

vec3 renderSky(vec3 ray, vec2 pixel)
{
	vec3 sun = u_sunDir.w > 0.5 ? normalize(u_sunDir.xyz) : normalize(vec3(0.3, 0.8, 0.2));
	// Elevation, not the sun colour. The colour is already multiplied by a
	// short daylight ramp, which skipped the orange band.
	float day = smoothstep(-0.04, 0.42, sun.y);
	float twilight = smoothstep(-0.16, 0.02, sun.y) * (1.0 - smoothstep(0.04, 0.38, sun.y));
	vec3 horizon = mix(vec3(0.07, 0.09, 0.14), vec3(0.57, 0.73, 0.86), day);
	horizon = mix(horizon, vec3(0.95, 0.42, 0.14), twilight);
	vec2 sunFlat = sun.xz;
	vec2 rayFlat = ray.xz;
	float sunLen = length(sunFlat);
	float rayLen = length(rayFlat);
	float towardSun = sunLen > 0.001 && rayLen > 0.001
	    ? saturate(dot(rayFlat / rayLen, sunFlat / sunLen)) : 0.0;
	float band = twilight * (1.0 - smoothstep(0.0, 0.34, ray.y)) * towardSun;
	horizon = mix(horizon, vec3(1.0, 0.36, 0.08), band * 0.9);
	vec3 zenith = mix(vec3(0.01, 0.02, 0.05), vec3(0.12, 0.33, 0.65), day);
	zenith = mix(zenith, vec3(0.28, 0.16, 0.22), twilight * 0.45);
	vec3 sky = mix(horizon, zenith, sqrt(saturate(ray.y)));
	float luminance = daylightDistribution(ray.y, dot(ray, sun)) /
	                  max(daylightDistribution(1.0, sun.y), 0.05);
	sky *= mix(1.0, clamp(sqrt(luminance), 0.65, 1.75), day);
	vec2 sphere = vec2(atan2(ray.z, ray.x), asin(clamp(ray.y, -1.0, 1.0)));
	float stars = starField(sphere, vec2(210.0, 97.0), 61.0)
	            + starField(sphere, vec2(347.0, 151.0), 19.0);
	stars *= (1.0 - smoothstep(-0.04, 0.22, sun.y)) * smoothstep(0.02, 0.16, ray.y);
	sky += vec3(0.73, 0.82, 1.0) * stars;
	float alignment = dot(ray, sun);
	float disc = smoothstep(0.99990, 0.99996, alignment);
	float glow = pow(saturate(alignment), 96.0);
	sky += u_sunDiff.rgb * (disc * 4.0 + glow * 0.45) * smoothstep(-0.04, 0.16, sun.y);
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
	vec4 tex = texture2D(s_texColor, mapUv(uvSet(u_lightmap.y, v_texcoord0, v_texcoord1, v_texcoord2), u_meshBaseUv));
	if (u_lightmap.w > 0.5) {
		vec4 layer = texture2D(s_meshMix, mapUv(uvSet(u_lightmap.w - 1.0, v_texcoord0, v_texcoord1, v_texcoord2), u_meshMixUv));
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
		// ColorMixTex. A wall atlas is a flat field plus the courses in alpha.
		// Its color channel also has a black seam, and that seam repeats as a
		// dark band across the facade. A chromatic texel is a sign or a poster
		// and still mixes over the wall.
		float chroma = max(tex.r, max(tex.g, tex.b)) - min(tex.r, min(tex.g, tex.b));
		vec3 detail = mix(vec3(1.0, 1.0, 1.0), tex.rgb, step(0.08, chroma));
		tex.rgb = mix(tint, detail, tex.a);
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
	if (u_staticShadowParams.x > 0.0 && use < 0.5) {
		vec3 p = v_staticShadow;
		float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
		float range = u_staticShadowEye.w;
		float distanceToEye = length(v_worldPos - u_staticShadowEye.xyz);
		if (distanceToEye < range && edge > u_staticShadowParams.y && p.z > 0.0 && p.z < 1.0) {
			float texel = u_staticShadowParams.y;
			float slope = max(abs(dFdx(v_staticShadow.z)), abs(dFdy(v_staticShadow.z)));
			float depth = p.z - min(u_staticShadowParams.z + slope, 0.0008);
			// Hardware 2x2 compare. A point-sampled disk drew the shadow
			// map's own texels as stripes on the garage apron.
			vec2 o = vec2(texel * 1.5, texel * 0.5);
			float visible = (
				shadow2D(s_staticShadow, vec3(p.xy + vec2(-o.x, -o.y), depth)) +
				shadow2D(s_staticShadow, vec3(p.xy + vec2( o.y, -o.x), depth)) +
				shadow2D(s_staticShadow, vec3(p.xy + vec2( o.x,  o.y), depth)) +
				shadow2D(s_staticShadow, vec3(p.xy + vec2(-o.y,  o.x), depth))
			) * 0.25;
			visibility -= (1.0 - visible) * u_staticShadowParams.x *
			              (1.0 - smoothstep(range * 0.2, range, distanceToEye)) *
			              smoothstep(0.0, 0.22, edge);
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
		illumination += vec3(1.0, 0.94, 0.80) * (fade * fade * facing * 3.0 / (1.0 + d2))
		              * mix(0.35, 1.0, visibility);
	}
	vec3 shaded = tex.rgb * bake * tint * v_tint.rgb * illumination;
	// Sky and OSD set u_alphaTest.y. Their fragments stay sharp.
	if (u_fogColor.a > 0.5 && u_alphaTest.y < 0.5) {
		float span = max(u_fogRange.y - u_fogRange.x, 1.0);
		float t = saturate((length(v_worldPos - u_fogEye.xyz) - u_fogRange.x) / span);
		t = t * t * (3.0 - 2.0 * t);
		shaded = mix(shaded, u_fogColor.rgb, t);
	}
	gl_FragColor = vec4(shaded, outA * u_color.a);
}
