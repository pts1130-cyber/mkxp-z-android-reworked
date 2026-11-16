/*
 ** sprite-binding.cpp
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

#include "binding-types.h"
#include "binding-util.h"
#include "disposable-binding.h"
#include "flashable-binding.h"
#include "sceneelement-binding.h"
#include "sharedstate.h"
#include "sprite.h"
#include "arcshader.h"
#include "viewportelement-binding.h"

#include <string.h>
#include <vector>

#if RAPI_FULL > 187
DEF_TYPE(Sprite);
#else
DEF_ALLOCFUNC(Sprite);
#endif

RB_METHOD(spriteInitialize) {
    GFX_LOCK;
    Sprite *s = viewportElementInitialize<Sprite>(argc, argv, self);
    
    setPrivateData(self, s);
    
    /* Wrap property objects */
    s->initDynAttribs();
    
    wrapProperty(self, &s->getSrcRect(), "src_rect", RectType);
    wrapProperty(self, &s->getColor(), "color", ColorType);
    wrapProperty(self, &s->getTone(), "tone", ToneType);
    wrapProperty(self, &s->getMaterialRect(), "material_rect", RectType);
    
    GFX_UNLOCK;
    return self;
}

DEF_GFX_PROP_OBJ_REF(Sprite, Bitmap, Bitmap, "bitmap")
DEF_GFX_PROP_OBJ_REF(Sprite, Bitmap, Pattern, "pattern")
DEF_GFX_PROP_OBJ_REF(Sprite, ARCShader, Shader, "shader")
DEF_GFX_PROP_OBJ_REF(Sprite, Bitmap, MaterialMap, "material_map")
DEF_GFX_PROP_OBJ_REF(Sprite, Bitmap, MaterialEnv, "material_env")
DEF_GFX_PROP_OBJ_REF(Sprite, Bitmap, MaterialRamp, "material_ramp")
DEF_GFX_PROP_OBJ_VAL(Sprite, Rect, MaterialRect, "material_rect")
DEF_GFX_PROP_OBJ_VAL(Sprite, Rect, SrcRect, "src_rect")
DEF_GFX_PROP_OBJ_VAL(Sprite, Color, Color, "color")
DEF_GFX_PROP_OBJ_VAL(Sprite, Tone, Tone, "tone")

DEF_GFX_PROP_I(Sprite, X)
DEF_GFX_PROP_I(Sprite, Y)
DEF_GFX_PROP_I(Sprite, OX)
DEF_GFX_PROP_I(Sprite, OY)
DEF_GFX_PROP_I(Sprite, BushDepth)
DEF_GFX_PROP_I(Sprite, BushOpacity)
DEF_GFX_PROP_I(Sprite, Opacity)
DEF_GFX_PROP_I(Sprite, BlendType)
DEF_GFX_PROP_I(Sprite, PatternBlendType)
DEF_GFX_PROP_I(Sprite, PatternOpacity)
DEF_GFX_PROP_I(Sprite, PatternScrollX)
DEF_GFX_PROP_I(Sprite, PatternScrollY)
DEF_GFX_PROP_I(Sprite, WaveAmp)
DEF_GFX_PROP_I(Sprite, WaveLength)
DEF_GFX_PROP_I(Sprite, WaveSpeed)
DEF_GFX_PROP_I(Sprite, MaterialMode)
DEF_GFX_PROP_I(Sprite, ShaderPad)

DEF_GFX_PROP_F(Sprite, ZoomX)
DEF_GFX_PROP_F(Sprite, ZoomY)
DEF_GFX_PROP_F(Sprite, Angle)
DEF_GFX_PROP_F(Sprite, WavePhase)
DEF_GFX_PROP_F(Sprite, PatternZoomX)
DEF_GFX_PROP_F(Sprite, PatternZoomY)

DEF_GFX_PROP_B(Sprite, Mirror)
DEF_GFX_PROP_B(Sprite, PatternTile)
DEF_GFX_PROP_B(Sprite, Invert)

RB_METHOD(spriteMaterialAvailable) {
    RB_UNUSED_PARAM;
    
    bool value = false;
    GFX_GUARD_EXC(value = Sprite::materialAvailable();)
    
    return rb_bool_new(value);
}

RB_METHOD(spriteGetMaterialParams) {
    RB_UNUSED_PARAM;
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    VALUE ary = rb_ary_new2(Sprite::MaterialParamCount);
    GUARD_EXC(
        for (int i = 0; i < Sprite::MaterialParamCount; ++i)
            rb_ary_push(ary, rb_float_new(s->getMaterialParam(i)));
    )
    
    return ary;
}

