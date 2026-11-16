
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

varying vec2 v_texCoord;
varying vec2 v_patCoord;

const vec3 lumaF = vec3(.299, .587, .114);
const vec2 repeat = vec2(1, 1);


// = = = = = = = = = = =
// mixing functions, from https://github.com/jamieowen/glsl-blend
// = = = = = = = = = = =

// Normal
vec3 blendNormal(vec3 base, vec3 blend) {
    return blend;
}

vec3 blendNormal(vec3 base, vec3 blend, float opacity) {
    return (blendNormal(base, blend) * opacity + base * (1.0 - opacity));
}

// Add
float blendAdd(float base, float blend) {
    return min(base+blend,1.0);
}

vec3 blendAdd(vec3 base, vec3 blend) {
    return min(base+blend,vec3(1.0));
}

vec3 blendAdd(vec3 base, vec3 blend, float opacity) {
    return (blendAdd(base, blend) * opacity + base * (1.0 - opacity));
}

// Subtract
float blendSubtract(float base, float blend) {
    return max(base-blend,0.0);
}

vec3 blendSubtract(vec3 base, vec3 blend) {
    return max(base-blend,vec3(0.0));
}

vec3 blendSubtract(vec3 base, vec3 blend, float opacity) {
    return (blendSubtract(base, blend) * opacity + base * (1.0 - opacity));
}

// = = = = = = = = = = =

#ifdef ARC_USER
uniform vec2 arcTexel;
uniform vec2 arcSize;
uniform vec4 arcRect;

bool arcInside(vec2 uv)
{
	return uv.x >= arcRect.x && uv.y >= arcRect.y && uv.x < arcRect.z && uv.y < arcRect.w;
}

vec4 arcTex(vec2 uv)
{
	return arcInside(uv) ? texture2D(texture, uv) : vec4(0.0);
}

/* shader_pad area takes the bush of the nearest src_rect pixel */
vec2 arcBushCoord(vec2 uv)
{
	return clamp(uv, arcRect.xy, arcRect.zw - arcTexel * 0.5);
}
#else
#define arcBushCoord(uv) (uv)
#endif
/*ARC_USER*/

void main()
{
	/* Sample source color */
	vec4 frag = texture2D(texture, v_texCoord);
    
    /* Apply pattern */
    if (renderPattern) {
        vec4 pattfrag = texture2D(pattern, mod(v_patCoord, repeat));
        if (patternBlendType == 1) {
            frag.rgb =  blendAdd(frag.rgb, pattfrag.rgb, pattfrag.a * patternOpacity);
        }
        else if (patternBlendType == 2) {
            frag.rgb = blendSubtract(frag.rgb, pattfrag.rgb, pattfrag.a * patternOpacity);
        }
        else {
            frag.rgb = blendNormal(frag.rgb, pattfrag.rgb, pattfrag.a * patternOpacity);
        }
    }

#ifdef ARC_USER
	frag = arcUser(arcInside(v_texCoord) ? frag : vec4(0.0), v_texCoord);
#endif
	
	/* Apply gray */
	float luma = dot(frag.rgb, lumaF);
	frag.rgb = mix(frag.rgb, vec3(luma), tone.w);
	
	/* Apply tone */
	frag.rgb += tone.rgb;

	/* Apply opacity */
	frag.a *= opacity;
	
	/* Apply color */
	frag.rgb = mix(frag.rgb, color.rgb, color.a);
    
    /* Apply color inversion */
    if (invert) {
        frag.rgb = vec3(1.0 - frag.r, 1.0 - frag.g, 1.0 - frag.b);
    }

	/* Apply bush alpha by mathematical if */
	lowp float underBush = float(arcBushCoord(v_texCoord).y < bushDepth);
	frag.a *= clamp(bushOpacity + underBush, 0.0, 1.0);
	
	gl_FragColor = frag;
}
