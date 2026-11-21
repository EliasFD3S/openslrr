$input a_position, a_texcoord0, a_texcoord1, a_texcoord2
$output v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint
#include <bgfx_shader.sh>
void main()
{
    // Keep model translation (including floating-origin rebasing) separate
    // from light rotation. Combining MVP first cancels large map coordinates
    // after rotation and loses sub-texel precision as the sun moves.
    vec4 relativeWorld=mul(u_model[0],vec4(a_position,1.0));
    gl_Position=mul(u_viewProj,relativeWorld);
    v_normal=vec3(0.0,0.0,0.0);
    v_texcoord0=a_texcoord0;v_texcoord1=a_texcoord1;v_texcoord2=a_texcoord2;
    v_color0=vec4(1.0,1.0,1.0,1.0);v_tint=v_color0;
    v_staticShadow=vec3(0.0,0.0,0.0);v_worldPos=relativeWorld.xyz;
}
