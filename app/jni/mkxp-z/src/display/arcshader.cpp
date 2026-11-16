/*
** arcshader.cpp
**
** This file is part of mkxp.
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "arcshader.h"

#include "bitmap.h"
#include "debugwriter.h"
#include "disposable.h"
#include "exception.h"
#include "gl-util.h"
#include "glstate.h"
#include "quad.h"
#include "shader.h"
#include "util.h"

#include <SDL_rect.h>

#include <algorithm>
#include <map>
#include <math.h>
#include <string.h>
#include <vector>

namespace
{

enum Variant
{
	VarSprite,
	VarMaterial,
	VarPost
};

enum EngineUniform
{
	U_Texel,
	U_Size,
	U_Rect,
	U_SceneTexel,
	U_SceneRect,
	U_SceneXform,
	U_Scene,
	U_Count
};

const char *const engineNames[U_Count] =
{
	"arcTexel", "arcSize", "arcRect", "arcSceneTexel", "arcSceneRect", "arcSceneXform", "arcScene"
};

/* Float uniforms owned by the templates (the "arc" and "material" prefixes are reserved too) */
const char *const templateFloats[] =
{
	"texSizeInv", "patternSizeInv", "patternScroll", "patternZoom", "tone", "opacity", "color",
	"bushDepth", "bushOpacity", "bushSlope", "bushIntercept", "patternOpacity", "translation", 0
};

bool isTemplateUniform(const std::string &name)
{
	if (name.compare(0, 3, "arc") == 0 || name.compare(0, 8, "material") == 0)
		return true;

	for (int i = 0; templateFloats[i]; ++i)
		if (name == templateFloats[i])
			return true;

	return false;
}

int typeComps(GLenum type)
{
	switch (type)
	{
	case GL_FLOAT_VEC2: return 2;
	case GL_FLOAT_VEC3: return 3;
	case GL_FLOAT_VEC4: return 4;
	default:            return 1;
	}
}

struct UserUniform
{
	std::string name;
	GLenum type;
	GLint size;
	GLint loc;
};

struct Program
{
	ShaderBase *shader;
	bool failed;
	std::string log;
	GLint engine[U_Count];
	std::vector<UserUniform> floats;
	std::map<std::string, GLint> samplers;

	/* Whose values the program currently holds */
	unsigned long long owner;
	unsigned long long stamp;

	/* The material template already uses units 2..5 */
	int unitShift;

	Program()
	    : shader(0), failed(false), owner(0), stamp(0), unitShift(0)
	{
		for (int i = 0; i < U_Count; ++i)
			engine[i] = -1;
	}

	GLint samplerLoc(const std::string &name)
	{
		std::map<std::string, GLint>::iterator it = samplers.find(name);
		if (it != samplers.end())
			return it->second;

		GLint loc = gl.GetUniformLocation(shader->glProgram(), name.c_str());
		samplers[name] = loc;
		return loc;
	}
};

typedef std::map<std::string, Program*> ProgramCache;

ProgramCache &programCache()
{
	static ProgramCache cache;
	return cache;
}

unsigned long long counter;

GLint maxTextureUnits()
{
	static GLint units = 0;

	if (units == 0)
	{
		gl.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
		if (units <= 0)
			units = 8;
	}

	return units;
}

void collectUniforms(Program &pr)
{
	GLuint prog = pr.shader->glProgram();

	for (int i = 0; i < U_Count; ++i)
		pr.engine[i] = gl.GetUniformLocation(prog, engineNames[i]);

	GLint count = 0, maxLen = 0;
	gl.GetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
	gl.GetProgramiv(prog, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLen);

	std::vector<GLchar> buf(std::max(maxLen, 1) + 1, 0);

	for (GLint i = 0; i < count; ++i)
	{
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		gl.GetActiveUniform(prog, i, (GLsizei) buf.size(), &len, &size, &type, &buf[0]);

		std::string name(&buf[0], len);
		size_t bracket = name.find('[');
		if (bracket != std::string::npos)
			name.resize(bracket);

		if (type != GL_FLOAT && type != GL_FLOAT_VEC2 &&
		    type != GL_FLOAT_VEC3 && type != GL_FLOAT_VEC4)
			continue;

		if (isTemplateUniform(name))
			continue;

		UserUniform u;
		u.name = name;
		u.type = type;
		u.size = size;
		u.loc = gl.GetUniformLocation(prog, name.c_str());

		if (u.loc >= 0)
			pr.floats.push_back(u);
	}
}

