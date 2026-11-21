// Main-view AO attenuates only indirect diffuse light, never lamps/emission/UI.
SAMPLER2D(s_aoField,9);
SAMPLER2D(s_aoDepth,10);
uniform vec4 u_aoSampling; // strength (zero disables), inverse screen size
uniform vec4 u_aoFieldSize; // inverse AO size, radius metres
uniform vec4 u_aoProjection; // projection A/B, homogeneous depth, bottom-left
float aoViewDepth(float depth) {
    float ndc=u_aoProjection.z>0.5?depth*2.0-1.0:depth;
    return u_aoProjection.y/(ndc-u_aoProjection.x);
}
float ambientVisibility(vec2 pixel) {
    if(u_aoSampling.x<=0.0) return 1.0;
    vec2 uv=pixel*u_aoSampling.yz;
    if(any(lessThan(uv,vec2(0.0,0.0))) || any(greaterThan(uv,vec2(1.0,1.0)))) return 1.0;
    float depth=texture2DLod(s_aoDepth,uv,0.0).r;
    if(depth<=0.0) return 1.0;
    float z=aoViewDepth(depth);
    vec2 grid=uv/u_aoFieldSize.xy-vec2(0.5,0.5);
    vec2 base=(floor(grid)+vec2(0.5,0.5))*u_aoFieldSize.xy;
    vec2 f=fract(grid);float sum=0.0,weights=0.0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        vec2 q=clamp(base+vec2(float(x),float(y))*u_aoFieldSize.xy,vec2(0.0,0.0),vec2(1.0,1.0));
        // The depth texel this AO texel was computed for (fs_ao.sc aoFullCentre,
        // #274), not whichever neighbour point sampling picks at the boundary.
        vec2 dq=(floor(q/u_aoSampling.yz-vec2(0.25,0.25))+vec2(0.5,0.5))*u_aoSampling.yz;
        float d=texture2DLod(s_aoDepth,dq,0.0).r;
        if(d<=0.0) continue;
        float w=(x==0?1.0-f.x:f.x)*(y==0?1.0-f.y:f.y);
        w*=exp2(-abs(aoViewDepth(d)-z)/max(u_aoFieldSize.z*0.08,0.02));
        sum+=texture2DLod(s_aoField,q,0.0).r*w;weights+=w;
    }
    float visibility=weights>0.0001?sum/weights:1.0;
    return pow(clamp(visibility,0.03,1.0),u_aoSampling.x);
}
