$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_viewPos
#include <bgfx_shader.sh>
SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_vehicleMix, 1);
SAMPLER2D(s_vehicleEnv, 2);
uniform vec4 u_vehicleLayers; // mix enabled, reflection weight, complemented alpha, painted
uniform vec4 u_vehicleChannels; // base, mix, reflection, generated reflection
uniform vec4 u_vehicleBaseUv;
uniform vec4 u_vehicleMixUv;
uniform vec4 u_vehicleEnvUv;
uniform vec4 u_vehicleDiffuse;
vec2 channelUv(float channel, vec2 uv0, vec2 uv1, vec2 uv2)
{
    if (channel > 1.5) return uv2;
    if (channel > 0.5) return uv1;
    return uv0;
}
void main()
{
    vec3 n = normalize(v_normal);
    vec2 baseUv = channelUv(u_vehicleChannels.x, v_texcoord0, v_texcoord1, v_texcoord2)*u_vehicleBaseUv.xy+u_vehicleBaseUv.zw;
    vec4 base = texture2D(s_texColor, baseUv);
    vec4 detail = texture2D(s_vehicleMix, channelUv(u_vehicleChannels.y, v_texcoord0, v_texcoord1, v_texcoord2)*u_vehicleMixUv.xy+u_vehicleMixUv.zw);
    vec3 albedo = mix(base.rgb, detail.rgb, detail.a*u_vehicleLayers.x)*u_vehicleDiffuse.rgb;
    vec3 lightDir = mul(u_view, vec4(normalize(vec3(0.35, 0.85, 0.45)), 0.0)).xyz;
    float light = 0.35+0.65*abs(dot(n, lightDir));
    vec3 color = albedo*light;
    vec2 envUv = channelUv(u_vehicleChannels.z, v_texcoord0, v_texcoord1, v_texcoord2);
    if (u_vehicleChannels.w > 0.5) {
        vec3 ray = reflect(normalize(v_viewPos), n);
        float yaw = atan2(u_view[0][2], u_view[2][2]);
        envUv = vec2(ray.x*0.5-yaw/6.2831853, -ray.y*0.5+0.5);
    }
    vec3 environment = texture2D(s_vehicleEnv, envUv*u_vehicleEnvUv.xy+u_vehicleEnvUv.zw).rgb;
    float weight = u_vehicleLayers.y;
    if (u_vehicleLayers.z > 0.5) weight *= 1.0-detail.a;
    if (u_vehicleLayers.w > 0.5) color = mix(color, environment, weight*0.5);
    else color += environment*weight;
    gl_FragColor = vec4(color, base.a*u_vehicleDiffuse.a);
}
