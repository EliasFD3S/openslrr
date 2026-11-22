$input a_position, a_normal, a_texcoord0, a_texcoord1, a_texcoord2, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_viewPos, v_worldPos, v_staticShadow, v_tint
#include <bgfx_shader.sh>
uniform mat4 u_staticShadowMtx;
uniform vec4 u_vehicleDiffuse;
uniform vec4 u_dynamicReflection; // z: per-instance colour multiplies (1) or replaces (2) diffuse
void main()
{
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    vec4 world = mul(model, vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, world);
    vec3 worldNormal = mul(model, vec4(a_normal, 0.0)).xyz;
    v_normal = mul(u_view, vec4(worldNormal, 0.0)).xyz;
    v_worldPos = world.xyz;
    v_viewPos = mul(u_view, world).xyz;
    float normalLength = dot(worldNormal, worldNormal);
    if (normalLength > 1e-8)
        world.xyz += worldNormal * (0.05 / sqrt(normalLength));
    v_staticShadow = mul(u_staticShadowMtx, world).xyz;
    v_texcoord0 = a_texcoord0;
    v_texcoord1 = a_texcoord1;
    v_texcoord2 = a_texcoord2;
    v_tint = u_vehicleDiffuse;
    // White is the create-string's "no paint" sentinel, not a replacement.
    if (i_data4.w > 0.5 && u_dynamicReflection.z > 0.5)
        v_tint.rgb = u_dynamicReflection.z > 1.5 ? i_data4.rgb : v_tint.rgb * i_data4.rgb;
}
