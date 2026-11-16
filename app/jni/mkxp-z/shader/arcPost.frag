
uniform sampler2D texture;
uniform sampler2D arcScene;

uniform vec2 arcTexel;
uniform vec2 arcSceneTexel;
uniform vec2 arcSize;
uniform vec4 arcRect;
uniform vec4 arcSceneRect;

varying vec2 v_texCoord;
varying vec2 v_sceneCoord;

vec4 arcTex(vec2 uv)
{
	return texture2D(texture, clamp(uv, arcRect.xy, arcRect.zw));
}

vec4 arcSceneTex(vec2 uv)
{
	return texture2D(arcScene, clamp(uv, arcSceneRect.xy, arcSceneRect.zw));
}

/*ARC_USER*/

void main()
{
	gl_FragColor = arcUser(texture2D(texture, v_texCoord), v_texCoord);
}
