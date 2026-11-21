uniform vec4 u_shadowFilter; // 0 sharp, 1 soft, 2 very soft
// Projected depth slope on the actual receiver surface. Keep derivatives
// outside varying bounds branches (uniform light enable/count branches are OK).
vec2 receiverGradientFrom(vec3 dx,vec3 dy)
{
    float determinant=dx.x*dy.y-dx.y*dy.x;
    float scale=max(dot(dx.xy,dx.xy),dot(dy.xy,dy.xy));
    if(abs(determinant)>max(scale*0.00001,1e-18))
        return vec2(dy.y*dx.z-dx.y*dy.z,dx.x*dy.z-dy.x*dx.z)/determinant;
    return vec2(0.0,0.0);
}
vec2 shadowReceiverGradient(vec3 p)
{
    vec3 dx=dFdx(p),dy=dFdy(p);
    float determinant=dx.x*dy.y-dx.y*dy.x;
    float scale=max(dot(dx.xy,dx.xy),dot(dy.xy,dy.xy));
    if(abs(determinant)>max(scale*0.00001,1e-18))
        return vec2(dy.y*dx.z-dx.y*dy.z,dx.x*dy.z-dy.x*dx.z)/determinant;
    return vec2(0.0,0.0);
}
vec2 clampReceiverGradient(vec2 gradient,float limit)
{
    float length2=dot(gradient,gradient);
    return length2>limit*limit?gradient*(limit*inversesqrt(length2)):gradient;
}
// Bounded ceiling emitters share one downward projection each, not six cube
// faces per tube. The housing limits the direct source to its lower hemisphere.
uniform vec4 u_fixtureParams; // count, inverse atlas size xy, shadows enabled
// Per-frame light data (#254): a 256 x 3 RGBA32F texture read with texelFetch
// instead of ~550 vec4 of fragment constants re-uploaded with every draw.
// Row 0: lamp position+range [0,64), colour*intensity [64,128), attenuation
// [128,192). Row 1: per shadowed lamp s, scenery tile matrix [4s,4s+4) and
// tile [96+s], car tile matrix [120+4s,124+4s) and tile [216+s]. Row 2:
// fixture position+range [0,12), colour+intensity [12,24), direction+outer
// cosine [24,36), shadow matrix [36+4i,40+4i). Matrices are stored by column.
SAMPLER2D(s_lightData, 11);
// Headlight shadow atlas (#283): 3 x 2 tiles, standard depth.
SAMPLER2DSHADOW(s_headlightShadow, 12);
uniform vec4 u_headlightShadow; // atlas texel size
vec4 lightData(int x,int y) { return texelFetch(s_lightData,ivec2(x,y),0); }
mat4 lightMatrix(int x,int y)
{
    return mtxFromCols(lightData(x,y),lightData(x+1,y),lightData(x+2,y),lightData(x+3,y));
}
float fixtureReceiverTap(vec2 uv,vec3 receiver,vec2 receiverUv,vec2 gradient)
{
    float depth=receiver.z+dot(gradient,(uv-receiverUv)*vec2(4.0,3.0))-0.000005;
    return shadow2D(s_fixtureShadow,vec3(uv,depth));
}
void fixtureLighting(vec3 n,vec3 viewDirection,vec3 world,float power,bool specular,
                     out vec3 diffuse,out vec3 highlight)
{
    diffuse=vec3(0.0,0.0,0.0);highlight=vec3(0.0,0.0,0.0);
    for(int i=0;i<12;++i) {
        if(float(i)>=u_fixtureParams.x) break;
        vec3 delta=lightData(i,2).xyz-world;
        float d2=max(dot(delta,delta),0.001);
        vec3 toward=delta*inversesqrt(d2);
        float cone=smoothstep(lightData(24+i,2).w,0.422618,
                             dot(-toward,lightData(24+i,2).xyz));
        float range=lightData(i,2).w;
        float window=saturate(1.0-pow(d2/(range*range),2.0));
        float visibility=1.0;
        if(u_fixtureParams.w>0.5) {
            vec4 clip=mul(lightMatrix(36+4*i,2),vec4(world+toward*0.015,1.0));
            vec3 p=clip.xyz/max(clip.w,0.0001);
            // Evaluate derivatives before the varying bounds branch. Compare
            // each depth texel against the receiver plane at that texel, not
            // one central depth for an entire grazing-angle PCF footprint.
            vec2 gradient=shadowReceiverGradient(p);
            visibility=0.0;
            if(clip.w>0.0 && all(greaterThan(p,vec3(0.0,0.0,0.0))) &&
               all(lessThan(p,vec3(1.0,1.0,1.0)))) {
                float row=floor(float(i)*0.25);
                vec2 tile=vec2((float(i)-row*4.0)*0.25,row/3.0);
                vec2 uv=tile+p.xy*vec2(0.25,1.0/3.0);
                vec2 texel=u_fixtureParams.yz;
                uv=clamp(uv,tile+texel*1.5,tile+vec2(0.25,1.0/3.0)-texel*1.5);
                // Aligned comparison taps avoid the hardware's hidden PCF
                // neighbours, which cannot have separate receiver depths.
                vec2 grid=uv/texel-vec2(0.5,0.5);
                vec2 base=(floor(grid)+vec2(0.5,0.5))*texel;
                vec2 weight=fract(grid);
                // Sharp retains a narrow, continuous anti-aliased edge.
                if(u_shadowFilter.x<0.5) weight=smoothstep(vec2(0.15,0.15),vec2(0.85,0.85),weight);
                vec2 a=base,b=base+vec2(texel.x,0.0);
                vec2 c=base+vec2(0.0,texel.y),d=base+texel;
                vec2 atlasToLocal=vec2(4.0,3.0);
                vec2 receiverUv=tile+p.xy*vec2(0.25,1.0/3.0);
                float bias=0.000005;
                float da=p.z+dot(gradient,(a-receiverUv)*atlasToLocal)-bias;
                float db=p.z+dot(gradient,(b-receiverUv)*atlasToLocal)-bias;
                float dc=p.z+dot(gradient,(c-receiverUv)*atlasToLocal)-bias;
                float dd=p.z+dot(gradient,(d-receiverUv)*atlasToLocal)-bias;
                visibility=mix(mix(shadow2D(s_fixtureShadow,vec3(a,da)),
                                   shadow2D(s_fixtureShadow,vec3(b,db)),weight.x),
                               mix(shadow2D(s_fixtureShadow,vec3(c,dc)),
                                   shadow2D(s_fixtureShadow,vec3(d,dd)),weight.x),weight.y);
                if(u_shadowFilter.x>1.5) {
                    visibility=0.0;
                    // Fractional tent weights move continuously across texels.
                    for(int fy=-1;fy<=2;++fy) for(int fx=-1;fx<=2;++fx) {
                        vec2 offset=vec2(float(fx),float(fy));
                        vec2 tent=max(vec2(2.0,2.0)-abs(offset-weight),vec2(0.0,0.0));
                        float w=tent.x*tent.y;
                        vec2 tap=clamp(base+offset*texel,tile+texel*0.5,
                                       tile+vec2(0.25,1.0/3.0)-texel*0.5);
                        visibility+=w*fixtureReceiverTap(tap,p,receiverUv,gradient);
                    }
                    visibility*=0.0625;
                }
            }
        }
        vec3 radiance=lightData(12+i,2).rgb*(lightData(12+i,2).w*window*window/
                        max(d2,0.25))*cone*visibility;
        float facing=max(dot(n,toward),0.0);
        diffuse+=radiance*facing;
        if(specular) {
            vec3 h=toward+viewDirection;h*=inversesqrt(max(dot(h,h),0.000001));
            highlight+=radiance*facing*pow(max(dot(n,h),0.0),power);
        }
    }
}

