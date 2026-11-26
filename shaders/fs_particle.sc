$input v_texcoord0, v_texcoord1, v_texcoord2, v_color0, v_worldPos, v_viewPos, v_normal

// Volumetric tyre smoke and ground dust (#454), drawn as camera-facing
// impostors of a sphere of smoke: per pixel the puff's thickness along the
// view ray, eroded by 3D fbm noise that boils with the particle's age, lit
// by the sun through itself (an optical depth toward the light) with
// Henyey-Greenstein forward scattering at the thin edges, plus the sky.
// Soft against the scene's depth (the AO prepass, when it runs) and against
// the ground plane of its emission. Output is premultiplied alpha.
//
// Modes (v_texcoord2.y): 0 smoke/dust, 1 debris (grass blades, pebbles:
// small lit sprites of the cloud texture), 2 skid mark (texture times the
// vertex colour, for a multiply-over-alpha blend).
#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_particleDepth, 1);

uniform vec4 u_particleSun;       // xyz toward the sun (render space), w: its strength
uniform vec4 u_particleSunColor;  // rgb: the sun's diffuse
uniform vec4 u_particleAmbient;   // rgb: the sky's ambient
uniform vec4 u_particleCam[4];    // right, up, forward (into the screen), eye
uniform vec4 u_particleParams;    // time, soft depth on, 1/width, 1/height
uniform vec4 u_particleDepth;     // projection A/B, homogeneous depth
uniform vec4 u_fogColor;
uniform vec4 u_fogRange;
uniform vec4 u_fogEye;