RB_METHOD(spriteSetMaterialParams) {
    rb_check_argc(argc, 1);
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    VALUE ary = rb_Array(argv[0]);
    long n = RARRAY_LEN(ary);
    if (n > Sprite::MaterialParamCount)
        n = Sprite::MaterialParamCount;
    
    float values[Sprite::MaterialParamCount];
    for (long i = 0; i < n; ++i)
        values[i] = (float) NUM2DBL(rb_ary_entry(ary, i));
    
    GFX_GUARD_EXC(s->setMaterialParams(values, (int) n);)
    
    return argv[0];
}

static std::vector<float> meshScratch;

/* Array of numbers or a String packed with 'f*'; false for nil */
static bool meshReadFloats(VALUE v)
{
    static const long cap = 2L * (Sprite::MeshMaxCells + 1) * (Sprite::MeshMaxCells + 1);
    
    meshScratch.clear();
    if (NIL_P(v))
        return false;
    
    if (RB_TYPE_P(v, T_STRING))
    {
        long n = RSTRING_LEN(v) / (long) sizeof(float);
        if (n > cap)
            n = cap;
        meshScratch.resize(n);
        if (n > 0)
            memcpy(&meshScratch[0], RSTRING_PTR(v), n * sizeof(float));
        return true;
    }
    
    VALUE ary = rb_Array(v);
    long n = RARRAY_LEN(ary);
    if (n > cap)
        n = cap;
    meshScratch.resize(n);
    for (long i = 0; i < n; ++i)
        meshScratch[i] = (float) NUM2DBL(rb_ary_entry(ary, i));
    return true;
}

static VALUE meshOffsetArray(Sprite *s, bool uv)
{
    int n = 0;
    GUARD_EXC(n = s->getMeshVertexCount() * 2;)
    
    VALUE ary = rb_ary_new2(n);
    for (int i = 0; i < n; ++i)
        rb_ary_push(ary, rb_float_new(s->getMeshOffset(i, uv)));
    
    return ary;
}

RB_METHOD(spriteMeshGrid) {
    Sprite *s = getPrivateData<Sprite>(self);
    
    if (argc == 2)
    {
        int cols = NUM2INT(argv[0]);
        int rows = NUM2INT(argv[1]);
        GFX_GUARD_EXC(s->setMeshGrid(cols, rows);)
        return Qnil;
    }
    
    if (argc != 0)
        rb_raise(rb_eArgError, "wrong number of arguments (%d for 0 or 2)", argc);
    
    int cols = 0, rows = 0;
    GUARD_EXC(cols = s->getMeshCols(); rows = s->getMeshRows();)
    
    return rb_ary_new3(2, INT2FIX(cols), INT2FIX(rows));
}

RB_METHOD(spriteGetMeshOffsets) {
    RB_UNUSED_PARAM;
    
    return meshOffsetArray(getPrivateData<Sprite>(self), false);
}

RB_METHOD(spriteSetMeshOffsets) {
    rb_check_argc(argc, 1);
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    bool given = meshReadFloats(argv[0]);
    int n = (int) meshScratch.size();
    const float *v = (given && n > 0) ? &meshScratch[0] : 0;
    GFX_GUARD_EXC(s->setMeshOffsets(v, n);)
    
    return argv[0];
}

RB_METHOD(spriteGetMeshUVOffsets) {
    RB_UNUSED_PARAM;
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    bool has = false;
    GUARD_EXC(has = s->hasMeshUVOffsets();)
    
    return has ? meshOffsetArray(s, true) : Qnil;
}

RB_METHOD(spriteSetMeshUVOffsets) {
    rb_check_argc(argc, 1);
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    static const float zero = 0.f;
    bool given = meshReadFloats(argv[0]);
    int n = (int) meshScratch.size();
    const float *v = given ? (n > 0 ? &meshScratch[0] : &zero) : 0;
    GFX_GUARD_EXC(s->setMeshUVOffsets(v, n);)
    
    return argv[0];
}

RB_METHOD(spriteGetMeshDot) {
    RB_UNUSED_PARAM;
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    int value = 0;
    GUARD_EXC(value = s->getMeshDot();)
    
    return INT2FIX(value);
}

RB_METHOD(spriteSetMeshDot) {
    rb_check_argc(argc, 1);
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    int value = NIL_P(argv[0]) ? 0 : NUM2INT(argv[0]);
    GFX_GUARD_EXC(s->setMeshDot(value);)
    
    return argv[0];
}

RB_METHOD(spriteWidth) {
    RB_UNUSED_PARAM;
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    int value = 0;
    GUARD_EXC(value = s->getWidth();)
    
    return rb_fix_new(value);
}

RB_METHOD(spriteHeight) {
    RB_UNUSED_PARAM;
    
    Sprite *s = getPrivateData<Sprite>(self);
    
    int value = 0;
    GUARD_EXC(value = s->getHeight();)
    
    return rb_fix_new(value);
}

