/*
** arcshader-binding.cpp
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
#include "binding-types.h"
#include "binding-util.h"
#include "bitmap.h"
#include "graphics.h"
#include "sharedstate.h"

#if RAPI_FULL > 187
DEF_TYPE(ARCShader);
#else
DEF_ALLOCFUNC(ARCShader);
#endif

static ARCShader *arcShaderData(VALUE self)
{
    return getPrivateData<ARCShader>(self);
}

RB_METHOD(arcShaderInitialize) {
    VALUE src, kindObj = Qnil;
    rb_scan_args(argc, argv, "11", &src, &kindObj);

    StringValue(src);
    int kind = NIL_P(kindObj) ? ARCShader::KindSprite : NUM2INT(kindObj);
    if (kind != ARCShader::KindSprite && kind != ARCShader::KindPost)
        rb_raise(rb_eArgError, "ARCShader kind must be ARCShader::SPRITE or ARCShader::POST");

    ARCShader *s = 0;
    GFX_GUARD_EXC(s = new ARCShader(RSTRING_PTR(src), (int) RSTRING_LEN(src), kind);)

    setPrivateData(self, s);
    rb_iv_set(self, "textures", rb_hash_new());

    return self;
}

RB_METHOD(arcShaderOk) {
    RB_UNUSED_PARAM;
    return rb_bool_new(arcShaderData(self)->ok());
}

RB_METHOD(arcShaderLog) {
    RB_UNUSED_PARAM;
    const std::string &log = arcShaderData(self)->log();
    return rb_utf8_str_new(log.c_str(), (long) log.size());
}

RB_METHOD(arcShaderKind) {
    RB_UNUSED_PARAM;
    return INT2FIX(arcShaderData(self)->kind());
}

static VALUE arcShaderSetN(int argc, VALUE *argv, VALUE self, int comps) {
    rb_check_argc(argc, comps + 1);

    ARCShader *s = arcShaderData(self);
    const char *name = StringValueCStr(argv[0]);

    float v[4];
    for (int i = 0; i < comps; ++i)
        v[i] = (float) NUM2DBL(argv[i + 1]);

    GFX_GUARD_EXC(s->setFloats(name, v, comps, 1);)

    return self;
}

RB_METHOD(arcShaderSetFloat) { return arcShaderSetN(argc, argv, self, 1); }
RB_METHOD(arcShaderSetVec2)  { return arcShaderSetN(argc, argv, self, 2); }
RB_METHOD(arcShaderSetVec3)  { return arcShaderSetN(argc, argv, self, 3); }
RB_METHOD(arcShaderSetVec4)  { return arcShaderSetN(argc, argv, self, 4); }

RB_METHOD(arcShaderSetFloatArray) {
    rb_check_argc(argc, 2);

    ARCShader *s = arcShaderData(self);
    const char *name = StringValueCStr(argv[0]);
    VALUE ary = rb_Array(argv[1]);

    long n = RARRAY_LEN(ary);
    if (n > ARCShader::MaxArray)
        rb_raise(rb_eArgError, "ARCShader#set_float_array takes at most %d values", (int) ARCShader::MaxArray);

    float v[ARCShader::MaxArray] = { 0 };
    for (long i = 0; i < n; ++i)
        v[i] = (float) NUM2DBL(rb_ary_entry(ary, i));

    GFX_GUARD_EXC(s->setFloats(name, v, 1, n > 0 ? (int) n : 1);)

    return self;
}

RB_METHOD(arcShaderSetTexture) {
    VALUE nameObj, bmpObj, unitObj = Qnil;
    rb_scan_args(argc, argv, "21", &nameObj, &bmpObj, &unitObj);

    ARCShader *s = arcShaderData(self);
    const char *name = StringValueCStr(nameObj);

    int unit = NIL_P(unitObj) ? ARCShader::FirstUnit : NUM2INT(unitObj);
    if (unit < ARCShader::FirstUnit || unit > ARCShader::LastUnit)
        rb_raise(rb_eArgError, "ARCShader texture unit must be %d..%d",
                 (int) ARCShader::FirstUnit, (int) ARCShader::LastUnit);

    Bitmap *bmp = 0;
    if (!NIL_P(bmpObj))
        bmp = getPrivateDataCheck<Bitmap>(bmpObj, BitmapType);

    GFX_GUARD_EXC(s->setTexture(name, bmp, unit);)

    /* keep the bitmap alive while it is bound */
    VALUE textures = rb_iv_get(self, "textures");
    if (NIL_P(textures)) {
        textures = rb_hash_new();
        rb_iv_set(self, "textures", textures);
    }
    if (NIL_P(bmpObj))
        rb_hash_delete(textures, nameObj);
    else
        rb_hash_aset(textures, rb_str_new_frozen(nameObj), bmpObj);

    return self;
}

RB_METHOD(arcShaderGetScale) {
    RB_UNUSED_PARAM;
    return rb_float_new(arcShaderData(self)->getScale());
}

RB_METHOD(arcShaderSetScale) {
    rb_check_argc(argc, 1);
    ARCShader *s = arcShaderData(self);
    float v = (float) NUM2DBL(argv[0]);
    GFX_GUARD_EXC(s->setScale(v);)
    return argv[0];
}

RB_METHOD(arcShaderAvailable) {
    RB_UNUSED_PARAM;

    bool value = false;
    GFX_GUARD_EXC(value = ARCShader::available();)

    return rb_bool_new(value);
}

void arcShaderBindingInit() {
    VALUE klass = rb_define_class("ARCShader", rb_cObject);
#if RAPI_FULL > 187
    rb_define_alloc_func(klass, classAllocate<&ARCShaderType>);
#else
    rb_define_alloc_func(klass, ARCShaderAllocate);
#endif

    rb_define_const(klass, "SPRITE", INT2FIX(ARCShader::KindSprite));
    rb_define_const(klass, "POST", INT2FIX(ARCShader::KindPost));
    rb_define_const(klass, "MAX_PASSES", INT2FIX(ARCShader::MaxPasses));

    _rb_define_method(klass, "initialize", arcShaderInitialize);
    _rb_define_method(klass, "ok?", arcShaderOk);
    _rb_define_method(klass, "log", arcShaderLog);
    _rb_define_method(klass, "kind", arcShaderKind);
    _rb_define_method(klass, "set_float", arcShaderSetFloat);
    _rb_define_method(klass, "set_vec2", arcShaderSetVec2);
    _rb_define_method(klass, "set_vec3", arcShaderSetVec3);
    _rb_define_method(klass, "set_vec4", arcShaderSetVec4);
    _rb_define_method(klass, "set_float_array", arcShaderSetFloatArray);
    _rb_define_method(klass, "set_texture", arcShaderSetTexture);
    _rb_define_method(klass, "scale", arcShaderGetScale);
    _rb_define_method(klass, "scale=", arcShaderSetScale);
    rb_define_singleton_method(klass, "available?", RUBY_METHOD_FUNC(arcShaderAvailable), -1);
}