float hash3(vec3 p) {
	p = fract(p * 0.3183099 + vec3(0.11, 0.17, 0.13));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise3(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(hash3(i + vec3(0.0, 0.0, 0.0)), hash3(i + vec3(1.0, 0.0, 0.0)), f.x);
	float b = mix(hash3(i + vec3(0.0, 1.0, 0.0)), hash3(i + vec3(1.0, 1.0, 0.0)), f.x);
	float c = mix(hash3(i + vec3(0.0, 0.0, 1.0)), hash3(i + vec3(1.0, 0.0, 1.0)), f.x);
	float d = mix(hash3(i + vec3(0.0, 1.0, 1.0)), hash3(i + vec3(1.0, 1.0, 1.0)), f.x);
	return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}

// Four octaves, 0..~0.94.
float fbm(vec3 p) {
	float sum = 0.0;
	float amplitude = 0.5;
	for (int i = 0; i < 4; ++i) {
		sum += amplitude * noise3(p);
		p = p * 2.03 + vec3(1.7, 9.2, 3.1);
		amplitude *= 0.5;
	}
	return sum;
}

float linearDepth(float d) {
	float ndc = u_particleDepth.z > 0.5 ? d * 2.0 - 1.0 : d;
	float divisor = ndc - u_particleDepth.x;
	return u_particleDepth.y / (abs(divisor) > 1e-8 ? divisor : -1e-8);
}

vec3 applyFog(vec3 colour, vec3 worldPos) {
	if (u_fogColor.a < 0.5) return colour;
	float span = max(u_fogRange.y - u_fogRange.x, 1.0);
	float t = saturate((length(worldPos - u_fogEye.xyz) - u_fogRange.x) / span);
	t = t * t * (3.0 - 2.0 * t);
	return mix(colour, u_fogColor.rgb, t);
}

void main() {
	float mode = v_texcoord2.y;
	if (mode > 1.5) {
		vec4 mark = texture2D(s_texColor, v_texcoord0);
		float a = mark.a * v_color0.a;
		gl_FragColor = vec4(mark.rgb * v_color0.rgb * a, a);
		return;
	}
	vec2 c = v_texcoord0;
	float r2 = dot(c, c);
	if (r2 >= 1.0) discard;
	vec3 right = u_particleCam[0].xyz;
	vec3 up = u_particleCam[1].xyz;
	vec3 forward = u_particleCam[2].xyz;
	float radius = max(v_texcoord1.x, 0.01);
	float seed = v_texcoord1.y;
	float z = sqrt(1.0 - r2);
	// The sphere's surface toward the camera.
	vec3 n = normalize(right * c.x + up * c.y - forward * z);
	vec3 sun = normalize(u_particleSun.xyz + vec3(0.0, 1e-4, 0.0));
	vec3 sunLight = u_particleSunColor.rgb * u_particleSun.w;
	vec3 sky = u_particleAmbient.rgb;
	// Height of this pixel's front above the ground the particle came from.
	float groundHeight = v_texcoord2.x + n.y * radius;
	float alpha;
	vec3 colour;
	if (mode > 0.5) {
		vec4 tex = texture2D(s_texColor, c * 0.5 + 0.5);
		alpha = tex.a * v_color0.a;
		vec3 albedo = tex.rgb * v_color0.rgb;
		colour = albedo * (sky + sunLight * (0.35 + 0.65 * saturate(dot(n, sun))));
	} else {
		float time = u_particleParams.x;
		vec3 seedOffset = vec3(seed * 37.0, seed * 11.0, seed * 23.0);
		// Noise in the particle's own frame, so a growing puff keeps its
		// shape while its folds roll upward over its life.
		vec3 q = n * 1.35 + vec3(c * 0.6, 0.0) + seedOffset + vec3(0.0, -time * 0.22, time * 0.07);
		float body = fbm(q * 1.1);
		float detail = fbm(q * 3.3 + vec3(5.2, 1.3, 7.7));
		float shape = z * 1.5 - (1.0 - body) * 0.85 - (1.0 - detail) * 0.18;
		// Soft rims: neighbouring puffs merge into one cloud instead of
		// reading as separate balls.
		float density = saturate(shape) * (1.0 - smoothstep(0.35, 1.0, r2));
		density = pow(max(density, 1e-4), 1.25);
		// The authored puff (the cfg's texture) still shapes the cloud.
		vec4 tex = texture2D(s_texColor, c * 0.5 + 0.5);
		density *= mix(1.0, tex.a, 0.3);
		// Thick smoke scatters light many times: lift the authored colour toward
		// white so a grey cfg colour reads as tyre smoke, not oil smoke.
		vec3 albedo = mix(v_color0.rgb * mix(vec3(1.0, 1.0, 1.0), tex.rgb, 0.25), vec3(0.92, 0.92, 0.92), 0.25);

		// Light reaching this pixel crosses the puff from the sun side: an
		// optical depth growing from the lit face to the far face.
		float ndl = dot(n, sun);
		float across = 1.0 - (ndl * 0.5 + 0.5);
		float shadow = exp(-1.5 * across * (0.3 + density * z));
		// Forward scattering: bright rims when looking toward the sun
		// through the thin edges (Henyey-Greenstein, g 0.6, isotropic = 1).
		vec3 view = normalize(v_worldPos - u_particleCam[3].xyz);
		float cosTheta = dot(view, sun);
		float g = 0.6;
		float hg = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-3), 1.5);
		float rim = hg * 0.12 * (1.0 - density) * (1.0 - z * 0.5);
		vec3 direct = sunLight * (shadow * (0.4 + 0.6 * saturate(ndl * 0.5 + 0.5)) + rim);
		// Sky from above, darker underneath; a little light scattered many
		// times inside the cloud keeps its core from going black.
		vec3 ambient = sky * (0.7 + 0.3 * saturate(n.y * 0.5 + 0.5)) + sunLight * 0.1;
		// No brighter than a white surface facing the sun: a desert sun must
		// not burn the cloud to white.
		vec3 light = direct + ambient;
		vec3 limit = (sunLight + sky) * 0.95;
		colour = albedo * min(light, limit);
		alpha = density * v_color0.a * 0.85;
		// No hard line where the puff meets the road.
		alpha *= saturate(groundHeight / (radius * 0.45 + 0.05));
	}
	// Soft against the scene: fade as the puff's front nears what is behind it.
	if (u_particleParams.y > 0.5) {
		vec2 uv = gl_FragCoord.xy * u_particleParams.zw;
		float d = texture2D(s_particleDepth, uv).r;
		if (d > 0.0) {
			float scene = linearDepth(d);
			float front = v_viewPos.z - radius * z;
			alpha *= saturate((scene - front) / (radius * 0.6 + 0.05));
		}
	}
	// A camera inside the cloud does not see a wall of smoke.
	alpha *= saturate((v_viewPos.z - 0.35) / 1.2);
	colour = applyFog(colour, v_worldPos);
	gl_FragColor = vec4(colour * alpha, alpha);
}
