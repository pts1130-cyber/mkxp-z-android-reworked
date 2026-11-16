/*
 ** sprite.cpp
 **
 ** This file is part of mkxp.
 **
 ** Copyright (C) 2013 Jonas Kulla <Nyocurio@gmail.com>
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

#include "sprite.h"

#include "sharedstate.h"
#include "bitmap.h"
#include "etc.h"
#include "etc-internal.h"
#include "util.h"

#include "gl-util.h"
#include "quad.h"
#include "transform.h"
#include "shader.h"
#include "arcshader.h"
#include "debugwriter.h"
#include "exception.h"
#include "glstate.h"
#include "quadarray.h"
#include "meshwarp.h"

#include <algorithm>

#include <math.h>
#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

#include <SDL_rect.h>

#include "sigslot/signal.hpp"

struct SpritePrivate
{
    Bitmap *bitmap;
    
    Quad quad;
    Transform trans;
    
    Rect *srcRect;
    sigslot::connection srcRectCon;
    
    bool mirrored;
    int bushDepth;
    float efBushDepth;
    NormValue bushOpacity;
    NormValue opacity;
    BlendType blendType;
    
    Bitmap *pattern;
    BlendType patternBlendType;
    bool patternTile;
    NormValue patternOpacity;
    Vec2 patternScroll;
    Vec2 patternZoom;
    
    bool invert;
    
    ARCShader *shader;
    int shaderPad;
    bool padDirty;
    Quad padQuad;

    Bitmap *materialMap;
    Bitmap *materialEnv;
    Bitmap *materialRamp;
    int materialMode;
    Rect *materialRect;
    Rect materialRectTmp;
    float materialParams[Sprite::MaterialParamCount];
    
    IntRect sceneRect;
    Vec2i sceneOrig;
    
    /* Would this sprite be visible on
     * the screen if drawn? */
    bool isVisible;
    
    Color *color;
    Tone *tone;
    
    struct
    {
        int amp;
        int length;
        int speed;
        float phase;
        
        /* Wave effect is active (amp != 0) */
        bool active;
        /* qArray needs updating */
        bool dirty;
        SimpleQuadArray qArray;
    } wave;
    
    MeshWarp mesh;
    SimpleQuadArray *meshArray;
    FloatRect meshSrc;
    int meshDot;
    MeshDot dotArea;
    TEXFBO dotTex;
    Quad *dotQuad;
    float dotMat[16];
    
    EtcTemps tmp;
    
    sigslot::connection prepareCon;
    
    SpritePrivate()
    : bitmap(0),
    srcRect(&tmp.rect),
    mirrored(false),
    bushDepth(0),
    efBushDepth(0),
    bushOpacity(128),
    opacity(255),
    blendType(BlendNormal),
    pattern(0),
    patternTile(true),
    patternOpacity(255),
    invert(false),
    shader(0),
    shaderPad(0),
    padDirty(true),
    materialMap(0),
    materialEnv(0),
    materialRamp(0),
    materialMode(0),
    materialRect(&materialRectTmp),
    meshArray(0),
    meshDot(0),
    dotQuad(0),
    isVisible(false),
    color(&tmp.color),
    tone(&tmp.tone)
    
    {
        sceneRect.x = sceneRect.y = 0;
        
        updateSrcRectCon();
        
        prepareCon = shState->prepareDraw.connect
        (&SpritePrivate::prepare, this);
        
        patternScroll = Vec2(0,0);
        patternZoom = Vec2(1, 1);
        
        for (int i = 0; i < Sprite::MaterialParamCount; ++i)
            materialParams[i] = 0.f;
        
        wave.amp = 0;
        wave.length = 180;
        wave.speed = 360;
        wave.phase = 0.0f;
        wave.dirty = false;
    }
    
    ~SpritePrivate()
    {
        srcRectCon.disconnect();
        prepareCon.disconnect();
        releaseDot();
        delete meshArray;
    }
    
    void recomputeBushDepth()
    {
        if (nullOrDisposed(bitmap))
            return;
        
        /* Calculate effective (normalized) bush depth */
        float texBushDepth = (bushDepth / trans.getScale().y) -
        (srcRect->y + srcRect->height) +
        bitmap->height();
        
        efBushDepth = 1.0f - texBushDepth / bitmap->height();
    }
    
    void onSrcRectChange()
    {
        FloatRect rect = srcRect->toFloatRect();
        Vec2i bmSize;
        
        if (!nullOrDisposed(bitmap))
            bmSize = Vec2i(bitmap->width(), bitmap->height());
        
        /* Clamp the rectangle so it doesn't reach outside
         * the bitmap bounds */
        rect.w = clamp<int>(rect.w, 0, bmSize.x-rect.x);
        rect.h = clamp<int>(rect.h, 0, bmSize.y-rect.y);
        
        quad.setTexRect(mirrored ? rect.hFlipped() : rect);
        
        quad.setPosRect(FloatRect(0, 0, rect.w, rect.h));
        recomputeBushDepth();
        
        meshSrc = rect;
        mesh.dirty = true;
        wave.dirty = true;
        padDirty = true;
    }
    
    void updateSrcRectCon()
    {
        /* Cut old connection */
        srcRectCon.disconnect();
        /* Create new one */
        srcRectCon = srcRect->valueChanged.connect
        (&SpritePrivate::onSrcRectChange, this);
    }
    
    void updateVisibility()
    {
        isVisible = false;
        
        if (nullOrDisposed(bitmap))
            return;
        
        if (!opacity)
            return;
        
        if (wave.active || meshDrawn())
        {
            /* Don't do expensive wave bounding box
             * calculations */
            isVisible = true;
            return;
        }
        
        /* Compare sprite bounding box against the scene */
        
        /* If sprite is zoomed/rotated, just opt out for now
         * for simplicity's sake */
        const Vec2 &scale = trans.getScale();
        if (scale.x != 1 || scale.y != 1 || trans.getRotation() != 0)
        {
            isVisible = true;
            return;
        }
        
        IntRect self;
        self.setPos(trans.getPositionI() - (trans.getOriginI() + sceneOrig));
        self.w = bitmap->width();
        self.h = bitmap->height();
        
        if (shader && shaderPad > 0)
        {
            self.x -= shaderPad;
            self.y -= shaderPad;
            self.w += shaderPad * 2;
            self.h += shaderPad * 2;
        }
        
        isVisible = SDL_HasIntersection(&self, &sceneRect);
    }
    
    void emitWaveChunk(SVertex *&vert, float phase, int width,
                       float zoomY, int chunkY, int chunkLength)
    {
        float wavePos = phase + (chunkY / (float) wave.length) * (float) (M_PI * 2);
        float chunkX = sin(wavePos) * wave.amp;
        
        FloatRect tex(srcRect->x, srcRect->y + chunkY / zoomY, width, chunkLength / zoomY);
        FloatRect pos(chunkX, chunkY / zoomY, width, chunkLength / zoomY);
        
        Quad::setTexPosRect(vert, mirrored ? tex.hFlipped() : tex, pos);
        vert += 4;
    }
    
    void updateWave()
    {
        if (nullOrDisposed(bitmap))
            return;
        
        if (wave.amp == 0)
        {
            wave.active = false;
            return;
        }
        
        wave.active = true;
        
        int width = srcRect->width;
        int height = srcRect->height;
        float zoomY = trans.getScale().y;
        
        if (wave.amp < -(width / 2))
        {
            wave.qArray.resize(0);
            wave.qArray.commit();
            
            return;
        }
        
        /* RMVX does this, and I have no fucking clue why */
        if (wave.amp < 0)
        {
            wave.qArray.resize(1);
            
            int x = -wave.amp;
            int w = width - x * 2;
            
            FloatRect tex(x, srcRect->y, w, srcRect->height);
            
            Quad::setTexPosRect(&wave.qArray.vertices[0], tex, tex);
            wave.qArray.commit();
            
            return;
        }
        
        /* The length of the sprite as it appears on screen */
        int visibleLength = height * zoomY;
        
        /* First chunk length (aligned to 8 pixel boundary */
        int firstLength = ((int) trans.getPosition().y) % 8;
        
        /* Amount of full 8 pixel chunks in the middle */
        int chunks = (visibleLength - firstLength) / 8;
        
        /* Final chunk length */
        int lastLength = (visibleLength - firstLength) % 8;
        
        wave.qArray.resize(!!firstLength + chunks + !!lastLength);
        SVertex *vert = &wave.qArray.vertices[0];
        
        float phase = (wave.phase * (float) M_PI) / 180.0f;
        
        if (firstLength > 0)
            emitWaveChunk(vert, phase, width, zoomY, 0, firstLength);
        
        for (int i = 0; i < chunks; ++i)
            emitWaveChunk(vert, phase, width, zoomY, firstLength + i * 8, 8);
        
        if (lastLength > 0)
            emitWaveChunk(vert, phase, width, zoomY, firstLength + chunks * 8, lastLength);
        
        wave.qArray.commit();
    }
    
    /* Source rect (clamped to the bitmap) in the units of the quad */
    FloatRect userSrcRect() const
    {
        FloatRect rect = srcRect->toFloatRect();
        rect.w = clamp<int>(rect.w, 0, bitmap->width() - rect.x);
        rect.h = clamp<int>(rect.h, 0, bitmap->height() - rect.y);
        return rect;
    }

    void applyUser(ARCShader *user, ShaderBase &s)
    {
        const FloatRect rect = userSrcRect();
        const float bw = (float) bitmap->width();
        const float bh = (float) bitmap->height();
        const TEXFBO &tb = bitmap->getGLTypes();
        const Vec2 texel(1.f / tb.width, 1.f / tb.height);

        user->applySprite(s, texel, Vec2(bw, bh),
                          Vec4(rect.x / bw, rect.y / bh, (rect.x + rect.w) / bw, (rect.y + rect.h) / bh));
    }

    void drawPadded()
    {
        if (padDirty)
        {
            const FloatRect rect = userSrcRect();
            const float pd = (float) shaderPad;
            FloatRect tex(rect.x - pd, rect.y - pd, rect.w + pd * 2, rect.h + pd * 2);

            padQuad.setTexPosRect(mirrored ? tex.hFlipped() : tex,
                                  FloatRect(-pd, -pd, rect.w + pd * 2, rect.h + pd * 2));
            padDirty = false;
        }

        padQuad.draw();
    }

    bool dotActive() const
    {
        return meshDot > 0 && mesh.active();
    }
    
    bool meshDrawn() const
    {
        return mesh.deforms() || dotActive();
    }
    
    void releaseDot()
    {
        if (dotTex.tex != TEX::ID(0))
            TEXFBO::fini(dotTex);
        dotTex = TEXFBO();
        delete dotQuad;
        dotQuad = 0;
    }
    
    /* First pass target: grows only, cleared every draw */
    GLint beginDot()
    {
        GLint prev = 0;
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
        
        if (dotTex.tex == TEX::ID(0))
        {
            TEXFBO::init(dotTex);
            TEXFBO::allocEmpty(dotTex, dotArea.w, dotArea.h);
            TEXFBO::linkFBO(dotTex);
        }
        else if (dotArea.w > dotTex.width || dotArea.h > dotTex.height)
        {
            TEXFBO::allocEmpty(dotTex, std::max(dotArea.w, dotTex.width),
                               std::max(dotArea.h, dotTex.height));
        }
        
        FBO::bind(dotTex.fbo);
        glState.viewport.pushSet(IntRect(0, 0, dotTex.width, dotTex.height));
        glState.scissorTest.pushSet(false);
        glState.blend.pushSet(false);
        glState.clearColor.pushSet(Vec4());
        FBO::clear();
        glState.clearColor.pop();
        
        return prev;
    }
    
    void endDot(GLint prev)
    {
        glState.blend.pop();
        glState.scissorTest.pop();
        glState.viewport.pop();
        gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev);
    }
    
    /* Second pass: the dot target k times larger, nearest, with the
     * sprite's transform, opacity and blend type */
    void drawDot()
    {
        ShaderBase *base;
        
        if (opacity != 255)
        {
            AlphaSpriteShader &shader = shState->shaders().alphaSprite;
            shader.bind();
            shader.setSpriteMat(trans.getMatrix());
            shader.setAlpha(opacity.norm);
            shader.applyViewportProj();
            base = &shader;
        }
        else
        {
            SimpleSpriteShader &shader = shState->shaders().simpleSprite;
            shader.bind();
            shader.setSpriteMat(trans.getMatrix());
            shader.applyViewportProj();
            base = &shader;
        }
        
        glState.blendMode.pushSet(blendType);
        TEX::bind(dotTex.tex);
        base->setTexSize(Vec2i(dotTex.width, dotTex.height));
        dotQuad->draw();
        glState.blendMode.pop();
    }
    
    /* shader_pad of a working user shader widens the mesh by a ring */
    int meshPad() const
    {
        return (shader && shader->ok()) ? shaderPad : 0;
    }
    
    void updateMesh()
    {
        mesh.dirty = false;
        
        if (!meshArray)
            meshArray = new SimpleQuadArray;
        
        const int pad = meshPad();
        const bool flip = mirrored && !dotActive();
        meshArray->resize(mesh.quadCount() + (pad ? MeshWarp::padQuadCount(mesh.cols, mesh.rows) : 0));
        mesh.build(dataPtr(meshArray->vertices), meshSrc.x, meshSrc.y,
                   meshSrc.w, meshSrc.h, flip, 1.f, 1.f);
        if (pad)
            mesh.buildPad(dataPtr(meshArray->vertices) + mesh.quadCount() * 4, (float) pad, flip, 1.f, 1.f);
        meshArray->commit();
        
        if (dotActive())
            updateDotQuad(meshSrc.w, meshSrc.h);
    }
    
    void updateDotQuad(float sw, float sh)
    {
        dotArea.fit(dataPtr(meshArray->vertices), meshArray->vertices.size(),
                    meshDot, sw, sh, glState.caps.maxTexSize);
        dotArea.targetMatrix(dotMat, meshDot);
        
        float pr[4], tr[4];
        dotArea.posRect(pr, meshDot, sw, mirrored);
        dotArea.texRect(tr, mirrored);
        
        if (!dotQuad)
            dotQuad = new Quad;
        dotQuad->setTexPosRect(FloatRect(tr[0], tr[1], tr[2], tr[3]),
                               FloatRect(pr[0], pr[1], pr[2], pr[3]));
    }
    
    void prepare()
    {
        if (wave.dirty)
        {
            updateWave();
            wave.dirty = false;
        }
        
        if (meshDrawn() && (mesh.dirty || !meshArray) && !nullOrDisposed(bitmap))
            updateMesh();
        
        updateVisibility();
    }
};