// Scenery lamps (#237): street lamps outside garages, unshadowed and
// omnidirectional, separate from the 12 garage fixtures. Count in x.
uniform vec4 u_lampParams;
// Lamp data in s_lightData row 0 (#254): position+range, rgb*intensity,
// D3D attenuation c, l, q for 1/(c + l*d + q*d*d).
// Shadowed lamps (#237) are the first x entries of the lamp arrays. Each may
// sample a scenery tile (a downward perspective tile of s_fixtureShadow, free
// outside garages) and a car-only tile (a narrow perspective onto the car);
// a strength of 0 in z means no tile.
uniform vec4 u_lampShadowParams;       // count, inverse atlas width/height
// Tile matrices (world -> tile-local uv, depth) and tiles (origin uv,
// strength) in s_lightData row 1 (#254).
float lampShadowTap(vec2 uv,vec3 p,vec2 receiverUv,vec2 gradient,vec2 toLocal)
{
    float depth=p.z+dot(gradient,(uv-receiverUv)*toLocal)-0.000005;
    return shadow2D(s_fixtureShadow,vec3(uv,depth));
}
float lampTileVisibility(mat4 sampleMatrix,vec4 tileInfo,vec2 size,vec3 world,vec3 toward,bool rim)
{
    vec4 clip=mul(sampleMatrix,vec4(world+toward*0.03,1.0));
    vec3 p=clip.xyz/max(clip.w,0.0001);
    // Derivatives before the varying bounds branch, as for the fixtures.
    vec2 gradient=shadowReceiverGradient(p);
    float visibility=1.0;
    if(clip.w>0.0 && all(greaterThan(p,vec3(0.0,0.0,0.0))) &&
       all(lessThan(p,vec3(1.0,1.0,1.0)))) {
        vec2 tile=tileInfo.xy;
        vec2 texel=u_lampShadowParams.yz;
        vec2 toLocal=vec2(1.0,1.0)/size;
        vec2 receiverUv=tile+p.xy*size;
        vec2 uv=clamp(receiverUv,tile+texel*1.5,tile+size-texel*1.5);
        vec2 grid=uv/texel-vec2(0.5,0.5);
        vec2 base=(floor(grid)+vec2(0.5,0.5))*texel;
        vec2 weight=fract(grid);
        if(u_shadowFilter.x<0.5) weight=smoothstep(vec2(0.15,0.15),vec2(0.85,0.85),weight);
        float shadow;
        if(u_shadowFilter.x>1.5) {
            shadow=0.0;
            for(int fy=-1;fy<=2;++fy) for(int fx=-1;fx<=2;++fx) {
                vec2 offset=vec2(float(fx),float(fy));
                vec2 tent=max(vec2(2.0,2.0)-abs(offset-weight),vec2(0.0,0.0));
                vec2 tap=clamp(base+offset*texel,tile+texel*0.5,tile+size-texel*0.5);
                shadow+=tent.x*tent.y*lampShadowTap(tap,p,receiverUv,gradient,toLocal);
            }
            shadow*=0.0625;
        } else {
            shadow=mix(mix(lampShadowTap(base,p,receiverUv,gradient,toLocal),
                           lampShadowTap(base+vec2(texel.x,0.0),p,receiverUv,gradient,toLocal),weight.x),
                       mix(lampShadowTap(base+vec2(0.0,texel.y),p,receiverUv,gradient,toLocal),
                           lampShadowTap(base+texel,p,receiverUv,gradient,toLocal),weight.x),weight.y);
        }
        // The tile covers a 150-degree cone; fade to unshadowed at its rim
        // instead of a hard edge (the light there is about a tenth of the peak).
        float fade=rim?1.0-smoothstep(0.75,0.98,length(p.xy*2.0-vec2(1.0,1.0))):1.0;
        visibility=mix(1.0,shadow,fade);
    }
    return mix(1.0,visibility,tileInfo.z);
}
// Street lamp tiles without screen-space derivatives (#250): the receiver
// depth gradient comes from the world-position derivatives taken once per
// pixel, through the tile projection (quotient rule on clip.xyz / clip.w).
// That lets lampLighting skip lamps out of range or facing away before any
// tile work. The kernel is capped: Very soft uses the 2x2 taps on a two-texel
// grid (4 taps) instead of the 4x4 tent (16 taps); sun cascades unchanged.
float lampTileVisibilityFast(mat4 sampleMatrix,vec4 tileInfo,vec2 size,vec3 world,vec3 toward,
                             bool rim,vec3 dwx,vec3 dwy)
{
    vec4 clip=mul(sampleMatrix,vec4(world+toward*0.03,1.0));
    if(clip.w<=0.0) return 1.0;
    vec3 p=clip.xyz/clip.w;
    if(any(lessThanEqual(p,vec3(0.0,0.0,0.0))) || any(greaterThanEqual(p,vec3(1.0,1.0,1.0))))
        return 1.0;
    vec4 cx=mul(sampleMatrix,vec4(dwx,0.0)),cy=mul(sampleMatrix,vec4(dwy,0.0));
    vec2 gradient=receiverGradientFrom((cx.xyz-p*cx.w)/clip.w,(cy.xyz-p*cy.w)/clip.w);
    vec2 tile=tileInfo.xy;
    vec2 texel=u_lampShadowParams.yz;
    vec2 spacing=u_shadowFilter.x>1.5?texel*2.0:texel;
    vec2 toLocal=vec2(1.0,1.0)/size;
    vec2 receiverUv=tile+p.xy*size;
    vec2 uv=clamp(receiverUv,tile+spacing*1.5,tile+size-spacing*1.5);
    vec2 grid=uv/spacing-vec2(0.5,0.5);
    vec2 base=(floor(grid)+vec2(0.5,0.5))*spacing;
    vec2 weight=fract(grid);
    if(u_shadowFilter.x<0.5) weight=smoothstep(vec2(0.15,0.15),vec2(0.85,0.85),weight);
    float shadow=mix(mix(lampShadowTap(base,p,receiverUv,gradient,toLocal),
                         lampShadowTap(base+vec2(spacing.x,0.0),p,receiverUv,gradient,toLocal),weight.x),
                     mix(lampShadowTap(base+vec2(0.0,spacing.y),p,receiverUv,gradient,toLocal),
                         lampShadowTap(base+spacing,p,receiverUv,gradient,toLocal),weight.x),weight.y);
    float fade=rim?1.0-smoothstep(0.75,0.98,length(p.xy*2.0-vec2(1.0,1.0))):1.0;
    return mix(1.0,mix(1.0,shadow,fade),tileInfo.z);
}
// Car lights (#279) are spot lamps: lightData(192+i,0) holds the beam
// direction and the cosine of the outer half-angle, lightData(128+i,0).w the
// inner one. Omnidirectional lamps (street lamps) store an outer cosine of -2.
float lampCone(int i,vec3 toward)
{
    vec4 spot=lightData(192+i,0);
    if(spot.w<-1.5) return 1.0;
    return smoothstep(spot.w,lightData(128+i,0).w,dot(-toward,spot.xyz));
}
vec3 lampRadiance(int i,vec3 n,vec3 delta,float d2)
{
    float r2=lightData(i,0).w*lightData(i,0).w;
    if(d2>=r2) return vec3(0.0,0.0,0.0);
    d2=max(d2,0.001);
    float inv=inversesqrt(d2);
    float facing=max(dot(n,delta*inv),0.0);
    float window=saturate(1.0-(d2/r2)*(d2/r2));
    vec3 a=lightData(128+i,0).xyz;
    return lightData(64+i,0).rgb*(facing*window*window*lampCone(i,delta*inv)/max(a.x+a.y*d2*inv+a.z*d2,0.25));
}
// Headlight shadow (#283): tile t of the headlight atlas, 2 x 2 compare taps,
// 3 cm normal offset toward the lamp. No derivatives: it runs after the
// per-lamp early-outs.
float headlightVisibility(int t,vec3 world,vec3 toward)
{
    vec4 clip=mul(lightMatrix(96+4*t,2),vec4(world+toward*0.03,1.0));
    if(clip.w<=0.0) return 1.0;
    vec3 p=clip.xyz/clip.w;
    if(any(lessThanEqual(p,vec3(0.0,0.0,0.0))) || any(greaterThanEqual(p,vec3(1.0,1.0,1.0))))
        return 1.0;
    vec4 rect=lightData(120+t,2);
    vec2 uv=rect.xy+p.xy*rect.zw;
    vec2 texel=u_headlightShadow.xy;
    vec2 lo=rect.xy+texel*1.5,hi=rect.xy+rect.zw-texel*1.5;
    float depth=p.z-0.00001;
    float sum=0.0;
    sum+=shadow2D(s_headlightShadow,vec3(clamp(uv+vec2(-0.5,-0.5)*texel,lo,hi),depth));
    sum+=shadow2D(s_headlightShadow,vec3(clamp(uv+vec2( 0.5,-0.5)*texel,lo,hi),depth));
    sum+=shadow2D(s_headlightShadow,vec3(clamp(uv+vec2(-0.5, 0.5)*texel,lo,hi),depth));
    sum+=shadow2D(s_headlightShadow,vec3(clamp(uv+vec2( 0.5, 0.5)*texel,lo,hi),depth));
    return sum*0.25;
}
vec3 lampLighting(vec3 n,vec3 world)
{
    vec3 sum=vec3(0.0,0.0,0.0);
    // Derivatives once, in uniform control flow, before any per-lamp branch.
    vec3 dwx=dFdx(world),dwy=dFdy(world);
    if(u_lampShadowParams.w<0.5) {
    // Shadowed lamps (#250): out of range or facing away gives no light, so
    // they are skipped before the projection and the shadow lookup.
    for(int s=0;s<24;++s) {
        if(float(s)>=u_lampShadowParams.x) break;
        vec3 delta=lightData(s,0).xyz-world;
        float d2=dot(delta,delta);
        float r2=lightData(s,0).w*lightData(s,0).w;
        if(d2>=r2 || dot(n,delta)<=0.0) continue;
        vec3 toward=delta*inversesqrt(max(d2,0.001));
        float visibility=1.0;
        if(lightData(96+s,1).z>0.0)
            visibility*=lampTileVisibilityFast(lightMatrix(4*s,1),lightData(96+s,1),
                                               vec2(0.25,1.0/3.0),world,toward,true,dwx,dwy);
        if(lightData(216+s,1).z>0.0)
            visibility*=lampTileVisibilityFast(lightMatrix(120+4*s,1),lightData(216+s,1),
                                               vec2(0.125,1.0/6.0),world,toward,false,dwx,dwy);
        sum+=lampRadiance(s,n,delta,d2)*visibility;
    }
    } else {
    // Reference path (render_debug lamp_shadow_reference): the #237 loop,
    // screen-space derivatives per lamp and the full kernel.
    for(int s=0;s<24;++s) {
        if(float(s)>=u_lampShadowParams.x) break;
        vec3 delta=lightData(s,0).xyz-world;
        float d2=dot(delta,delta);
        vec3 toward=delta*inversesqrt(max(d2,0.001));
        float visibility=1.0;
        if(lightData(96+s,1).z>0.0)
            visibility*=lampTileVisibility(lightMatrix(4*s,1),lightData(96+s,1),
                                           vec2(0.25,1.0/3.0),world,toward,true);
        if(lightData(216+s,1).z>0.0)
            visibility*=lampTileVisibility(lightMatrix(120+4*s,1),lightData(216+s,1),
                                           vec2(0.125,1.0/6.0),world,toward,false);
        sum+=lampRadiance(s,n,delta,d2)*visibility;
    }
    }
    for(int i=0;i<64;++i) {
        if(float(i)>=u_lampParams.x) break;
        if(float(i)<u_lampShadowParams.x) continue;
        vec3 delta=lightData(i,0).xyz-world;
        float r2=lightData(i,0).w*lightData(i,0).w;
        float d2=dot(delta,delta);
        // Most lamps are far from a given pixel: skip them before any
        // transcendental work.
        if(d2>=r2) continue;
        d2=max(d2,0.001);
        float inv=inversesqrt(d2);
        float facing=max(dot(n,delta*inv),0.0);
        if(facing<=0.0) continue;
        float cone=lampCone(i,delta*inv);
        if(cone<=0.0) continue;
        float window=saturate(1.0-(d2/r2)*(d2/r2));
        vec3 a=lightData(128+i,0).xyz;
        vec4 colour=lightData(64+i,0);
        float visibility=colour.w>0.5?headlightVisibility(int(colour.w-0.5),world,delta*inv):1.0;
        sum+=colour.rgb*(facing*window*window*cone*visibility/max(a.x+a.y*d2*inv+a.z*d2,0.25));
    }
    return sum;
}

