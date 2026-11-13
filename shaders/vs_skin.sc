$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, a_color0
$output v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint

#include <bgfx_shader.sh>

uniform mat4 u_staticShadowMtx;
uniform mat4 u_skinBones[48];

int boneIndex(float channel)
{
	int index = int(channel * 255.0 + 0.5);
	if (index < 0) index = 0;
	if (index > 47) index = 47;
	return index;
}

vec4 skinVec(vec4 rest, vec4 weight, vec4 index)
{
	float sum = weight.x + weight.y + weight.z + weight.w;
	if (sum < 0.0001) return rest;
	int i0 = boneIndex(index.x);
	int i1 = boneIndex(index.y);
	int i2 = boneIndex(index.z);
	int i3 = boneIndex(index.w);
	vec4 skinned = vec4(0.0, 0.0, 0.0, 0.0);
	skinned += weight.x * mul(u_skinBones[i0], rest);
	skinned += weight.y * mul(u_skinBones[i1], rest);
	skinned += weight.z * mul(u_skinBones[i2], rest);
	skinned += weight.w * mul(u_skinBones[i3], rest);
	skinned /= sum;
	return skinned;
}

void main()
{
	vec4 weight = vec4(a_texcoord1.x, a_texcoord1.y, a_texcoord2.x, a_texcoord2.y);
	vec4 position = skinVec(vec4(a_position, 1.0), weight, a_color0);
	position.w = 1.0;
	vec3 normal = skinVec(vec4(a_normal, 0.0), weight, a_color0).xyz;
	gl_Position = mul(u_modelViewProj, position);
	v_normal = mul(u_model[0], vec4(normal, 0.0)).xyz;
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = vec2(0.0, 0.0);
	v_texcoord2 = vec2(0.0, 0.0);
	v_color0 = vec4(1.0, 1.0, 1.0, 1.0);
	v_tint = vec4(1.0, 1.0, 1.0, 1.0);
	vec4 world = mul(u_model[0], position);
	v_staticShadow = mul(u_staticShadowMtx, world).xyz;
	v_worldPos = world.xyz;
}