Sprite::Sprite(Viewport *viewport)
: ViewportElement(viewport)
{
    p = new SpritePrivate;
    onGeometryChange(scene->getGeometry());
}

Sprite::~Sprite()
{
    dispose();
}

DEF_ATTR_RD_SIMPLE(Sprite, Bitmap,     Bitmap*, p->bitmap)
DEF_ATTR_RD_SIMPLE(Sprite, X,          int,     p->trans.getPosition().x)
DEF_ATTR_RD_SIMPLE(Sprite, Y,          int,     p->trans.getPosition().y)
DEF_ATTR_RD_SIMPLE(Sprite, OX,         int,     p->trans.getOrigin().x)
DEF_ATTR_RD_SIMPLE(Sprite, OY,         int,     p->trans.getOrigin().y)
DEF_ATTR_RD_SIMPLE(Sprite, ZoomX,      float,   p->trans.getScale().x)
DEF_ATTR_RD_SIMPLE(Sprite, ZoomY,      float,   p->trans.getScale().y)
DEF_ATTR_RD_SIMPLE(Sprite, Angle,      float,   p->trans.getRotation())
DEF_ATTR_RD_SIMPLE(Sprite, Mirror,     bool,    p->mirrored)
DEF_ATTR_RD_SIMPLE(Sprite, BushDepth,  int,     p->bushDepth)
DEF_ATTR_RD_SIMPLE(Sprite, BlendType,  int,     p->blendType)
DEF_ATTR_RD_SIMPLE(Sprite, Pattern,    Bitmap*, p->pattern)
DEF_ATTR_RD_SIMPLE(Sprite, PatternBlendType, int, p->patternBlendType)
DEF_ATTR_RD_SIMPLE(Sprite, Width,      int,     p->srcRect->width)
DEF_ATTR_RD_SIMPLE(Sprite, Height,     int,     p->srcRect->height)
DEF_ATTR_RD_SIMPLE(Sprite, WaveAmp,    int,     p->wave.amp)
DEF_ATTR_RD_SIMPLE(Sprite, WaveLength, int,     p->wave.length)
DEF_ATTR_RD_SIMPLE(Sprite, WaveSpeed,  int,     p->wave.speed)
DEF_ATTR_RD_SIMPLE(Sprite, WavePhase,  float,   p->wave.phase)

