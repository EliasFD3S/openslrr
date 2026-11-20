$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_texColor, 0);
uniform vec4 u_color;
// outline width, shadow X/Y (pixels), SDF flag; then inverse atlas dimensions.
uniform vec4 u_fontParams[2];
uniform vec4 u_fontOutline;
uniform vec4 u_fontShadow;

void main()
{
    vec4 sampleColor = texture2D(s_texColor, v_texcoord0);
    if (u_fontParams[0].w < 0.5)
    {
        // Stock/mod bitmap fallback retains its authored colours and effects.
        gl_FragColor = sampleColor * u_color;
        return;
    }
    // The CPU atlas encodes zero distance at 128, with 16 steps per pixel.
    float distance = (sampleColor.a * 255.0 - 128.0) / 16.0;
    float aa = max(fwidth(distance) * 0.5, 0.0625);
    float fill = smoothstep(-aa, aa, distance);
    float outer = smoothstep(-aa, aa, distance + u_fontParams[0].x);
    float outline = max(outer - fill, 0.0) * u_fontOutline.a;
    vec2 shadowUv = v_texcoord0 - u_fontParams[0].yz * u_fontParams[1].xy;
    float shadowDistance = (texture2D(s_texColor, shadowUv).a * 255.0 - 128.0) / 16.0;
    float shadow = smoothstep(-aa, aa, shadowDistance) * u_fontShadow.a;
    shadow *= 1.0 - fill - outline;
    float alpha = fill + outline + shadow;
    vec3 premultiplied = u_color.rgb * fill + u_fontOutline.rgb * outline
                      + u_fontShadow.rgb * shadow;
    // The OSD uses straight-alpha blending; text opacity also fades effects.
    gl_FragColor = vec4(premultiplied / max(alpha, 0.00001), alpha * u_color.a);
}