Program *getProgram(int variant, const std::string &source)
{
	std::string key(1, (char) ('0' + variant));
	key += source;

	ProgramCache &cache = programCache();
	ProgramCache::iterator it = cache.find(key);
	if (it != cache.end())
		return it->second;

	Program *pr = new Program;
	pr->unitShift = (variant == VarMaterial) ? 4 : 0;

	try
	{
		Shader::UserSplice splice(source);

		switch (variant)
		{
		case VarSprite:
			pr->shader = new SpriteShader();
			break;
		case VarMaterial:
			pr->shader = new SpriteMaterialShader();
			break;
		default:
			pr->shader = new ARCPostShader();
			break;
		}
	}
	catch (const Exception &e)
	{
		pr->failed = true;
		pr->log = std::string(e.msg.c_str()) + "\n" + Shader::lastLog();
		Debug() << "ARCShader:" << pr->log;
	}

	if (pr->shader)
		collectUniforms(*pr);

	cache[key] = pr;
	return pr;
}

struct PostState
{
	TEX::ID scene;
	Vec2i sceneSize;
	TEXFBO aux[2];
	bool auxInit;
	Quad *quad;

	PostState()
	    : auxInit(false), quad(0)
	{}
};

PostState post;

void setLinear()
{
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

/* Storage only grows, so several shaded viewports per frame do not reallocate */
void growTex(TEX::ID tex, Vec2i &size, int w, int h)
{
	TEX::bind(tex);

	if (size.x >= w && size.y >= h)
		return;

	size = Vec2i(std::max(size.x, w), std::max(size.y, h));
	TEX::allocEmpty(size.x, size.y);
}

Vec4 regionRect(const IntRect &r, const Vec2i &alloc)
{
	/* Half a texel inside so linear reads never leave the region */
	const float ix = 1.f / alloc.x, iy = 1.f / alloc.y;
	return Vec4((r.x + 0.5f) * ix, (r.y + 0.5f) * iy,
	            (r.x + r.w - 0.5f) * ix, (r.y + r.h - 0.5f) * iy);
}

} // namespace

struct ARCShaderPrivate
{
	struct Value
	{
		int comps;
		int count;
		float data[ARCShader::MaxArray];
	};

	struct TexSlot
	{
		std::string name;
		Bitmap *bitmap;

		TexSlot() : bitmap(0) {}
	};

	int kind;
	std::string source;
	std::string log;
	Program *main;
	Program *material;
	bool materialTried;
	std::map<std::string, Value> values;
	TexSlot tex[ARCShader::LastUnit - ARCShader::FirstUnit + 1];
	float scale;
	unsigned long long id;
	unsigned long long stamp;
	bool unitWarned;

	void touch()
	{
		stamp = ++counter;
	}

	void upload(Program &pr)
	{
		if (pr.owner != id || pr.stamp != stamp)
		{
			for (size_t i = 0; i < pr.floats.size(); ++i)
			{
				const UserUniform &u = pr.floats[i];
				const int comps = typeComps(u.type);
				const int n = (comps == 1) ? std::min<int>(u.size, ARCShader::MaxArray) : 1;

				float tmp[ARCShader::MaxArray];
				memset(tmp, 0, sizeof(tmp));

				std::map<std::string, Value>::const_iterator it = values.find(u.name);
				if (it != values.end())
				{
					const int have = it->second.comps * it->second.count;
					memcpy(tmp, it->second.data, sizeof(float) * std::min(have, n * comps));
				}

				switch (comps)
				{
				case 1: gl.Uniform1fv(u.loc, n, tmp); break;
				case 2: gl.Uniform2f(u.loc, tmp[0], tmp[1]); break;
				case 3: gl.Uniform3f(u.loc, tmp[0], tmp[1], tmp[2]); break;
				default: gl.Uniform4f(u.loc, tmp[0], tmp[1], tmp[2], tmp[3]); break;
				}
			}

			pr.owner = id;
			pr.stamp = stamp;
		}

		bool bound = false;

		for (int i = 0; i <= ARCShader::LastUnit - ARCShader::FirstUnit; ++i)
		{
			const TexSlot &t = tex[i];
			if (t.name.empty())
				continue;

			GLint loc = pr.samplerLoc(t.name);
			if (loc < 0)
				continue;

			const int unit = ARCShader::FirstUnit + i + pr.unitShift;
			if (unit >= maxTextureUnits())
			{
				if (!unitWarned)
					Debug() << "ARCShader: texture" << t.name << "needs unit" << unit << "beyond the device limit";
				unitWarned = true;
				continue;
			}

			TEX::ID id = nullOrDisposed(t.bitmap) ? TEX::ID(0) : t.bitmap->getGLTypes().tex;

			gl.ActiveTexture(GL_TEXTURE0 + unit);
			TEX::bind(id);
			gl.Uniform1i(loc, unit);
			bound = true;
		}

		if (bound)
			gl.ActiveTexture(GL_TEXTURE0);
	}
};