DEF_ATTR_SIMPLE(Sprite, BushOpacity, int,     p->bushOpacity)
DEF_ATTR_SIMPLE(Sprite, Opacity,     int,     p->opacity)
DEF_ATTR_SIMPLE(Sprite, SrcRect,     Rect&,  *p->srcRect)
DEF_ATTR_SIMPLE(Sprite, Color,       Color&, *p->color)
DEF_ATTR_SIMPLE(Sprite, Tone,        Tone&,  *p->tone)
DEF_ATTR_SIMPLE(Sprite, PatternTile, bool, p->patternTile)
DEF_ATTR_SIMPLE(Sprite, PatternOpacity, int, p->patternOpacity)
DEF_ATTR_SIMPLE(Sprite, PatternScrollX, int, p->patternScroll.x)
DEF_ATTR_SIMPLE(Sprite, PatternScrollY, int, p->patternScroll.y)
DEF_ATTR_SIMPLE(Sprite, PatternZoomX, float, p->patternZoom.x)
DEF_ATTR_SIMPLE(Sprite, PatternZoomY, float, p->patternZoom.y)
DEF_ATTR_SIMPLE(Sprite, Invert,      bool,    p->invert)
DEF_ATTR_RD_SIMPLE(Sprite, Shader,    ARCShader*, p->shader)
DEF_ATTR_RD_SIMPLE(Sprite, ShaderPad, int,        p->shaderPad)

