uniform mat4 projMat;

uniform vec2 texSizeInv;

/* xy: scale, zw: offset (target pixel -> scene uv) */
uniform vec4 arcSceneXform;

attribute vec2 position;
attribute vec2 texCoord;

varying vec2 v_texCoord;
varying vec2 v_sceneCoord;

void main()
{
	gl_Position = projMat * vec4(position, 0.0, 1.0);

	v_texCoord = texCoord * texSizeInv;
	v_sceneCoord = position * arcSceneXform.xy + arcSceneXform.zw;
}
