// Cascade atlas sampling (#371), included twice by scene_shadow.sh: for the
// current atlas, and for the atlas of the previous sun direction while the
// shadows crossfade after a sun step. The includer defines SHADOW_ATLAS,
// SHADOW_CASCADES, the three function names and SHADOW_COVERAGE (1: return
// -1 where the atlas does not cover the receiver).
float SHADOW_FIELD_TAP(vec2 uv,vec3 p,vec2 receiverUv,vec2 gradient,float scale,float bias) {
    float depth=p.z+dot(gradient,(uv-receiverUv)/scale)-bias;
    return shadow2D(SHADOW_ATLAS,vec3(uv,depth));
}
float SHADOW_TAP_FIELD(vec3 p,vec2 tile,float scale,float bias,vec2 gradient)
{
    float edge=min(min(p.x,1.0-p.x),min(p.y,1.0-p.y));
    float texel=u_staticShadowParams.y/scale;
    if(edge<=texel*2.0 || p.z<=0.0 || p.z>=1.0) return 1.0;
    vec2 atlasTexel=vec2(u_staticShadowParams.y,u_staticShadowParams.y);
    vec2 uv=p.xy*scale+tile;
    vec2 grid=uv/atlasTexel-vec2(0.5,0.5);
    vec2 base=(floor(grid)+vec2(0.5,0.5))*atlasTexel;
    vec2 phase=fract(grid);
    if(u_shadowFilter.x>0.5) {
        bool wide=u_shadowFilter.x>1.5;
        float result=0.0;
        for(int y=-1;y<=2;++y) for(int x=-1;x<=2;++x) {
            vec2 offset=vec2(float(x),float(y));
            vec2 delta=offset-phase;
            vec2 tent=max(vec2(2.0,2.0)-abs(delta),vec2(0.0,0.0))*0.25;
            vec2 cubic=vec2(shadowCubicWeight(delta.x),shadowCubicWeight(delta.y));
            vec2 weight=wide?tent:mix(cubic,tent,0.25);
            vec2 q=clamp(base+offset*atlasTexel,tile+atlasTexel*0.5,tile+vec2(scale,scale)-atlasTexel*0.5);
            result+=weight.x*weight.y*SHADOW_FIELD_TAP(q,p,uv,gradient,scale,bias);
        }
        return result;
    }
    vec2 weight=u_shadowFilter.x<0.5?smoothstep(vec2(0.15,0.15),vec2(0.85,0.85),phase):phase;
    vec2 a=base,b=base+vec2(atlasTexel.x,0.0),c=base+vec2(0.0,atlasTexel.y),d=base+atlasTexel;
    return mix(mix(SHADOW_FIELD_TAP(a,p,uv,gradient,scale,bias),SHADOW_FIELD_TAP(b,p,uv,gradient,scale,bias),weight.x),
               mix(SHADOW_FIELD_TAP(c,p,uv,gradient,scale,bias),SHADOW_FIELD_TAP(d,p,uv,gradient,scale,bias),weight.x),weight.y);
}
float SHADOW_CASCADE_VISIBILITY(vec3 world, float distanceToEye)
{
    // Evaluate coordinates outside distance branches to keep their derivatives coherent.
    vec4 w=vec4(world-u_shadowOrigin.xyz,1.0);
    vec3 p0=mul(SHADOW_CASCADES[0],w).xyz;
    vec3 p1=mul(SHADOW_CASCADES[1],w).xyz;
    vec3 p2=mul(SHADOW_CASCADES[2],w).xyz;
    vec3 p3=mul(SHADOW_CASCADES[3],w).xyz;
    // Bias represents 2 cm along the light depth, independent of cascade size.
    vec4 bias=vec4(.02,.02,.02,.02)/(2.0*(u_shadowRanges*1.1+vec4(768.0,768.0,768.0,768.0)));
    // Evaluate derivatives before radial selection and correct each texel's
    // reference depth on the receiver plane, including the widest footprint.
    vec2 g0=shadowReceiverGradient(p0),g1=shadowReceiverGradient(p1);
    vec2 g2=shadowReceiverGradient(p2),g3=shadowReceiverGradient(p3);
    // A pixel quad that straddles two surfaces (pole on ground, kerb, wall/
    // floor crease) yields an arbitrary slope; applied across the 4x4
    // footprint it compares taps against far-off depths. Bound it to a real
    // receiver slope of 20:1 (about 87 degrees from the light), converted to
    // this cascade's depth/width units: width 2*extent, depth 2*(extent+768).
    vec4 extent=u_shadowRanges*1.1;
    vec4 slopeLimit=vec4(20.0,20.0,20.0,20.0)*extent/(extent+vec4(768.0,768.0,768.0,768.0));
    g0=clampReceiverGradient(g0,slopeLimit.x);g1=clampReceiverGradient(g1,slopeLimit.y);
    g2=clampReceiverGradient(g2,slopeLimit.z);g3=clampReceiverGradient(g3,slopeLimit.w);
    // Select only the current and adjacent level; do not sample every tile.
    float s1=step(u_shadowRanges.x,distanceToEye);
    float s2=step(u_shadowRanges.y,distanceToEye);
    float s3=step(u_shadowRanges.z,distanceToEye);
    vec3 primary=mix(p0,p1,s1);
    primary=mix(primary,p2,s2);primary=mix(primary,p3,s3);
    vec3 adjacent=mix(p1,p2,s1);adjacent=mix(adjacent,p3,s2);
    vec2 tile=mix(vec2(0.0,0.0),vec2(.5,0.0),s1);
    tile=mix(tile,vec2(0.0,.5),s2);tile=mix(tile,vec2(.5,.5),s3);
    vec2 nextTile=mix(vec2(.5,0.0),vec2(0.0,.5),s1);
    nextTile=mix(nextTile,vec2(.5,.5),s2);
#if SHADOW_COVERAGE
    {
        float primaryEdge=min(min(primary.x,1.0-primary.x),min(primary.y,1.0-primary.y));
        if(primaryEdge<=u_staticShadowParams.y*4.0 || primary.z<=0.0 || primary.z>=1.0) return -1.0;
    }
#endif
    float primaryBias=mix(bias.x,bias.y,s1);
    primaryBias=mix(primaryBias,bias.z,s2);primaryBias=mix(primaryBias,bias.w,s3);
    float nextBias=mix(bias.y,bias.z,s1);nextBias=mix(nextBias,bias.w,s2);
    float radius=mix(u_shadowRanges.x,u_shadowRanges.y,s1);
    radius=mix(radius,u_shadowRanges.z,s2);
    float blend=smoothstep(radius*.85,radius,distanceToEye)*(1.0-s3);
    vec2 gradient=mix(g0,g1,s1);gradient=mix(gradient,g2,s2);gradient=mix(gradient,g3,s3);
    vec2 nextGradient=mix(g1,g2,s1);nextGradient=mix(nextGradient,g3,s2);
    float a=SHADOW_TAP_FIELD(primary,tile,.5,primaryBias,gradient);
    float result=a;
    if(blend>0.0) result=mix(a,SHADOW_TAP_FIELD(adjacent,nextTile,.5,nextBias,nextGradient),blend);
    return mix(result,1.0,smoothstep(u_shadowRanges.w*.9,u_shadowRanges.w,distanceToEye));
}
