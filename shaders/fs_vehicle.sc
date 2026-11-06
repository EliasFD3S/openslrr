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
uniform vec4 u_sunDiff; // rgb, set
uniform vec4 u_sunAmb;
uniform vec4 u_sunSpec;
uniform vec4 u_sunDir; // render-space toward the light, w = present
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
    // Toward the scene light. Rendered vertices flip Z, so the CPU already
    // flipped it. Without a light resource, the stand-in is (3, 5, 4).
    vec3 lightDir = normalize(mul(u_view, vec4(normalize(
        u_sunDir.w > 0.5 ? u_sunDir.xyz : vec3(3.0, 5.0, -4.0)), 0.0)).xyz);
    float ndl = max(dot(n, lightDir), 0.0);
    float shade = 0.45+0.55*ndl;
    vec3 illum = u_sunDiff.a > 0.5
        ? (u_sunAmb.rgb + u_sunDiff.rgb*ndl)
        : vec3(shade, shade, shade);
    vec2 raw = channelUv(u_lightmap.z, v_texcoord0, v_texcoord1, v_texcoord2);
    vec2 luv = vec2(((raw.x - 0.5) - u_lightmapUv.z) * u_lightmapUv.x + 0.5,
                    0.5 - ((0.5 - raw.y) - u_lightmapUv.w) * u_lightmapUv.y);
    vec3 bake = texture2D(s_vehicleBake, luv).rgb;
    float use = step(0.5, u_lightmap.x);
    illum = mix(illum, vec3(1.0, 1.0, 1.0), use);
    bake = mix(vec3(1.0, 1.0, 1.0), bake, use);
    vec3 color = albedo*illum*bake;
    vec3 viewDir = normalize(-v_viewPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float ndh = max(dot(n, halfDir), 0.0);
    float spec = pow(ndh, max(u_vehicleSpec.a, 1.0))*ndl;
    float sheen = pow(ndh, 8.0);
    color += u_vehicleSpec.rgb*(spec+sheen*0.35)*(1.0-use);
    vec2 envUv = channelUv(u_vehicleChannels.z, v_texcoord0, v_texcoord1, v_texcoord2);
    if (u_vehicleChannels.w > 0.5) {
        vec3 ray = reflect(normalize(v_viewPos), n);
        // Stock view-space spheremap with camera heading compensation.
        vec3 look = mtxGetRow(u_view, 2).xyz;
        float yaw = atan2(look.x, look.z);
        envUv = vec2(ray.x*0.5-yaw/3.1415927, -ray.y*0.5+0.5);
    }
    vec3 environment = texture2D(s_vehicleEnv, envUv*u_vehicleEnvUv.xy+u_vehicleEnvUv.zw).rgb;
    float weight = u_vehicleLayers.y;
    if (u_vehicleLayers.z > 0.5) weight *= 1.0-detail.a;
    float fres = pow(1.0-max(dot(n, viewDir), 0.0), 3.0);
    // ColorMixTex. Texture alpha reveals colour 0. A transparent group
    // drops texels at or under alpha ref 4, so a grille's holes stay open.
    if (u_vehicleLayers.w > 3.5) {
        vec3 mixed = mix(u_vehicleDiffuse.rgb, base.rgb, base.a);
        float alpha = base.a*u_vehicleDiffuse.a;
        if (u_lightmap.w > 0.5 && alpha < 4.0/255.0) discard;
        vec3 pane = mixed*illum*bake;
        pane += u_vehicleSpec.rgb*spec;
        if (u_vehicleLayers.w > 4.5) pane += environment*weight;
        gl_FragColor = vec4(pane, alpha);
        return;
    }
    // Stock glass is two passes. The lit tint blends by the material
    // alpha. The spheremap times its strength, plus the specular, is added.
    // u_sunDiff.a marks a scene light (setLight or a directional resource).
    if (u_vehicleLayers.w > 2.5) {
        // The mesh is two-sided, so both windings blend. Stock culls one.
        // Two full alphas of this near-black tint stack into a black pane.
        // 1-sqrt(1-a) twice transmits the same as one authored alpha.
        float alpha = 1.0-sqrt(max(1.0-u_vehicleDiffuse.a, 0.0));
        vec3 lit = u_vehicleDiffuse.rgb*illum;
        // A headlight cover names an empty spheremap and borrows the sky.
        // Cabin glass keeps its strength, plus a graze, so it is not a hole.
        float envK = u_vehicleChannels.w > 1.5 ? 0.4 : (1.0+fres*0.5);
        vec3 shine = environment*weight*envK*0.5 + u_vehicleSpec.rgb*spec*0.5;
        gl_FragColor = vec4(lit*alpha + shine, alpha);
        return;
    } else if (u_vehicleLayers.w > 1.5) {
        // Stock chrome: lit colour + env*strength + specular. The 0.5 is the
        // garage light's specular when no scene light has been read yet.
        vec3 lit = u_vehicleDiffuse.rgb*(u_sunDiff.a > 0.5
            ? illum
            : (vec3(0.347, 0.347, 0.347)+vec3(0.919, 0.958, 1.0)*ndl));
        color = lit + environment*weight;
        vec3 sp = u_sunSpec.a > 0.5 ? u_sunSpec.rgb : vec3(0.5, 0.5, 0.5);
        color += u_vehicleSpec.rgb*pow(ndh, max(u_vehicleSpec.a, 1.0))*sp*step(0.001, ndl);
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
