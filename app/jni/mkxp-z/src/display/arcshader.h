/*
** arcshader.h
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

#ifndef ARCSHADER_H
#define ARCSHADER_H

#include "etc-internal.h"

#include <string>

class Bitmap;
class ShaderBase;
class SpriteShader;
class SpriteMaterialShader;
struct TEXFBO;
struct ARCShaderPrivate;

/* User fragment shader. The source defines
 *   vec4 arcUser(vec4 c, vec2 uv)
 * and is compiled into an engine template: Sprite (after pattern/material,
 * before tone/opacity/color/flash/invert/bush) or Post (Viewport pass). */
class ARCShader
{
public:
	enum Kind
	{
		KindSprite = 0,
		KindPost   = 1
	};

	enum
	{
		MaxArray   = 64,
		FirstUnit  = 2,
		LastUnit   = 5,
		MaxPasses  = 4,
		MaxPad     = 256
	};

	ARCShader(const char *source, int length, int kind);
	~ARCShader();

	bool ok() const;
	const std::string &log() const;
	int kind() const;

	/* comps 1..4; count > 1 only for comps == 1 (float array) */
	void setFloats(const char *name, const float *values, int comps, int count);
	/* bitmap 0 removes the binding of that name */
	void setTexture(const char *name, Bitmap *bitmap, int unit);

	/* Size of a non-final Viewport pass relative to the viewport */
	float getScale() const;
	void setScale(float value);

	/* Engine side */
	SpriteShader *spriteShader();
	SpriteMaterialShader *materialShader();

	/* After the template uniforms are set on the bound program */
	void applySprite(ShaderBase &bound, const Vec2 &texel,
	                 const Vec2 &size, const Vec4 &rect);

	/* Runs the passes over 'rect' of the bound screen target */
	static void postProcess(ARCShader *const *passes, int count,
	                        const IntRect &rect, const IntRect &screen,
	                        const TEXFBO &target);

	static bool available();

private:
	ARCShaderPrivate *p;
};

#endif // ARCSHADER_H
