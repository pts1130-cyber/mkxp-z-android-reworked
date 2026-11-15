
uniform sampler2D texture;

uniform lowp vec4 tone;
uniform lowp float opacity;
uniform lowp vec4 color;

uniform float bushDepth;
uniform lowp float bushOpacity;

uniform sampler2D pattern;
uniform int patternBlendType;
uniform lowp float patternOpacity;
uniform bool renderPattern;

uniform bool invert;

/* Material stage.
 * mode 1 surface: map r=value g=hue b=saturation a=weight
 * mode 2 lit:     map r=level g,b=normal xy (0.5 = facing) a=weight
 * mode 3 halo:    sprite alpha neighbourhood, output glow colour */
uniform int materialMode;
uniform sampler2D materialMap;
uniform sampler2D materialEnv;
uniform sampler2D materialRamp;
uniform sampler2D materialBack;
uniform vec2 materialMapTexel;
uniform vec2 materialSrcTexel;
uniform vec2 materialBackInv;
uniform bool materialHasEnv;
uniform bool materialHasRamp;
uniform bool materialHasBack;
uniform float materialMirror;
uniform vec4 materialP0;
uniform vec4 materialP1;
uniform vec4 materialP2;
uniform vec4 materialP3;

varying vec2 v_texCoord;
varying vec2 v_patCoord;
varying vec2 v_matCoord;
varying vec2 v_scrCoord;

const vec3 lumaF = vec3(.299, .587, .114);
const vec2 repeat = vec2(1, 1);

vec3 blendNormal(vec3 base, vec3 blend, float o) { return blend * o + base * (1.0 - o); }
vec3 blendAdd(vec3 base, vec3 blend, float o) { return min(base + blend, vec3(1.0)) * o + base * (1.0 - o); }
vec3 blendSubtract(vec3 base, vec3 blend, float o) { return max(base - blend, vec3(0.0)) * o + base * (1.0 - o); }

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
	vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
	return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