void Sprite::setShader(ARCShader *value)
{
    guardDisposed();

    if (value && value->kind() != ARCShader::KindSprite)
        throw Exception(Exception::ArgumentError, "Sprite#shader needs an ARCShader::SPRITE shader");

    if (p->shader != value)
        p->mesh.dirty = true;
    p->shader = value;
}

void Sprite::setShaderPad(int value)
{
    guardDisposed();

    value = clamp(value, 0, (int) ARCShader::MaxPad);
    if (p->shaderPad == value)
        return;

    p->shaderPad = value;
    p->padDirty = true;
    p->mesh.dirty = true;
}
DEF_ATTR_RD_SIMPLE(Sprite, MaterialMap,  Bitmap*, p->materialMap)
DEF_ATTR_RD_SIMPLE(Sprite, MaterialEnv,  Bitmap*, p->materialEnv)
DEF_ATTR_RD_SIMPLE(Sprite, MaterialRamp, Bitmap*, p->materialRamp)
DEF_ATTR_RD_SIMPLE(Sprite, MaterialMode, int,     p->materialMode)
DEF_ATTR_SIMPLE(Sprite, MaterialRect, Rect&,   *p->materialRect)

#define DEF_MATERIAL_BITMAP_SETTER(Name, field) \
void Sprite::setMaterial##Name(Bitmap *value) \
{ \
    guardDisposed(); \
    p->field = value; \
    if (!nullOrDisposed(value)) \
        value->ensureNonMega(); \
}

