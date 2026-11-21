$input v_normal, v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_staticShadow, v_worldPos, v_tint
// bgfx links a program only when this $input list equals the vertex shader's
// $output list (vs_mesh, vs_mesh_inst). Keep the two in step.

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
// y: alpha reference, z: diffuse UV channel.
uniform vec4 u_staticShadowParams;
SAMPLER2D(s_aperture, 1);
uniform vec4 u_apertureParams; // triangle count, coplanar tolerance in metres
uniform mat4 u_apertureMatrices[32];
uniform vec4 u_apertureUv[32];
uniform vec4 u_apertureEdge[32];

void main()
{
    for (int i=0;i<32;++i) {
        if (float(i)>=u_apertureParams.x) break;
        vec3 b=mul(u_apertureMatrices[i],vec4(v_worldPos,1.0)).xyz;
        if(abs(b.z)<=u_apertureParams.y && b.x>=-0.0001 && b.y>=-0.0001 && b.x+b.y<=1.0001) {
            vec2 uv=u_apertureUv[i].xy + b.x*u_apertureUv[i].zw + b.y*u_apertureEdge[i].xy;
            if(texture2DLod(s_aperture,uv,0.0).a<=0.5) discard;
        }
    }
	if (u_staticShadowParams.y >= 0.0) {
		vec2 uv = u_staticShadowParams.z > 1.5 ? v_texcoord2
		        : u_staticShadowParams.z > 0.5 ? v_texcoord1 : v_texcoord0;
		if (texture2D(s_texColor, uv).a <= u_staticShadowParams.y)
			discard;
	}
	gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
}
