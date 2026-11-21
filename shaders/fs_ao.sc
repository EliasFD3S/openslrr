$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_aoDepth,0);
SAMPLER2D(s_aoInput,1);
// AO geometry (#256), written once per frame at AO resolution by pass 3:
// linear view depth (0 = no surface) and the reconstructed normal (w = 1 when
// valid). Every AO texel stands for the full-resolution depth texel under its
// centre, so positions rebuilt from it match positionAt exactly.
SAMPLER2D(s_aoLinear,2);
SAMPLER2D(s_aoNormal,3);
uniform vec4 u_aoScreen; // inverse depth size, projection scale
uniform vec4 u_aoProjection; // projection A/B, homogeneous depth, bottom-left texture
uniform vec4 u_aoConfig; // method, quality, world radius, reference sampling (#256)
uniform vec4 u_aoParams; // pass (0 AO, 1/2 filters, 3 geometry), inverse AO size
const float PI=3.14159265359;
float depthAt(vec2 uv) {return texture2DLod(s_aoDepth,uv,0.0).r;}
float linearDepth(float d) {
    float ndc=u_aoProjection.z>0.5?d*2.0-1.0:d;
    float divisor=ndc-u_aoProjection.x;
    return u_aoProjection.y/(abs(divisor)>1e-8?divisor:-1e-8);
}
vec3 positionAt(vec2 uv) {
    // Nearest-depth sampling and position reconstruction must refer to the
    // same texel centre. Subpixel rays with a quantised depth invent slopes
    // even on a perfectly planar receiver.
    uv=clamp((floor(uv/u_aoScreen.xy)+vec2(0.5,0.5))*u_aoScreen.xy,
             u_aoScreen.xy*0.5,vec2(1.0,1.0)-u_aoScreen.xy*0.5);
    float z=linearDepth(depthAt(uv));
    vec2 ndc=uv*2.0-1.0;
    if(u_aoProjection.w<0.5) ndc.y=-ndc.y;
    return vec3(ndc/u_aoScreen.zw*z,z);
}
vec3 surfaceNormal(vec2 uv,vec3 p) {
    vec2 t=u_aoScreen.xy;
    vec2 lo=t*0.5,hi=vec2(1.0,1.0)-lo;
    vec2 left=clamp(uv-vec2(t.x,0.0),lo,hi),right=clamp(uv+vec2(t.x,0.0),lo,hi);
    vec2 down=clamp(uv-vec2(0.0,t.y),lo,hi),up=clamp(uv+vec2(0.0,t.y),lo,hi);
    float c=depthAt(uv),l=depthAt(left),r=depthAt(right),d=depthAt(down),u=depthAt(up);
    float l2=depthAt(clamp(uv-vec2(2.0*t.x,0.0),lo,hi));
    float r2=depthAt(clamp(uv+vec2(2.0*t.x,0.0),lo,hi));
    float d2=depthAt(clamp(uv-vec2(0.0,2.0*t.y),lo,hi));
    float u2=depthAt(clamp(uv+vec2(0.0,2.0*t.y),lo,hi));
    // Projected depth is affine on a plane. Predict the centre from two
    // samples per side instead of choosing the nearest Z (which crosses
    // silhouettes on tilted floors and manufactures sloping AO receivers).
    float el=l>0.0 && l2>0.0?abs(2.0*l-l2-c):1e10;
    float er=r>0.0 && r2>0.0?abs(2.0*r-r2-c):1e10;
    float ed=d>0.0 && d2>0.0?abs(2.0*d-d2-c):1e10;
    float eu=u>0.0 && u2>0.0?abs(2.0*u-u2-c):1e10;
    if(min(el,er)>max(abs(c)*0.01,1e-7) || min(ed,eu)>max(abs(c)*0.01,1e-7)) return vec3(0.0,0.0,0.0);
    bool chooseLeft=abs(el-er)<max(abs(c)*1e-5,1e-9)?abs(l-c)<abs(r-c):el<er;
    bool chooseDown=abs(ed-eu)<max(abs(c)*1e-5,1e-9)?abs(d-c)<abs(u-c):ed<eu;
    // Recover the plane normal directly from projected-depth slopes. Taking
    // differences of reconstructed positions loses precision at grazing angles.
    float sx=(chooseLeft?c-l:r-c)/(2.0*t.x);
    float sy=(chooseDown?c-d:u-c)/(2.0*t.y);
    vec2 ndc=uv*2.0-1.0;
    float scale=u_aoProjection.z>0.5?2.0:1.0;
    float centerNdc=u_aoProjection.z>0.5?c*2.0-1.0:c;
    vec3 n=vec3(sx*scale*u_aoScreen.z,
                sy*scale*u_aoScreen.w*(u_aoProjection.w>0.5?1.0:-1.0),
                centerNdc-u_aoProjection.x-scale*dot(vec2(sx,sy),ndc));
    float n2=dot(n,n);
    if(n2<1e-30) return vec3(0.0,0.0,0.0);
    n*=inversesqrt(n2);
    return dot(n,-p)<0.0?-n:n;
}
vec2 fullTexelCentre(vec2 uv) {
    return clamp((floor(uv/u_aoScreen.xy)+vec2(0.5,0.5))*u_aoScreen.xy,
                 u_aoScreen.xy*0.5,vec2(1.0,1.0)-u_aoScreen.xy*0.5);
}
vec3 positionFromDepth(vec2 centre,float z) {
    vec2 ndc=centre*2.0-1.0;
    if(u_aoProjection.w<0.5) ndc.y=-ndc.y;
    return vec3(ndc/u_aoScreen.zw*z,z);
}
// AO texel holding uv (clamped) and its full-resolution depth texel centre.
ivec2 aoTexel(vec2 uv) {
    vec2 size=vec2(1.0,1.0)/u_aoParams.yz;
    return ivec2(clamp(floor(uv*size),vec2(0.0,0.0),size-vec2(1.0,1.0)));
}
vec2 aoTexelCentre(ivec2 t) { return (vec2(t)+vec2(0.5,0.5))*u_aoParams.yz; }
// The full-resolution depth texel an AO texel stands for (#274). The AO
// texel centre lies exactly on a full-texel boundary at half resolution, so
// fullTexelCentre(aoTexelCentre(t)) could pick either neighbour, and the
// geometry pass (from the interpolated uv) and the AO pass (from integer
// texels) did not always pick the same one. The stored depth then belonged
// to one texel and the rebuilt position to its neighbour: on a wall seen at
// a grazing angle that is a large depth error, and open walls darkened.
// A quarter-texel bias makes the choice unambiguous and identical everywhere.
vec2 aoFullCentre(ivec2 t) {
    vec2 full=floor((vec2(t)+vec2(0.5,0.5))*u_aoParams.yz/u_aoScreen.xy-vec2(0.25,0.25));
    vec2 size=vec2(1.0,1.0)/u_aoScreen.xy;
    return (clamp(full,vec2(0.0,0.0),size-vec2(1.0,1.0))+vec2(0.5,0.5))*u_aoScreen.xy;
}
vec3 sampleDelta(vec2 uv,vec3 p) {
    if(any(lessThan(uv,vec2(0.0,0.0))) || any(greaterThan(uv,vec2(1.0,1.0))))
        return vec3(0.0,0.0,0.0);
    if(u_aoConfig.w>0.5) {
        // Reference (#256 off): the full-resolution depth, one fetch.
        vec2 centre=fullTexelCentre(uv);
        float d=depthAt(centre);
        return d<=0.0?vec3(0.0,0.0,0.0):positionFromDepth(centre,linearDepth(d))-p;
    }
    // AO-resolution linear depth: a quarter of the texels over the radius.
    ivec2 t=aoTexel(uv);
    float z=texelFetch(s_aoLinear,t,0).r;
    return z<=0.0?vec3(0.0,0.0,0.0):positionFromDepth(aoFullCentre(t),z)-p;
}
float hbao(vec2 uv,vec3 p,vec3 n,vec2 radiusUv,float rotation,float jitter) {
    // High runs at half resolution like Medium (#248) with more directions.
    int directions=u_aoConfig.y<0.5?4:u_aoConfig.y<1.5?6:10;
    int steps=u_aoConfig.y<0.5?3:u_aoConfig.y<1.5?4:6;
    float sum=0.0,r2=u_aoConfig.z*u_aoConfig.z;
    for(int i=0;i<10;++i) {
        if(i>=directions) break;
        float angle=2.0*PI*(float(i)+rotation)/float(directions);
        vec2 direction=vec2(cos(angle),sin(angle))*radiusUv;
        float horizon=0.05;
        for(int k=0;k<6;++k) {
            if(k>=steps) break;
            float distance=(float(k)+jitter)/float(steps);distance*=distance;
            vec3 delta=sampleDelta(uv+direction*distance,p);
            float d2=dot(delta,delta);
            if(d2<0.0001 || d2>r2) continue;
            float elevation=max(dot(n,delta*inversesqrt(d2)),0.0);
            float next=max(horizon,elevation);
            sum+=(next-horizon)*saturate(1.0-d2/r2);
            horizon=next;
        }
    }
    return saturate(1.0-sum*2.0/float(directions));
}
// Ground-truth AO slice integral (Jimenez et al. 2016), independently expressed
// for bgfx. The projected normal and two horizon angles define each cosine-
// weighted visible arc. No vendor SDK, temporal history or deinterleaving.
float gtao(vec2 uv,vec3 p,vec3 n,vec2 radiusUv,float rotation,float jitter) {
    // High runs at half resolution like Medium (#248) with 5 slices x 8 steps.
    int slices=u_aoConfig.y<0.5?2:u_aoConfig.y<1.5?3:5;
    int steps=u_aoConfig.y<0.5?4:u_aoConfig.y<1.5?6:8;
    vec3 view=normalize(-p);float visibility=0.0,r2=u_aoConfig.z*u_aoConfig.z;
    for(int i=0;i<5;++i) {
        if(i>=slices) break;
        float phi=PI*(float(i)+rotation)/float(slices);
        vec2 screenDirection=vec2(cos(phi),sin(phi));
        vec3 direction=vec3(screenDirection.x,u_aoProjection.w>0.5?screenDirection.y:-screenDirection.y,0.0);
        vec3 tangent=direction-dot(direction,view)*view;
        vec3 axis=cross(tangent,view);axis*=inversesqrt(max(dot(axis,axis),1e-8));
        vec3 projected=n-axis*dot(n,axis);
        float lengthN=max(length(projected),0.0001);
        float cosine=clamp(dot(projected,view)/lengthN,0.0,1.0);
        float normalAngle=(dot(tangent,projected)<0.0?-1.0:1.0)*acos(cosine);
        float low0=cos(normalAngle+PI*0.5),low1=cos(normalAngle-PI*0.5);
        float h0=low0,h1=low1;
        for(int k=0;k<8;++k) {
            if(k>=steps) break;
            float distance=(float(k)+jitter)/float(steps);distance*=distance;
            vec2 offset=screenDirection*radiusUv*distance;
            vec3 a=sampleDelta(uv+offset,p),b=sampleDelta(uv-offset,p);
            float a2=dot(a,a),b2=dot(b,b);
            // Quantised screen samples can leave the nominal slice plane.
            // A coplanar/behind-tangent sample cannot occlude its hemisphere.
            if(a2>0.0001 && a2<r2 && dot(n,a)*inversesqrt(a2)>0.02) h0=max(h0,mix(low0,dot(a*inversesqrt(a2),view),saturate(1.0-a2/r2)));
            if(b2>0.0001 && b2<r2 && dot(n,b)*inversesqrt(b2)>0.02) h1=max(h1,mix(low1,dot(b*inversesqrt(b2),view),saturate(1.0-b2/r2)));
        }
        // Avoid cancellation in the grazing-angle integral when no sample occludes this slice.
        if(h0==low0 && h1==low1) {visibility+=1.0;continue;}
        float a0=clamp(-acos(clamp(h1,-1.0,1.0)),normalAngle-PI*0.5,normalAngle+PI*0.5);
        float a1=clamp( acos(clamp(h0,-1.0,1.0)),normalAngle-PI*0.5,normalAngle+PI*0.5);
        float integral0=(cosine+2.0*a0*sin(normalAngle)-cos(2.0*a0-normalAngle))*0.25;
        float integral1=(cosine+2.0*a1*sin(normalAngle)-cos(2.0*a1-normalAngle))*0.25;
        // Normalise the finite-slice quadrature against its unobstructed
        // hemisphere. A tilted plane must remain fully visible at every tier.
        float open0=normalAngle-PI*0.5,open1=normalAngle+PI*0.5;
        float baseline=(2.0*cosine+2.0*(open0+open1)*sin(normalAngle)
                       -cos(2.0*open0-normalAngle)-cos(2.0*open1-normalAngle))*0.25;
        visibility+=clamp((integral0+integral1)/max(baseline,0.0001),0.0,1.0);
    }
    return clamp(visibility/float(slices),0.03,1.0);
}
void main() {
    vec2 uv=v_texcoord0;
    ivec2 texel=aoTexel(uv);
    vec2 receiverUv=aoFullCentre(texel);
    // One shader serves the geometry pass (two targets) and the single-target
    // passes: initialise the second output for the latter.
    gl_FragData[1]=vec4(0.0,0.0,0.0,0.0);
    if(u_aoParams.x>2.5) {
        // Geometry pass (#256): depth and the 9-tap normal, once per frame.
        float depth=depthAt(receiverUv);
        if(depth<=0.0) {gl_FragData[0]=vec4(0.0,0.0,0.0,0.0);gl_FragData[1]=vec4(0.0,0.0,0.0,0.0);return;}
        vec3 p=positionAt(receiverUv);
        vec3 n=surfaceNormal(receiverUv,p);
        gl_FragData[0]=vec4(p.z,0.0,0.0,0.0);
        gl_FragData[1]=vec4(n,dot(n,n)<0.5?0.0:1.0);
        return;
    }
    float z=texelFetch(s_aoLinear,texel,0).r;
    if(z<=0.0) {gl_FragData[0]=vec4(1.0,1.0,1.0,1.0);return;}
    vec4 stored=texelFetch(s_aoNormal,texel,0);
    if(stored.w<0.5) {gl_FragData[0]=vec4(1.0,1.0,1.0,1.0);return;}
    vec3 p=positionFromDepth(receiverUv,z);
    vec3 n=stored.xyz;
    if(u_aoParams.x>0.5) {
        vec2 direction=u_aoParams.x<1.5?vec2(u_aoParams.y,0.0):vec2(0.0,u_aoParams.z);
        float sum=0.0,weight=0.0;
        for(int k=-2;k<=2;++k) {
            vec2 q=clamp(uv+direction*float(k),vec2(0.0,0.0),vec2(1.0,1.0));
            ivec2 tq=aoTexel(q);
            float zq=texelFetch(s_aoLinear,tq,0).r;
            if(zq<=0.0) continue;
            vec3 samplePosition=positionFromDepth(aoFullCentre(tq),zq);
            // [1,2,2,2,1]/8 spans exactly one 4-texel noise period, so a flat
            // receiver averages all sixteen rotation/jitter cells after both
            // passes. A Gaussian leaves the per-pixel pattern visible as
            // streaks along occluder silhouettes.
            float spatial=abs(k)==2?0.125:0.25;
            // Same-plane neighbours remain filterable on tilted receivers;
            // foreground silhouettes are rejected by their plane distance.
            float planeDistance=abs(dot(samplePosition-p,n));
            float w=spatial*exp2(-planeDistance/max(u_aoConfig.z*0.025,0.01));
            sum+=texture2DLod(s_aoInput,q,0.0).r*w;weight+=w;
        }
        float result=weight>0.0001?sum/weight:1.0;
        gl_FragData[0]=vec4(result,result,result,1.0);return;
    }
    vec2 radiusUv=u_aoConfig.z*u_aoScreen.zw/(2.0*max(p.z,0.05));
    // Radius is in metres: a fixed pixel cap shrinks contact AO as the camera
    // approaches an object. Off-screen samples are rejected by sampleDelta.
    // HBAO+-style 4x4 tile: every texel of a 4x4 block gets a distinct
    // direction rotation and a distinct radial step offset. With fixed step
    // distances every receiver sampled the same rings, so a car or pedestrian
    // silhouette was stamped as parallel copies on the ground (reported
    // streaks). The separable [1,2,2,2,1]/8 filter passes remove the tile.
    vec2 cell=mod(floor(uv/u_aoParams.yz),4.0);
    float index=dot(cell,vec2(1.0,4.0));
    float rotation=(mod(index*5.0,16.0)+0.5)/16.0;
    float jitter=(mod(dot(cell,vec2(4.0,1.0))+5.0,16.0)+0.5)/16.0;
    float visibility=u_aoConfig.x<1.5?hbao(receiverUv,p,n,radiusUv,rotation,jitter):gtao(receiverUv,p,n,radiusUv,rotation,jitter);
    gl_FragData[0]=vec4(visibility,visibility,visibility,1.0);
}
