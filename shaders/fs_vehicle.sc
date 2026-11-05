$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_viewPos
#include <bgfx_shader.sh>
SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_vehicleMix, 1);
SAMPLER2D(s_vehicleEnv, 2);
SAMPLER2D(s_vehicleBake, 3);
uniform vec4 u_lightmap;
uniform vec4 u_lightmapUv;
uniform vec4 u_vehicleLayers; // mix enabled, reflection weight, complemented alpha, painted
uniform vec4 u_vehicleChannels; // base, mix, reflection, generated reflection
uniform vec4 u_vehicleBaseUv;
uniform vec4 u_vehicleMixUv;
uniform vec4 u_vehicleEnvUv;
uniform vec4 u_vehicleDiffuse;
uniform vec4 u_vehicleSpec; // rgb, shininess
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
    // Same sun as the scene light: (3, 5, 4) in game space. Rendered
    // vertices flip Z, so the direction does too.
    vec3 lightDir = normalize(mul(u_view, vec4(normalize(vec3(3.0, 5.0, -4.0)), 0.0)).xyz);
    float ndl = max(dot(n, lightDir), 0.0);
    float light = 0.45+0.55*ndl;
    vec2 raw = channelUv(u_lightmap.z, v_texcoord0, v_texcoord1, v_texcoord2);
    vec2 luv = vec2(((raw.x - 0.5) - u_lightmapUv.z) * u_lightmapUv.x + 0.5,
                    0.5 - ((0.5 - raw.y) - u_lightmapUv.w) * u_lightmapUv.y);
    vec3 bake = texture2D(s_vehicleBake, luv).rgb;
    float use = step(0.5, u_lightmap.x);
    light = mix(light, 1.0, use);
    bake = mix(vec3(1.0, 1.0, 1.0), bake, use);
    vec3 color = albedo*light*bake;
    vec3 viewDir = normalize(-v_viewPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float ndh = max(dot(n, halfDir), 0.0);
    float spec = pow(ndh, max(u_vehicleSpec.a, 1.0))*ndl;
    float sheen = pow(ndh, 8.0);
    color += u_vehicleSpec.rgb*(spec+sheen*0.35)*(1.0-use);
    vec2 envUv = channelUv(u_vehicleChannels.z, v_texcoord0, v_texcoord1, v_texcoord2);
    if (u_vehicleChannels.w > 0.5) {
        vec3 ray = reflect(normalize(v_viewPos), n);
        float yaw = atan2(u_view[0][2], u_view[2][2]);
        envUv = vec2(ray.x*0.5-yaw/6.2831853, -ray.y*0.5+0.5);
    }
    vec3 environment = texture2D(s_vehicleEnv, envUv*u_vehicleEnvUv.xy+u_vehicleEnvUv.zw).rgb;
    float weight = u_vehicleLayers.y;
    if (u_vehicleLayers.z > 0.5) weight *= 1.0-detail.a;
    float fres = pow(1.0-max(dot(n, viewDir), 0.0), 3.0);
    // Glass stays a dark tint. The spheremap slides across the curve,
    // which is the warped highlight on the sail panel.
    if (u_vehicleLayers.w > 2.5) {
        color = albedo*light*0.4;
        color += environment*(0.45+1.4*fres);
        color += u_vehicleSpec.rgb*spec;
    } else if (u_vehicleLayers.w > 1.5) {
        // Stock chrome: lit colour + env*strength + specular. The garage
        // light is diffuse (0.92, 0.96, 1), ambient 0.35, specular 0.5.
        vec3 lit = u_vehicleDiffuse.rgb*(vec3(0.347, 0.347, 0.347)+vec3(0.919, 0.958, 1.0)*ndl);
        color = lit + environment*weight;
        color += u_vehicleSpec.rgb*pow(ndh, max(u_vehicleSpec.a, 1.0))*0.5*step(0.001, ndl);
    } else if (u_vehicleLayers.w > 0.5) {
        // Painted panels keep their color and gain the room on top.
        color += environment*weight*(0.9+fres);
    } else {
        // Half the authored strength, added on the metal color. The full
        // weight paints an exhaust pipe with the room and it reads white.
        color += environment*weight*0.5;
    }
    gl_FragColor = vec4(color, base.a*u_vehicleDiffuse.a);
}