// Independent interior daylight field; ambient is never multiplied by it.
uniform mat4 u_interiorSunMtx;
uniform vec4 u_interiorSunDir; // toward sun, atlas texel size
uniform vec4 u_interiorSunColor; // direct radiance, enabled
float sunReceiverTap(vec2 uv,vec3 receiver,vec2 gradient)
{
    float depth=receiver.z+dot(gradient,uv-receiver.xy)-0.000002;
    return shadow2D(s_interiorSun,vec3(uv,depth));
}
float interiorSunVisibility(vec3 world)
{
    if(u_interiorSunColor.w<0.5) return 0.0;
    vec3 p=mul(u_interiorSunMtx,vec4(world,1.0)).xyz;
    vec2 gradient=shadowReceiverGradient(p);
    if(any(lessThanEqual(p,vec3(0.0,0.0,0.0))) || any(greaterThanEqual(p,vec3(1.0,1.0,1.0)))) return 0.0;
    vec2 aperture=texture2D(s_sunAperture,p.xy).rg;
    if(aperture.y<=0.0 || p.z<aperture.x-0.0000005) return 0.0;
    float texel=u_interiorSunDir.w;
    vec2 grid=p.xy/texel-vec2(0.5,0.5);
    vec2 base=(floor(grid)+vec2(0.5,0.5))*texel;
    vec2 weight=fract(grid);
    if(u_shadowFilter.x<0.5) weight=smoothstep(vec2(0.15,0.15),vec2(0.85,0.85),weight);
    vec2 a=clamp(base,vec2(texel*0.5,texel*0.5),vec2(1.0-texel*1.5,1.0-texel*1.5));
    vec2 b=a+vec2(texel,0.0),c=a+vec2(0.0,texel),d=a+vec2(texel,texel);
    // Small residual depth bias; a multi-centimetre constant offsets
    // receivers across the paper-thin ceiling/wall junction and leaks sunlight.
    float bias=0.000002;
    float da=p.z+dot(gradient,a-p.xy)-bias;
    float db=p.z+dot(gradient,b-p.xy)-bias;
    float dc=p.z+dot(gradient,c-p.xy)-bias;
    float dd=p.z+dot(gradient,d-p.xy)-bias;
    float result=mix(mix(shadow2D(s_interiorSun,vec3(a,da)),shadow2D(s_interiorSun,vec3(b,db)),weight.x),
               mix(shadow2D(s_interiorSun,vec3(c,dc)),shadow2D(s_interiorSun,vec3(d,dd)),weight.x),weight.y);
    if(u_shadowFilter.x>1.5) {
        result=0.0;
        for(int fy=-1;fy<=2;++fy) for(int fx=-1;fx<=2;++fx) {
            vec2 offset=vec2(float(fx),float(fy));
            vec2 tent=max(vec2(2.0,2.0)-abs(offset-weight),vec2(0.0,0.0));
            float w=tent.x*tent.y;
            vec2 uv=clamp(base+offset*texel,vec2(texel*0.5,texel*0.5),vec2(1.0-texel*0.5,1.0-texel*0.5));
            result+=w*sunReceiverTap(uv,p,gradient);
        }
        result*=0.0625;
    }
    return aperture.y*result;
}
vec3 interiorSunDiffuse(vec3 normal,vec3 world)
{
    return u_interiorSunColor.rgb*max(dot(normal,normalize(u_interiorSunDir.xyz)),0.0)*interiorSunVisibility(world);
}
// Shared world-space visibility; stages are assigned by each material caller.
// x enabled, y atlas texel, z single-field bias, w sun(0)/local(1).
uniform vec4 u_staticShadowParams;
uniform vec4 u_staticShadowEye;
uniform mat4 u_shadowCascades[4];
uniform vec4 u_shadowOrigin;
uniform vec4 u_shadowRanges; // x=0 selects single/local field, otherwise radial splits.
uniform mat4 u_pointShadowFaces[6];
uniform vec4 u_pointShadowParams; // enabled, inv atlas width/height, bias metres
uniform vec4 u_pointLightColor; // source colour and engine intensity
vec3 localLightRadiance(vec3 delta,float range,bool punctual)
{
    float d2=dot(delta,delta);
    float fade=saturate(1.0-d2/(range*range));
    float attenuation=fade*fade/(1.0+d2);
    if(punctual) {
        float window=saturate(1.0-pow(d2/(range*range),2.0));
        attenuation=window*window/max(d2,0.25);
    }
    return u_pointLightColor.rgb*(u_pointLightColor.w*attenuation);
}
float pointShadowVisibility(vec3 world)
{
    vec3 delta=world-u_shadowOrigin.xyz;
    float distance=length(delta);
    if(distance<0.05) return 1.0;
    vec3 a=abs(delta);
    int face=a.x>=a.y && a.x>=a.z ? (delta.x>=0.0?0:1)
           : a.y>=a.z ? (delta.y>=0.0?2:3) : (delta.z>=0.0?4:5);
    vec3 biased=delta*(max(distance-u_pointShadowParams.w,0.0)/distance);
    vec4 clip=mul(u_pointShadowFaces[face],vec4(biased,1.0));
    if(clip.w<=0.0) return 1.0;
    vec3 p=clip.xyz/clip.w;
    if(any(lessThan(p,vec3(0.0,0.0,0.0))) || any(greaterThan(p,vec3(1.0,1.0,1.0)))) return 1.0;
    vec2 scale=vec2(1.0/3.0,0.5);
    float row=step(2.5,float(face));
    vec2 tile=vec2((float(face)-row*3.0)/3.0,row*0.5);
    vec2 texel=u_pointShadowParams.yz;
    vec2 uv=clamp(tile+p.xy*scale,tile+texel*1.5,tile+scale-texel*1.5);
    if(u_shadowFilter.x<0.5) return shadow2D(s_staticShadow,vec3(uv,p.z));
    vec2 offset=texel*(u_shadowFilter.x>1.5?1.0:0.5);
    return (shadow2D(s_staticShadow,vec3(uv+offset,p.z))+
            shadow2D(s_staticShadow,vec3(uv-offset,p.z))+
            shadow2D(s_staticShadow,vec3(uv+vec2(offset.x,-offset.y),p.z))+
            shadow2D(s_staticShadow,vec3(uv+vec2(-offset.x,offset.y),p.z)))*0.25;
}
float shadowCubicWeight(float distance) {
    float x=abs(distance);
    return x<1.0?(4.0-6.0*x*x+3.0*x*x*x)/6.0:
           x<2.0?pow(2.0-x,3.0)/6.0:0.0;
}
float fieldReceiverTap(vec2 uv,vec3 p,vec2 receiverUv,vec2 gradient,float scale,float bias) {
    float depth=p.z+dot(gradient,(uv-receiverUv)/scale)-bias;
    return shadow2D(s_staticShadow,vec3(uv,depth));
}
float shadowTapField(vec3 p,vec2 tile,float scale,float bias,vec2 gradient)
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
            result+=weight.x*weight.y*fieldReceiverTap(q,p,uv,gradient,scale,bias);
        }
        return result;
    }
    vec2 weight=u_shadowFilter.x<0.5?smoothstep(vec2(0.15,0.15),vec2(0.85,0.85),phase):phase;
    vec2 a=base,b=base+vec2(atlasTexel.x,0.0),c=base+vec2(0.0,atlasTexel.y),d=base+atlasTexel;
    return mix(mix(fieldReceiverTap(a,p,uv,gradient,scale,bias),fieldReceiverTap(b,p,uv,gradient,scale,bias),weight.x),
               mix(fieldReceiverTap(c,p,uv,gradient,scale,bias),fieldReceiverTap(d,p,uv,gradient,scale,bias),weight.x),weight.y);
}
float cascadeVisibility(vec3 world, float distanceToEye)
{
    // Evaluate coordinates outside distance branches to keep their derivatives coherent.
    vec4 w=vec4(world-u_shadowOrigin.xyz,1.0);
    vec3 p0=mul(u_shadowCascades[0],w).xyz;
    vec3 p1=mul(u_shadowCascades[1],w).xyz;
    vec3 p2=mul(u_shadowCascades[2],w).xyz;
    vec3 p3=mul(u_shadowCascades[3],w).xyz;
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
    float primaryBias=mix(bias.x,bias.y,s1);
    primaryBias=mix(primaryBias,bias.z,s2);primaryBias=mix(primaryBias,bias.w,s3);
    float nextBias=mix(bias.y,bias.z,s1);nextBias=mix(nextBias,bias.w,s2);
    float radius=mix(u_shadowRanges.x,u_shadowRanges.y,s1);
    radius=mix(radius,u_shadowRanges.z,s2);
    float blend=smoothstep(radius*.85,radius,distanceToEye)*(1.0-s3);
    vec2 gradient=mix(g0,g1,s1);gradient=mix(gradient,g2,s2);gradient=mix(gradient,g3,s3);
    vec2 nextGradient=mix(g1,g2,s1);nextGradient=mix(nextGradient,g3,s2);
    float a=shadowTapField(primary,tile,.5,primaryBias,gradient);
    float result=a;
    if(blend>0.0) result=mix(a,shadowTapField(adjacent,nextTile,.5,nextBias,nextGradient),blend);
    return mix(result,1.0,smoothstep(u_shadowRanges.w*.9,u_shadowRanges.w,distanceToEye));
}
float sceneShadowVisibility(vec3 p,vec3 world)
{
    if(u_staticShadowParams.x<=0.0) return 1.0;
    if(u_pointShadowParams.x>0.5) return pointShadowVisibility(world);
    float distanceToEye=length(world-u_staticShadowEye.xyz);
    if(u_shadowRanges.x>0.0)
        return mix(1.0,cascadeVisibility(world,distanceToEye),u_staticShadowParams.x);
    float edge=min(min(p.x,1.0-p.x),min(p.y,1.0-p.y));
    // A fitted interior field (w=0) covers its complete footprint. The
    // projection/tap boundary already rejects points outside that field.
    float fade=1.0;
    if(u_staticShadowEye.w>0.0)
        fade=(1.0-smoothstep(u_staticShadowEye.w*.2,u_staticShadowEye.w,distanceToEye))*smoothstep(0.0,.22,edge);
    float visible=shadowTapField(p,vec2(0.0,0.0),1.0,u_staticShadowParams.z,shadowReceiverGradient(p));
    return mix(1.0,visible,u_staticShadowParams.x*fade);
}