/* keep HSL saturation and lightness of base, take hue h */
vec3 withHue(vec3 base, float h)
{
	float mx = max(max(base.r, base.g), base.b);
	float mn = min(min(base.r, base.g), base.b);
	float l = (mx + mn) * 0.5;
	float d = mx - mn;
	float s = (d <= 0.0) ? 0.0 : d / (1.0 - abs(2.0 * l - 1.0) + 1.0e-10);
	vec3 p = abs(fract(vec3(h) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
	float C = (1.0 - abs(2.0 * l - 1.0)) * s;
	return l + C * (clamp(p - 1.0, 0.0, 1.0) - 0.5);
}

vec3 overlay(vec3 a, vec3 b)
{
	return mix(2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b), step(0.5, a));
}

vec3 softLight(vec3 a, vec3 b)
{
	return mix(2.0 * a * b + a * a * (1.0 - 2.0 * b),
	           sqrt(a) * (2.0 * b - 1.0) + 2.0 * a * (1.0 - b), step(0.5, b));
}

vec3 screenB(vec3 a, vec3 b) { return 1.0 - (1.0 - a) * (1.0 - b); }

vec3 blendKind(vec3 a, vec3 b, float kind)
{
	if (kind < 0.5) return overlay(a, b);
	if (kind < 1.5) return softLight(a, b);
	if (kind < 2.5) return screenB(a, b);
	return min(a + b, vec3(1.0));
}

/* map normal, optionally bilinear (weighted by map alpha so the silhouette does not pull it to 0) */
vec2 mapNormal(vec4 m)
{
	if (materialP1.w < 0.5)
		return m.gb * 2.0 - 1.0;

	vec2 px = v_matCoord / materialMapTexel - 0.5;
	vec2 f = fract(px);
	vec2 b = (floor(px) + 0.5) * materialMapTexel;
	vec4 t00 = texture2D(materialMap, b);
	vec4 t10 = texture2D(materialMap, b + vec2(materialMapTexel.x, 0.0));
	vec4 t01 = texture2D(materialMap, b + vec2(0.0, materialMapTexel.y));
	vec4 t11 = texture2D(materialMap, b + materialMapTexel);
	float w00 = (1.0 - f.x) * (1.0 - f.y) * t00.a;
	float w10 = f.x * (1.0 - f.y) * t10.a;
	float w01 = (1.0 - f.x) * f.y * t01.a;
	float w11 = f.x * f.y * t11.a;
	float ws = w00 + w10 + w01 + w11;
	if (ws <= 0.0)
		return m.gb * 2.0 - 1.0;
	vec2 g = (t00.gb * w00 + t10.gb * w10 + t01.gb * w01 + t11.gb * w11) / ws;
	return g * 2.0 - 1.0;
}

vec3 surfaceStage(vec3 orig, vec4 m)
{
	float s = materialP0.x * m.a;
	float L = dot(orig, lumaF);
	vec3 tint = materialP1.rgb;
	vec3 hsv = rgb2hsv(orig);

	vec3 col1 = mix(orig, withHue(orig, rgb2hsv(tint).x), materialP1.w * s);
	col1 += (tint - col1) * (materialP2.x * s * (1.0 - L));

	if (materialP2.y >= 0.0) {
		float dh = abs(fract(hsv.x - materialP2.y + 0.5) - 0.5);
		float isMain = step(dh, materialP2.z) * step(0.2, hsv.y);
		col1 += (1.0 - col1) * (materialP2.w * s * isMain);
	}

	float v = clamp(materialP3.y + (m.r - materialP3.y) * materialP3.x, 0.05, 1.0);
	float sat = (materialP3.z >= 0.0 && L < materialP3.w) ? materialP3.z : m.b;
	vec3 pat = hsv2rgb(vec3(fract(m.g + materialP0.z), sat, v));
	pat = mix(pat, tint, materialP0.w);

	return mix(col1, blendKind(col1, pat, materialP0.y), s);
}

vec3 litStage(vec3 orig, vec4 m)
{
	float s = materialP0.x * m.a;
	vec3 tint = materialP3.rgb;
	vec2 n = mapNormal(m);
	n.x *= materialMirror;

	float t = clamp(m.r + (dot(orig, lumaF) - materialP0.w) * materialP0.z, 0.0, 1.0);
	vec3 col = materialHasRamp ? texture2D(materialRamp, vec2(t, materialP0.y)).rgb : tint * t;

	if (materialHasEnv) {
		vec2 e = clamp(n * (0.5 * materialP1.z) + 0.5 + materialP2.xy, 0.0, 1.0);
		col = screenB(col, texture2D(materialEnv, e).rgb * materialP1.y);
	}

	if (materialP2.w > 0.0) {
		float lc = dot(col, lumaF);
		float lt = max(dot(tint, lumaF), 1.0e-3);
		col = mix(col, clamp(tint * (lc / lt), 0.0, 1.0), materialP2.w);
	}

	if (materialHasBack && materialP3.w > 0.0) {
		vec3 bg = texture2D(materialBack, v_scrCoord + n * (materialP3.w * materialBackInv)).rgb;
		col = mix(col, bg * mix(vec3(1.0), tint, 0.5), materialP2.z * (1.0 - m.r));
	}

	col = mix(col, softLight(col, orig), materialP1.x);
	return mix(orig, col, s);
}

vec4 haloStage(vec4 frag)
{
	float r = materialP0.y;
	vec2 tx = v_texCoord;
	vec2 d = r * materialSrcTexel;
	float acc = 0.0;
	/* two rings of 8 taps, gaussian weights for 1 and 2 sigma */
	acc += 0.6065 * (texture2D(texture, tx + vec2( d.x, 0.0)).a + texture2D(texture, tx + vec2(-d.x, 0.0)).a
	              +  texture2D(texture, tx + vec2(0.0,  d.y)).a + texture2D(texture, tx + vec2(0.0, -d.y)).a);
	acc += 0.6065 * (texture2D(texture, tx + d * 0.7071).a + texture2D(texture, tx - d * 0.7071).a
	              +  texture2D(texture, tx + vec2(d.x, -d.y) * 0.7071).a + texture2D(texture, tx + vec2(-d.x, d.y) * 0.7071).a);
	vec2 e = d * 2.0;
	acc += 0.1353 * (texture2D(texture, tx + vec2( e.x, 0.0)).a + texture2D(texture, tx + vec2(-e.x, 0.0)).a
	              +  texture2D(texture, tx + vec2(0.0,  e.y)).a + texture2D(texture, tx + vec2(0.0, -e.y)).a);
	acc += 0.1353 * (texture2D(texture, tx + e * 0.7071).a + texture2D(texture, tx - e * 0.7071).a
	              +  texture2D(texture, tx + vec2(e.x, -e.y) * 0.7071).a + texture2D(texture, tx + vec2(-e.x, e.y) * 0.7071).a);
	float g = (acc + frag.a) / (1.0 + 8.0 * 0.6065 + 8.0 * 0.1353);
	g *= 1.0 - frag.a * materialP0.z;
	return vec4(materialP3.rgb, clamp(g * materialP0.x, 0.0, 1.0));
}

void main()
{
	vec4 frag = texture2D(texture, v_texCoord);

	if (renderPattern) {
		vec4 pattfrag = texture2D(pattern, mod(v_patCoord, repeat));
		if (patternBlendType == 1)
			frag.rgb = blendAdd(frag.rgb, pattfrag.rgb, pattfrag.a * patternOpacity);
		else if (patternBlendType == 2)
			frag.rgb = blendSubtract(frag.rgb, pattfrag.rgb, pattfrag.a * patternOpacity);
		else
			frag.rgb = blendNormal(frag.rgb, pattfrag.rgb, pattfrag.a * patternOpacity);
	}

	if (materialMode == 3) {
		frag = haloStage(frag);
	}
	else if (materialMode > 0) {
		vec4 m = texture2D(materialMap, v_matCoord);
		if (m.a > 0.0 && frag.a > 0.0) {
			if (materialMode == 1)
				frag.rgb = surfaceStage(frag.rgb, m);
			else
				frag.rgb = litStage(frag.rgb, m);
		}
	}

	float luma = dot(frag.rgb, lumaF);
	frag.rgb = mix(frag.rgb, vec3(luma), tone.w);
	frag.rgb += tone.rgb;
	frag.a *= opacity;
	frag.rgb = mix(frag.rgb, color.rgb, color.a);

	if (invert)
		frag.rgb = vec3(1.0) - frag.rgb;

	lowp float underBush = float(v_texCoord.y < bushDepth);
	frag.a *= clamp(bushOpacity + underBush, 0.0, 1.0);

	gl_FragColor = frag;
}
