/*
** particlelayer.cpp
**
** Many small cells of one bitmap sheet drawn with a single draw call.
** GLES2 has no instancing, so every particle becomes one quad of a
** streamed vertex buffer that shares the global quad index buffer.
*/

#include "particlelayer.h"
#include "particlecore.h"

#include "sharedstate.h"
#include "bitmap.h"
#include "etc.h"
#include "etc-internal.h"
#include "util.h"
#include "exception.h"
#include "debugwriter.h"

#include "gl-util.h"
#include "gl-meta.h"
#include "quadarray.h"
#include "shader.h"
#include "glstate.h"

#include <string.h>

static const char particleVert[] =
"uniform mat4 projMat;\n"
"uniform vec2 texSizeInv;\n"
"uniform vec2 translation;\n"
"attribute vec2 position;\n"
"attribute vec2 texCoord;\n"
"attribute lowp vec4 color;\n"
"varying vec2 v_texCoord;\n"
"varying lowp vec4 v_color;\n"
"void main()\n"
"{\n"
"	gl_Position = projMat * vec4(position + translation, 0, 1);\n"
"	v_texCoord = texCoord * texSizeInv;\n"
"	v_color = color;\n"
"}\n";

static const char particleFrag[] =
"uniform sampler2D texture;\n"
"uniform lowp vec4 tone;\n"
"uniform lowp vec4 color;\n"
"varying vec2 v_texCoord;\n"
"varying lowp vec4 v_color;\n"
"const vec3 lumaF = vec3(.299, .587, .114);\n"
"void main()\n"
"{\n"
"	vec4 frag = texture2D(texture, v_texCoord);\n"
"	frag.rgb *= v_color.rgb;\n"
"	float luma = dot(frag.rgb, lumaF);\n"
"	frag.rgb = mix(frag.rgb, vec3(luma), tone.w);\n"
"	frag.rgb += tone.rgb;\n"
"	frag.a *= v_color.a;\n"
"	frag.rgb = mix(frag.rgb, color.rgb, color.a);\n"
"	gl_FragColor = frag;\n"
"}\n";

class ParticleShader : public ShaderBase
{
public:
	ParticleShader()
	{
		Shader::init((const unsigned char *) particleVert, sizeof(particleVert) - 1,
		             (const unsigned char *) particleFrag, sizeof(particleFrag) - 1,
		             "particle", "particle", "ParticleShader");

		ShaderBase::init();

		u_tone = gl.GetUniformLocation(program, "tone");
		u_color = gl.GetUniformLocation(program, "color");
	}

	void setTone(const Vec4 &value) { setVec4Uniform(u_tone, value); }
	void setColor(const Vec4 &value) { setVec4Uniform(u_color, value); }

private:
	GLint u_tone, u_color;
};

/* Built on first use so a driver that rejects it only turns particles off */
static ParticleShader *particleShader;
static bool particleShaderFailed;

static ParticleShader *getParticleShader()
{
	if (particleShader || particleShaderFailed)
		return particleShader;

	try {
		particleShader = new ParticleShader();
	} catch (const Exception &e) {
		particleShaderFailed = true;
		Debug() << "ParticleLayer shader unavailable:" << e.msg;
	}

	return particleShader;
}

struct ParticleLayerPrivate
{
	EtcTemps tmp;

	Bitmap *bitmap;

	NormValue opacity;
	BlendType blendType;
	Color *color;
	Tone *tone;

	float x, y;
	bool snap;
	bool smooth;

	int cellW, cellH, cellOX, cellOY;

	ParticleCore::Store store;

	ColorQuadArray qArray;
	size_t iboQuads;
	int drawn;
	bool dirty;

	Scene::Geometry sceneGeo;

	ParticleLayerPrivate()
	    : bitmap(0),
	      opacity(255),
	      blendType(BlendNormal),
	      color(&tmp.color),
	      tone(&tmp.tone),
	      x(0), y(0),
	      snap(true),
	      smooth(false),
	      cellW(0), cellH(0), cellOX(-1), cellOY(-1),
	      iboQuads(0),
	      drawn(0),
	      dirty(true)
	{
		sceneGeo.rect = IntRect(0, 0, 0, 0);
		sceneGeo.orig = Vec2i(0, 0);
	}