void spriteBindingInit() {
    VALUE klass = rb_define_class("Sprite", rb_cObject);
#if RAPI_FULL > 187
    rb_define_alloc_func(klass, classAllocate<&SpriteType>);
#else
    rb_define_alloc_func(klass, SpriteAllocate);
#endif
    
    disposableBindingInit<Sprite>(klass);
    flashableBindingInit<Sprite>(klass);
    viewportElementBindingInit<Sprite>(klass);
    
    _rb_define_method(klass, "initialize", spriteInitialize);
    
    INIT_PROP_BIND(Sprite, Bitmap, "bitmap");
    INIT_PROP_BIND(Sprite, SrcRect, "src_rect");
    INIT_PROP_BIND(Sprite, X, "x");
    INIT_PROP_BIND(Sprite, Y, "y");
    INIT_PROP_BIND(Sprite, OX, "ox");
    INIT_PROP_BIND(Sprite, OY, "oy");
    INIT_PROP_BIND(Sprite, ZoomX, "zoom_x");
    INIT_PROP_BIND(Sprite, ZoomY, "zoom_y");
    INIT_PROP_BIND(Sprite, Angle, "angle");
    INIT_PROP_BIND(Sprite, Mirror, "mirror");
    INIT_PROP_BIND(Sprite, BushDepth, "bush_depth");
    INIT_PROP_BIND(Sprite, Opacity, "opacity");
    INIT_PROP_BIND(Sprite, BlendType, "blend_type");
    INIT_PROP_BIND(Sprite, Color, "color");
    INIT_PROP_BIND(Sprite, Tone, "tone");
    
    _rb_define_method(klass, "width", spriteWidth);
    _rb_define_method(klass, "height", spriteHeight);
    
    INIT_PROP_BIND(Sprite, BushOpacity, "bush_opacity");
    
    INIT_PROP_BIND(Sprite, Pattern, "pattern");
    INIT_PROP_BIND(Sprite, PatternBlendType, "pattern_blend_type");
    INIT_PROP_BIND(Sprite, PatternTile, "pattern_tile");
    INIT_PROP_BIND(Sprite, PatternOpacity, "pattern_opacity");
    INIT_PROP_BIND(Sprite, PatternScrollX, "pattern_scroll_x");
    INIT_PROP_BIND(Sprite, PatternScrollY, "pattern_scroll_y");
    INIT_PROP_BIND(Sprite, PatternZoomX, "pattern_zoom_x");
    INIT_PROP_BIND(Sprite, PatternZoomY, "pattern_zoom_y");
    INIT_PROP_BIND(Sprite, Invert, "invert");
    INIT_PROP_BIND(Sprite, Shader, "shader");
    INIT_PROP_BIND(Sprite, ShaderPad, "shader_pad");
    
    INIT_PROP_BIND(Sprite, MaterialMap, "material_map");
    INIT_PROP_BIND(Sprite, MaterialEnv, "material_env");
    INIT_PROP_BIND(Sprite, MaterialRamp, "material_ramp");
    INIT_PROP_BIND(Sprite, MaterialMode, "material_mode");
    INIT_PROP_BIND(Sprite, MaterialRect, "material_rect");
    _rb_define_method(klass, "material_params", spriteGetMaterialParams);
    _rb_define_method(klass, "material_params=", spriteSetMaterialParams);
    rb_define_singleton_method(klass, "material_available?", RUBY_METHOD_FUNC(spriteMaterialAvailable), -1);
    
    _rb_define_method(klass, "mesh_grid", spriteMeshGrid);
    _rb_define_method(klass, "mesh_offsets", spriteGetMeshOffsets);
    _rb_define_method(klass, "mesh_offsets=", spriteSetMeshOffsets);
    _rb_define_method(klass, "mesh_uv_offsets", spriteGetMeshUVOffsets);
    _rb_define_method(klass, "mesh_uv_offsets=", spriteSetMeshUVOffsets);
    _rb_define_method(klass, "mesh_dot", spriteGetMeshDot);
    _rb_define_method(klass, "mesh_dot=", spriteSetMeshDot);
    rb_define_const(klass, "MESH_MAX_CELLS", INT2FIX(Sprite::MeshMaxCells));
    rb_define_const(klass, "MESH_MAX_DOT", INT2FIX(Sprite::MeshMaxDot));
    
    INIT_PROP_BIND(Sprite, WaveAmp, "wave_amp");
    INIT_PROP_BIND(Sprite, WaveLength, "wave_length");
    INIT_PROP_BIND(Sprite, WaveSpeed, "wave_speed");
    INIT_PROP_BIND(Sprite, WavePhase, "wave_phase");
}