ARCShader::ARCShader(const char *source, int length, int kind)
{
	p = new ARCShaderPrivate;
	p->kind = (kind == KindPost) ? KindPost : KindSprite;
	p->source.assign(source, length);
	p->material = 0;
	p->materialTried = false;
	p->scale = 1.f;
	p->unitWarned = false;
	p->id = ++counter;
	p->stamp = ++counter;

	p->main = getProgram(p->kind == KindPost ? VarPost : VarSprite, p->source);
	p->log = p->main->log;
}

ARCShader::~ARCShader()
{
	delete p;
}

bool ARCShader::ok() const
{
	return !p->main->failed;
}

const std::string &ARCShader::log() const
{
	return p->log;
}

int ARCShader::kind() const
{
	return p->kind;
}

void ARCShader::setFloats(const char *name, const float *values, int comps, int count)
{
	ARCShaderPrivate::Value &v = p->values[name];
	v.comps = clamp(comps, 1, 4);
	v.count = (v.comps == 1) ? clamp(count, 1, (int) MaxArray) : 1;

	memset(v.data, 0, sizeof(v.data));
	memcpy(v.data, values, sizeof(float) * v.comps * v.count);

	p->touch();
}

void ARCShader::setTexture(const char *name, Bitmap *bitmap, int unit)
{
	unit = clamp(unit, (int) FirstUnit, (int) LastUnit);

	/* one name lives in one unit */
	for (int i = 0; i <= LastUnit - FirstUnit; ++i)
		if (p->tex[i].name == name)
			p->tex[i] = ARCShaderPrivate::TexSlot();

	if (!bitmap)
		return;

	bitmap->ensureNonMega();

	ARCShaderPrivate::TexSlot &t = p->tex[unit - FirstUnit];
	t.name = name;
	t.bitmap = bitmap;
}

float ARCShader::getScale() const
{
	return p->scale;
}

void ARCShader::setScale(float value)
{
	p->scale = clamp(value, 1.f / 16.f, 1.f);
}

SpriteShader *ARCShader::spriteShader()
{
	if (p->kind != KindSprite || p->main->failed)
		return 0;

	return static_cast<SpriteShader*>(p->main->shader);
}

SpriteMaterialShader *ARCShader::materialShader()
{
	if (p->kind != KindSprite || p->main->failed)
		return 0;

	if (!p->materialTried)
	{
		p->materialTried = true;
		p->material = getProgram(VarMaterial, p->source);

		if (p->material->failed)
			p->log += p->material->log;
	}

	return p->material->failed ? 0 : static_cast<SpriteMaterialShader*>(p->material->shader);
}

void ARCShader::applySprite(ShaderBase &bound, const Vec2 &texel,
                            const Vec2 &size, const Vec4 &rect)
{
	Program *pr = 0;

	if (&bound == p->main->shader)
		pr = p->main;
	else if (p->material && &bound == p->material->shader)
		pr = p->material;

	if (!pr)
		return;

	gl.Uniform2f(pr->engine[U_Texel], texel.x, texel.y);
	gl.Uniform2f(pr->engine[U_Size], size.x, size.y);
	gl.Uniform4f(pr->engine[U_Rect], rect.x, rect.y, rect.z, rect.w);

	p->upload(*pr);
}