	void rebuild()
	{
		dirty = false;
		drawn = 0;

		if (nullOrDisposed(bitmap) || store.count() == 0)
			return;

		const int bw = bitmap->width();
		const int bh = bitmap->height();
		const int cw = cellW > 0 ? cellW : bw;
		const int ch = cellH > 0 ? cellH : bh;

		ParticleCore::BuildParams bp;
		bp.cellW = (float) cw;
		bp.cellH = (float) ch;
		bp.originX = (float) (cellOX >= 0 ? cellOX : cw / 2);
		bp.originY = (float) (cellOY >= 0 ? cellOY : ch / 2);
		bp.cols = bw / cw;
		bp.cellCount = bp.cols * (bh / ch);
		bp.offsetX = x;
		bp.offsetY = y;
		bp.alpha = opacity.norm;
		bp.snap = snap;
		bp.cull = sceneGeo.rect.w > 0 && sceneGeo.rect.h > 0;
		bp.cullX0 = (float) sceneGeo.orig.x;
		bp.cullY0 = (float) sceneGeo.orig.y;
		bp.cullX1 = (float) (sceneGeo.orig.x + sceneGeo.rect.w);
		bp.cullY1 = (float) (sceneGeo.orig.y + sceneGeo.rect.h);

		qArray.vertices.resize((size_t) store.count() * 4);
		drawn = store.build(reinterpret_cast<float *>(dataPtr(qArray.vertices)), bp);

		if (drawn == 0)
			return;

		/* Re-specifying the whole store each frame lets tiled GPUs
		 * hand out a fresh buffer instead of waiting on the last frame */
		VBO::bind(qArray.vbo);
		VBO::uploadData((GLsizeiptr) drawn * 4 * sizeof(Vertex), dataPtr(qArray.vertices), GL_STREAM_DRAW);
		VBO::unbind();

		if ((size_t) drawn > iboQuads)
		{
			shState->ensureQuadIBO(drawn);
			iboQuads = drawn;
		}

		qArray.quadCount = drawn;
	}
};

static_assert(sizeof(Vertex) == ParticleCore::VertexFloats * sizeof(float),
              "ParticleCore writes Vertex as 8 packed floats");

ParticleLayer::ParticleLayer(Viewport *viewport)
    : ViewportElement(viewport)
{
	p = new ParticleLayerPrivate();

	onGeometryChange(scene->getGeometry());
}

ParticleLayer::~ParticleLayer()
{
	dispose();
}

DEF_ATTR_RD_SIMPLE(ParticleLayer, Bitmap,    Bitmap*, p->bitmap)
DEF_ATTR_RD_SIMPLE(ParticleLayer, Opacity,   int,     p->opacity.unNorm)
DEF_ATTR_RD_SIMPLE(ParticleLayer, BlendType, int,     p->blendType)
DEF_ATTR_RD_SIMPLE(ParticleLayer, X,         float,   p->x)
DEF_ATTR_RD_SIMPLE(ParticleLayer, Y,         float,   p->y)
DEF_ATTR_RD_SIMPLE(ParticleLayer, PixelSnap, bool,    p->snap)

DEF_ATTR_SIMPLE(ParticleLayer, Smooth, bool,    p->smooth)
DEF_ATTR_SIMPLE(ParticleLayer, Color,  Color&, *p->color)
DEF_ATTR_SIMPLE(ParticleLayer, Tone,   Tone&,  *p->tone)

void ParticleLayer::setBitmap(Bitmap *value)
{
	guardDisposed();

	if (value)
		value->ensureNonMega();

	p->bitmap = value;
	p->dirty = true;
}

void ParticleLayer::setOpacity(int value)
{
	guardDisposed();

	if (p->opacity.unNorm == clamp(value, 0, 255))
		return;

	p->opacity = value;
	p->dirty = true;
}