DEF_MATERIAL_BITMAP_SETTER(Map,  materialMap)
DEF_MATERIAL_BITMAP_SETTER(Env,  materialEnv)
DEF_MATERIAL_BITMAP_SETTER(Ramp, materialRamp)

#undef DEF_MATERIAL_BITMAP_SETTER

void Sprite::setMaterialMode(int value)
{
    guardDisposed();
    p->materialMode = clamp(value, 0, 3);
}

float Sprite::getMaterialParam(int index) const
{
    guardDisposed();
    if (index < 0 || index >= MaterialParamCount)
        return 0.f;
    return p->materialParams[index];
}

void Sprite::setMaterialParams(const float *values, int count)
{
    guardDisposed();
    for (int i = 0; i < MaterialParamCount; ++i)
        p->materialParams[i] = (i < count) ? values[i] : 0.f;
}

void Sprite::setMeshGrid(int cols, int rows)
{
    guardDisposed();
    p->mesh.setGrid(cols, rows);
    if (!p->mesh.active())
        p->releaseDot();
}

void Sprite::setMeshDot(int value)
{
    guardDisposed();
    value = clamp(value, 0, (int) MeshDot::MaxDot);
    if (p->meshDot == value)
        return;
    
    p->meshDot = value;
    if (value == 0)
        p->releaseDot();
    p->mesh.dirty = true;
}

int Sprite::getMeshDot() const
{
    guardDisposed();
    return p->meshDot;
}

int Sprite::getMeshCols() const
{
    guardDisposed();
    return p->mesh.cols;
}

int Sprite::getMeshRows() const
{
    guardDisposed();
    return p->mesh.rows;
}

int Sprite::getMeshVertexCount() const
{
    guardDisposed();
    return p->mesh.vertexCount();
}

void Sprite::setMeshOffsets(const float *values, int count)
{
    guardDisposed();
    p->mesh.setOffsets(values, count);
}

void Sprite::setMeshUVOffsets(const float *values, int count)
{
    guardDisposed();
    p->mesh.setUVOffsets(values, count);
}

bool Sprite::hasMeshUVOffsets() const
{
    guardDisposed();
    return !p->mesh.uvOffsets.empty();
}