void ARCShader::postProcess(ARCShader *const *passes, int count,
                            const IntRect &rect, const IntRect &screen,
                            const TEXFBO &target)
{
	ARCShader *list[MaxPasses];
	int n = 0;

	for (int i = 0; i < count && n < MaxPasses; ++i)
		if (passes[i] && passes[i]->p->kind == KindPost && !passes[i]->p->main->failed)
			list[n++] = passes[i];

	if (n == 0)
		return;

	SDL_Rect a = { rect.x, rect.y, rect.w, rect.h };
	SDL_Rect b = { screen.x, screen.y, screen.w, screen.h };
	SDL_Rect c;
	if (!SDL_IntersectRect(&a, &b, &c))
		return;

	const IntRect r(c.x, c.y, c.w, c.h);

	if (!post.quad)
	{
		post.quad = new Quad;
		post.scene = TEX::gen();
		TEX::bind(post.scene);
		TEX::setRepeat(false);
		setLinear();
	}

	/* Copy what is drawn under the viewport (a native blit may have left another read framebuffer bound) */
	growTex(post.scene, post.sceneSize, r.w, r.h);
	if (gl.BlitFramebuffer)
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, target.fbo.gl);
	gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, r.x, r.y, r.w, r.h);

	glState.scissorTest.pushSet(false);
	glState.blend.pushSet(false);

	const IntRect sceneRegion(0, 0, r.w, r.h);
	const Vec4 sceneRect = regionRect(sceneRegion, post.sceneSize);
	const Vec2 sceneTexel(1.f / post.sceneSize.x, 1.f / post.sceneSize.y);

	TEX::ID inTex = post.scene;
	Vec2i inAlloc = post.sceneSize;
	IntRect inRegion = sceneRegion;

	for (int i = 0; i < n; ++i)
	{
		const bool last = (i == n - 1);
		IntRect outPos;

		if (last)
		{
			FBO::bind(target.fbo);
			outPos = r;
		}
		else
		{
			TEXFBO &aux = post.aux[i & 1];

			if (!post.auxInit)
			{
				for (int k = 0; k < 2; ++k)
				{
					TEXFBO::init(post.aux[k]);
					setLinear();
				}
				post.auxInit = true;
			}

			const float s = list[i]->p->scale;
			const int w = std::max(1, (int) lround(r.w * s));
			const int h = std::max(1, (int) lround(r.h * s));

			if (aux.width < w || aux.height < h)
			{
				TEXFBO::allocEmpty(aux, std::max(aux.width, w), std::max(aux.height, h));
				TEXFBO::linkFBO(aux);
			}

			FBO::bind(aux.fbo);
			glState.viewport.pushSet(IntRect(0, 0, aux.width, aux.height));
			outPos = IntRect(0, 0, w, h);
		}

		Program &pr = *list[i]->p->main;
		ShaderBase &shader = *pr.shader;

		shader.bind();
		shader.applyViewportProj();
		shader.setTexSize(inAlloc);

		const float sx = (float) r.w / (outPos.w * post.sceneSize.x);
		const float sy = (float) r.h / (outPos.h * post.sceneSize.y);
		const Vec4 inRect = regionRect(inRegion, inAlloc);

		gl.Uniform2f(pr.engine[U_Texel], 1.f / inAlloc.x, 1.f / inAlloc.y);
		gl.Uniform2f(pr.engine[U_Size], (float) outPos.w, (float) outPos.h);
		gl.Uniform4f(pr.engine[U_Rect], inRect.x, inRect.y, inRect.z, inRect.w);
		gl.Uniform2f(pr.engine[U_SceneTexel], sceneTexel.x, sceneTexel.y);
		gl.Uniform4f(pr.engine[U_SceneRect], sceneRect.x, sceneRect.y, sceneRect.z, sceneRect.w);
		gl.Uniform4f(pr.engine[U_SceneXform], sx, sy, -outPos.x * sx, -outPos.y * sy);

		gl.ActiveTexture(GL_TEXTURE1);
		TEX::bind(post.scene);
		gl.Uniform1i(pr.engine[U_Scene], 1);
		gl.ActiveTexture(GL_TEXTURE0);

		list[i]->p->upload(pr);

		TEX::bind(inTex);

		post.quad->setTexPosRect(FloatRect(inRegion), FloatRect(outPos));
		post.quad->draw();

		if (!last)
		{
			glState.viewport.pop();

			inTex = post.aux[i & 1].tex;
			inAlloc = Vec2i(post.aux[i & 1].width, post.aux[i & 1].height);
			inRegion = outPos;
		}
	}

	glState.blend.pop();
	glState.scissorTest.pop();
}

bool ARCShader::available()
{
	static int state = -1;

	if (state < 0)
	{
		static const char identity[] = "vec4 arcUser(vec4 c, vec2 uv) { return c; }";
		ARCShader probe(identity, sizeof(identity) - 1, KindSprite);
		ARCShader probePost(identity, sizeof(identity) - 1, KindPost);
		state = (probe.ok() && probePost.ok()) ? 1 : 0;
	}

	return state == 1;
}