void ParticleLayer::setBlendType(int value)
{
	guardDisposed();

	switch (value)
	{
	default :
	case BlendNormal :
		p->blendType = BlendNormal;
		return;
	case BlendAddition :
		p->blendType = BlendAddition;
		return;
	case BlendSubstraction :
		p->blendType = BlendSubstraction;
		return;
	}
}

void ParticleLayer::setX(float value)
{
	guardDisposed();

	if (p->x == value || !ParticleCore::finitef(value))
		return;

	p->x = value;
	p->dirty = true;
}

void ParticleLayer::setY(float value)
{
	guardDisposed();

	if (p->y == value || !ParticleCore::finitef(value))
		return;

	p->y = value;
	p->dirty = true;
}

void ParticleLayer::setPixelSnap(bool value)
{
	guardDisposed();

	p->snap = value;
	p->dirty = true;
}

void ParticleLayer::setCell(int w, int h, int ox, int oy)
{
	guardDisposed();

	p->cellW = w > 0 ? w : 0;
	p->cellH = h > 0 ? h : 0;
	p->cellOX = ox;
	p->cellOY = oy;
	p->dirty = true;
}

int ParticleLayer::getCellWidth() const
{
	guardDisposed();
	return p->cellW;
}

int ParticleLayer::getCellHeight() const
{
	guardDisposed();
	return p->cellH;
}

int ParticleLayer::getCellOX() const
{
	guardDisposed();
	return p->cellOX >= 0 ? p->cellOX : p->cellW / 2;
}

int ParticleLayer::getCellOY() const
{
	guardDisposed();
	return p->cellOY >= 0 ? p->cellOY : p->cellH / 2;
}

int ParticleLayer::setParticles(const float *state, int stateStride,
                                const float *motion, int motionStride, int n)
{
	guardDisposed();

	p->dirty = true;
	return p->store.load(state, stateStride, motion, motionStride, n, false);
}

int ParticleLayer::addParticles(const float *state, int stateStride,
                                const float *motion, int motionStride, int n)
{
	guardDisposed();

	p->dirty = true;
	return p->store.load(state, stateStride, motion, motionStride, n, true);
}

void ParticleLayer::clearParticles()
{
	guardDisposed();

	p->store.clear();
	p->dirty = true;
}

int ParticleLayer::update(float frames)
{
	guardDisposed();

	p->dirty = true;
	return p->store.update(frames);
}

int ParticleLayer::getCount() const
{
	guardDisposed();
	return p->store.count();
}

int ParticleLayer::getDrawnCount() const
{
	guardDisposed();
	return p->drawn;
}

void ParticleLayer::exportParticles(float *out) const
{
	guardDisposed();
	p->store.exportState(out);
}

void ParticleLayer::initDynAttribs()
{
	p->color = new Color;
	p->tone = new Tone;
}

bool ParticleLayer::available()
{
	return getParticleShader() != 0;
}

void ParticleLayer::draw()
{
	if (nullOrDisposed(p->bitmap) || !p->opacity || p->store.count() == 0)
		return;

	ParticleShader *shader = getParticleShader();
	if (!shader)
		return;

	if (p->dirty)
		p->rebuild();

	if (p->drawn == 0)
		return;

	shader->bind();
	shader->applyViewportProj();
	shader->setTranslation(p->sceneGeo.offset());
	shader->setTone(p->tone->norm);
	shader->setColor(p->color->norm);

	glState.blendMode.pushSet(p->blendType);

	p->bitmap->bindTex(*shader);

	if (p->smooth)
	{
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	p->qArray.draw(0, p->drawn);

	if (p->smooth)
	{
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	glState.blendMode.pop();
}

void ParticleLayer::onGeometryChange(const Scene::Geometry &geo)
{
	p->sceneGeo = geo;
	p->dirty = true;
}

void ParticleLayer::releaseResources()
{
	unlink();

	delete p;
}