float Sprite::getMeshOffset(int index, bool uv) const
{
    guardDisposed();
    const std::vector<float> &v = uv ? p->mesh.uvOffsets : p->mesh.offsets;
    if (index < 0 || index >= (int) v.size())
        return 0.f;
    return v[index];
}

void Sprite::setBitmap(Bitmap *bitmap)
{
    guardDisposed();
    
    if (p->bitmap == bitmap)
        return;
    
    p->bitmap = bitmap;
    
    if (nullOrDisposed(bitmap))
        return;
    
    bitmap->ensureNonMega();
    
    *p->srcRect = bitmap->rect();
    p->onSrcRectChange();
    p->quad.setPosRect(p->srcRect->toFloatRect());
    
    p->wave.dirty = true;
}

void Sprite::setX(int value)
{
    guardDisposed();
    
    if (p->trans.getPosition().x == value)
        return;
    
    p->trans.setPosition(Vec2(value, getY()));
}

void Sprite::setY(int value)
{
    guardDisposed();
    
    if (p->trans.getPosition().y == value)
        return;
    
    p->trans.setPosition(Vec2(getX(), value));
    
    if (rgssVer >= 2)
    {
        p->wave.dirty = true;
        setSpriteY(value);
    }
}

void Sprite::setOX(int value)
{
    guardDisposed();
    
    if (p->trans.getOrigin().x == value)
        return;
    
    p->trans.setOrigin(Vec2(value, getOY()));
}

void Sprite::setOY(int value)
{
    guardDisposed();
    
    if (p->trans.getOrigin().y == value)
        return;
    
    p->trans.setOrigin(Vec2(getOX(), value));
}

void Sprite::setZoomX(float value)
{
    guardDisposed();
    
    if (p->trans.getScale().x == value)
        return;
    
    p->trans.setScale(Vec2(value, getZoomY()));
}

void Sprite::setZoomY(float value)
{
    guardDisposed();
    
    if (p->trans.getScale().y == value)
        return;
    
    p->trans.setScale(Vec2(getZoomX(), value));
    p->recomputeBushDepth();
    
    if (rgssVer >= 2)
        p->wave.dirty = true;
}

void Sprite::setAngle(float value)
{
    guardDisposed();
    
    if (p->trans.getRotation() == value)
        return;
    
    p->trans.setRotation(value);
}

void Sprite::setMirror(bool mirrored)
{
    guardDisposed();
    
    if (p->mirrored == mirrored)
        return;
    
    p->mirrored = mirrored;
    p->onSrcRectChange();
}

void Sprite::setBushDepth(int value)
{
    guardDisposed();
    
    if (p->bushDepth == value)
        return;
    
    p->bushDepth = value;
    p->recomputeBushDepth();
}

void Sprite::setBlendType(int type)
{
    guardDisposed();
    
    switch (type)
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

void Sprite::setPattern(Bitmap *value)
{
    guardDisposed();
    
    if (p->pattern == value)
        return;
    
    p->pattern = value;
    
    if (!nullOrDisposed(value))
        value->ensureNonMega();
}

void Sprite::setPatternBlendType(int type)
{
    guardDisposed();
    
    switch (type)
    {
        default :
        case BlendNormal :
            p->patternBlendType = BlendNormal;
            return;
        case BlendAddition :
            p->patternBlendType = BlendAddition;
            return;
        case BlendSubstraction :
            p->patternBlendType = BlendSubstraction;
            return;
    }
}

#define DEF_WAVE_SETTER(Name, name, type) \
void Sprite::setWave##Name(type value) \
{ \
guardDisposed(); \
if (p->wave.name == value) \
return; \
p->wave.name = value; \
p->wave.dirty = true; \
}

DEF_WAVE_SETTER(Amp,    amp,    int)
DEF_WAVE_SETTER(Length, length, int)
DEF_WAVE_SETTER(Speed,  speed,  int)
DEF_WAVE_SETTER(Phase,  phase,  float)

#undef DEF_WAVE_SETTER

void Sprite::initDynAttribs()
{
    p->srcRect = new Rect;
    p->materialRect = new Rect;
    p->color = new Color;
    p->tone = new Tone;
    
    p->updateSrcRectCon();
}

/* Flashable */
void Sprite::update()
{
    guardDisposed();
    
    Flashable::update();
    
    p->wave.phase += p->wave.speed / 180;
    p->wave.dirty = true;
}

/* SceneElement */
/* Built on first use so a driver that rejects it only turns materials off */
static SpriteMaterialShader *materialShader;
static bool materialShaderFailed;

static SpriteMaterialShader *getMaterialShader()
{
    if (materialShader || materialShaderFailed)
        return materialShader;

    try {
        materialShader = new SpriteMaterialShader();
    } catch (const Exception &e) {
        materialShaderFailed = true;
        Debug() << "Sprite material shader unavailable:" << e.msg;
    }

    return materialShader;
}

bool Sprite::materialAvailable()
{
    return getMaterialShader() != 0;
}

/* Copy of what is already drawn under the current render target,
 * read by material mode 2 for refraction */
static TEX::ID materialBackTex;
static Vec2i materialBackSize;

static bool grabMaterialBackdrop(Vec2 &sizeInv)
{
    const IntRect &vp = glState.viewport.get();
    if (vp.w <= 0 || vp.h <= 0)
        return false;

    if (materialBackTex == TEX::ID(0)) {
        materialBackTex = TEX::gen();
        TEX::bind(materialBackTex);
        TEX::setRepeat(false);
        TEX::setSmooth(true);
    }

    TEX::bind(materialBackTex);
    if (materialBackSize.x != vp.w || materialBackSize.y != vp.h) {
        TEX::allocEmpty(vp.w, vp.h);
        materialBackSize = Vec2i(vp.w, vp.h);
    }

    /* a native blit may have left another read framebuffer bound */
    if (gl.BlitFramebuffer) {
        GLint cur = 0;
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) cur);
    }
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vp.x, vp.y, vp.w, vp.h);
    sizeInv = Vec2(1.f / vp.w, 1.f / vp.h);
    return true;
}

