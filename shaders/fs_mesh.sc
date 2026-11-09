$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_lightmap, 1);
SAMPLER2D(s_meshMix, 2);
// x: cutout (discard a<0.5)
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
	if (u_alphaTest.x > 0.5 && tex.a < 0.5)
		discard;
	float light = (u_alphaTest.y > 0.75) ? 1.0 : ((u_alphaTest.y > 0.25) ? 0.65 + 0.35 * ndl : ndl);
	float outA = (u_alphaTest.z > 0.5) ? tex.a : 1.0;
	vec3 tint = u_color.rgb;
	if (u_alphaTest.w > 1.5) {
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
	gl_FragColor = vec4(tex.rgb * bake * light * tint, outA * u_color.a);
}
