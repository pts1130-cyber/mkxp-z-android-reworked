
uniform mat4 projMat;

uniform mat4 spriteMat;

uniform vec2 texSizeInv;
uniform vec2 patternSizeInv;
uniform vec2 patternScroll;
uniform vec2 patternZoom;
uniform bool renderPattern;
uniform bool patternTile;

/* xy: scale, zw: offset (texture pixel -> map uv) */
uniform vec4 materialXform;

/* Clip position on screen when the sprite is drawn off-screen first */
uniform mat4 scrMat;
uniform bool scrMatOn;

attribute vec2 position;
attribute vec2 texCoord;

varying vec2 v_texCoord;
varying vec2 v_patCoord;
varying vec2 v_matCoord;
varying vec2 v_scrCoord;

void main()
{
	gl_Position = projMat * spriteMat * vec4(position, 0, 1);

	v_texCoord = texCoord * texSizeInv;
	v_matCoord = texCoord * materialXform.xy + materialXform.zw;
	vec4 scr = scrMatOn ? scrMat * vec4(position, 0, 1) : gl_Position;
	v_scrCoord = scr.xy * 0.5 + 0.5;

	v_patCoord = vec2(0.0);
	if (renderPattern) {
		vec2 scroll = patternScroll * (patternSizeInv / texSizeInv);
		if (patternTile)
			v_patCoord = (texCoord * (patternSizeInv / patternZoom)) - (scroll * patternSizeInv);
		else
			v_patCoord = (texCoord * (texSizeInv / patternZoom)) - (scroll * texSizeInv);
	}
}