void Sprite::draw()
{
    if (!p->isVisible)
        return;
    
    if (emptyFlashFlag)
        return;
    
    ShaderBase *base;
    
    bool renderEffect = p->color->hasEffect() ||
    p->tone->hasEffect()  ||
    flashing              ||
    p->bushDepth != 0     ||
    p->invert             ||
    (p->pattern && !p->pattern->isDisposed());
    
    ARCShader *user = (p->shader && p->shader->ok()) ? p->shader : 0;

    bool useMaterial = (p->materialMode == 3 ||
    (p->materialMode > 0 && !nullOrDisposed(p->materialMap))) && getMaterialShader();
    
    const bool dot = p->dotActive() && p->meshArray && p->dotQuad;
    const float *spriteMat = p->trans.getMatrix();
    float opacity = p->opacity.norm;
    
    Vec2 backInv;
    bool back = useMaterial && p->materialMode == 2 && p->materialParams[15] > 0.f &&
    grabMaterialBackdrop(backInv);
    
    float screenMat[16];
    GLint screenFbo = 0;
    if (dot)
    {
        const IntRect &vp = glState.viewport.get();
        MeshDot::screenMatrix(screenMat, vp.w, vp.h, spriteMat, p->meshSrc.w, p->mirrored);
        screenFbo = p->beginDot();
        spriteMat = p->dotMat;
        opacity = 1.f;
    }

    /* A user shader that cannot combine with the material is left out */
    SpriteMaterialShader *userMaterial = (useMaterial && user) ? user->materialShader() : 0;
    if (useMaterial && !userMaterial)
        user = 0;

    if (useMaterial)
    {
        SpriteMaterialShader &shader = userMaterial ? *userMaterial : *materialShader;
        const float *mp = p->materialParams;

        shader.bind();
        shader.applyViewportProj();
        shader.setSpriteMat(spriteMat);

        shader.setTone(p->tone->norm);
        shader.setOpacity(opacity);
        shader.setBushDepth(p->efBushDepth);
        shader.setBushOpacity(p->bushOpacity.norm);

        if (p->pattern && !p->pattern->isDisposed() && p->patternOpacity > 0) {
            shader.setPattern(p->pattern->getGLTypes().tex, Vec2(p->pattern->width(), p->pattern->height()));
            shader.setPatternBlendType(p->patternBlendType);
            shader.setPatternTile(p->patternTile);
            shader.setPatternZoom(p->patternZoom);
            shader.setPatternOpacity(p->patternOpacity.norm);
            shader.setPatternScroll(p->patternScroll);
            shader.setShouldRenderPattern(true);
        }
        else {
            shader.setShouldRenderPattern(false);
        }

        shader.setInvert(p->invert);

        const Vec4 *blend = (flashing && flashColor.w > p->color->norm.w) ?
        &flashColor : &p->color->norm;
        shader.setColor(*blend);

        const float bw = (float) p->bitmap->width();
        const float bh = (float) p->bitmap->height();
        const TEXFBO &bt = p->bitmap->getGLTypes();
        shader.setSrcTexel(Vec2(1.f / bt.width, 1.f / bt.height));
        shader.setMode(p->materialMode);
        shader.setMirror(p->mirrored);
        shader.setParams(mp);

        if (!nullOrDisposed(p->materialMap)) {
            Bitmap *m = p->materialMap;
            const TEXFBO &mt = m->getGLTypes();
            const Rect &r = *p->materialRect;
            const float mw = (float) m->width();
            const float mh = (float) m->height();
            const float rw = (r.width  > 0) ? (float) r.width  : bw;
            const float rh = (r.height > 0) ? (float) r.height : bh;
            shader.setMap(mt.tex, Vec2(1.f / mt.width, 1.f / mt.height),
                          Vec4(rw / (bw * mw), rh / (bh * mh), r.x / mw, r.y / mh));
        }
        else {
            shader.setMap(bt.tex, Vec2(1.f / bt.width, 1.f / bt.height), Vec4(0, 0, 0, 0));
        }

        bool env = !nullOrDisposed(p->materialEnv);
        shader.setEnv(env ? p->materialEnv->getGLTypes().tex : TEX::ID(0), env);
        bool ramp = !nullOrDisposed(p->materialRamp);
        shader.setRamp(ramp ? p->materialRamp->getGLTypes().tex : TEX::ID(0), ramp);
        shader.setBack(materialBackTex, backInv, back);
        shader.setScreenMat((dot && back) ? screenMat : 0);

        if (user)
            p->applyUser(user, shader);

        base = &shader;
    }
    else if (renderEffect || user)
    {
        SpriteShader &shader = user ? *user->spriteShader() : shState->shaders().sprite;
        
        shader.bind();
        shader.applyViewportProj();
        shader.setSpriteMat(spriteMat);
        
        shader.setTone(p->tone->norm);
        shader.setOpacity(opacity);
        shader.setBushDepth(p->efBushDepth);
        shader.setBushOpacity(p->bushOpacity.norm);
        
        if (p->pattern && p->patternOpacity > 0) {
            shader.setPattern(p->pattern->getGLTypes().tex, Vec2(p->pattern->width(), p->pattern->height()));
            shader.setPatternBlendType(p->patternBlendType);
            shader.setPatternTile(p->patternTile);
            shader.setPatternZoom(p->patternZoom);
            shader.setPatternOpacity(p->patternOpacity.norm);
            shader.setPatternScroll(p->patternScroll);
            shader.setShouldRenderPattern(true);
        }
        else {
            shader.setShouldRenderPattern(false);
        }
        
        shader.setInvert(p->invert);
        
        /* When both flashing and effective color are set,
         * the one with higher alpha will be blended */
        const Vec4 *blend = (flashing && flashColor.w > p->color->norm.w) ?
        &flashColor : &p->color->norm;
        
        shader.setColor(*blend);
        
        if (user)
            p->applyUser(user, shader);
        
        base = &shader;
    }
    else if (!dot && p->opacity != 255)
    {
        AlphaSpriteShader &shader = shState->shaders().alphaSprite;
        shader.bind();
        
        shader.setSpriteMat(spriteMat);
        shader.setAlpha(p->opacity.norm);
        shader.applyViewportProj();
        base = &shader;
    }
    else
    {
        SimpleSpriteShader &shader = shState->shaders().simpleSprite;
        shader.bind();
        
        shader.setSpriteMat(spriteMat);
        shader.applyViewportProj();
        base = &shader;
    }
    
    if (dot)
    {
        p->bitmap->bindTex(*base);
        p->meshArray->draw();
        p->endDot(screenFbo);
        p->drawDot();
        return;
    }
    
    glState.blendMode.pushSet(p->blendType);
    
    p->bitmap->bindTex(*base);
    
    if (p->mesh.deforms())
    {
        if (p->mesh.dirty || !p->meshArray)
            p->updateMesh();
        p->meshArray->draw();
    }
    else if (p->wave.active)
        p->wave.qArray.draw();
    else if (user && p->shaderPad > 0)
        p->drawPadded();
    else
        p->quad.draw();
    
    glState.blendMode.pop();
}

void Sprite::onGeometryChange(const Scene::Geometry &geo)
{
    /* Offset at which the sprite will be drawn
     * relative to screen origin */
    p->trans.setGlobalOffset(geo.offset());
    
    p->sceneRect.setSize(geo.rect.size());
    p->sceneOrig = geo.orig;
}

void Sprite::releaseResources()
{
    unlink();
    
    delete p;
}
