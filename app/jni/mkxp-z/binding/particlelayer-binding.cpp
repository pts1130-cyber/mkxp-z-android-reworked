/*
** particlelayer-binding.cpp
**
** Ruby class ParticleLayer.
*/

#include "binding-types.h"
#include "binding-util.h"
#include "disposable-binding.h"
#include "particlelayer.h"
#include "viewportelement-binding.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef RB_METHOD_GUARD
#define PL_METHOD(name) RB_METHOD_GUARD(name)
#define PL_METHOD_END RB_METHOD_GUARD_END
#define PL_GUARD(exp) { exp }
#else
#define PL_METHOD(name) RB_METHOD(name)
#define PL_METHOD_END
#define PL_GUARD(exp) GUARD_EXC(exp)
#endif

#if RAPI_FULL > 187
DEF_TYPE(ParticleLayer);
#else
DEF_ALLOCFUNC(ParticleLayer);
#endif

PL_METHOD(particleLayerInitialize) {
  ParticleLayer *l = viewportElementInitialize<ParticleLayer>(argc, argv, self);

  setPrivateData(self, l);

  GFX_LOCK;
  l->initDynAttribs();

  wrapProperty(self, &l->getColor(), "color", ColorType);
  wrapProperty(self, &l->getTone(), "tone", ToneType);
  GFX_UNLOCK;

  return self;
}
PL_METHOD_END

DEF_GFX_PROP_OBJ_REF(ParticleLayer, Bitmap, Bitmap, "bitmap")
DEF_GFX_PROP_OBJ_VAL(ParticleLayer, Color, Color, "color")
DEF_GFX_PROP_OBJ_VAL(ParticleLayer, Tone, Tone, "tone")

DEF_GFX_PROP_I(ParticleLayer, Opacity)
DEF_GFX_PROP_I(ParticleLayer, BlendType)

DEF_GFX_PROP_F(ParticleLayer, X)
DEF_GFX_PROP_F(ParticleLayer, Y)

DEF_GFX_PROP_B(ParticleLayer, PixelSnap)
DEF_GFX_PROP_B(ParticleLayer, Smooth)

/* Float data given as a packed String ("e*"/"f*") or an Array of numbers
 * (flat, or one Array per particle). The result is a Ruby String so a
 * raise while converting leaks nothing. */
static VALUE particleFloats(VALUE data, int stride, int maxCount, const float **out, int *count)
{
  *out = 0;
  *count = 0;

  if (NIL_P(data))
    return Qnil;

  if (RB_TYPE_P(data, RUBY_T_STRING)) {
    long n = RSTRING_LEN(data) / (long) (sizeof(float) * stride);
    if (n > maxCount)
      n = maxCount;

    const char *src = RSTRING_PTR(data);
    if (((uintptr_t) src % sizeof(float)) == 0) {
      *out = (const float *) src;
      *count = (int) n;
      return data;
    }

    VALUE buf = rb_str_new(src, n * sizeof(float) * stride);
    *out = (const float *) RSTRING_PTR(buf);
    *count = (int) n;
    return buf;
  }

  if (!RB_TYPE_P(data, RUBY_T_ARRAY))
    rb_raise(rb_eTypeError, "particle data must be a packed String or an Array");

  long len = RARRAY_LEN(data);
  bool nested = len > 0 && RB_TYPE_P(rb_ary_entry(data, 0), RUBY_T_ARRAY);
  long n = nested ? len : len / stride;
  if (n > maxCount)
    n = maxCount;

  VALUE buf = rb_str_new(0, n * sizeof(float) * stride);
  float *dst = (float *) RSTRING_PTR(buf);

  for (long i = 0; i < n; ++i) {
    if (nested) {
      VALUE row = rb_Array(rb_ary_entry(data, i));
      long rl = RARRAY_LEN(row);
      for (int k = 0; k < stride; ++k)
        dst[i * stride + k] = (k < rl) ? (float) NUM2DBL(rb_ary_entry(row, k)) : NAN;
    } else {
      for (int k = 0; k < stride; ++k)
        dst[i * stride + k] = (float) NUM2DBL(rb_ary_entry(data, i * stride + k));
    }
  }

  *out = dst;
  *count = (int) n;
  return buf;
}

static int particleStride(VALUE v, int def, int max) {
  if (NIL_P(v))
    return def;

  int s = NUM2INT(v);
  if (s < 1 || s > max)
    rb_raise(rb_eArgError, "stride must be 1..%d", max);

  return s;
}

/* (data, stride = 6, motion = nil, motion_stride = 12) */
static VALUE particleLoad(int argc, VALUE *argv, VALUE self, bool append) {
  VALUE data = Qnil, strideV = Qnil, motion = Qnil, motionStrideV = Qnil;
  rb_scan_args(argc, argv, "13", &data, &strideV, &motion, &motionStrideV);

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);

  int stride = particleStride(strideV, 6, ParticleLayer::StateFields);
  int mstride = particleStride(motionStrideV, ParticleLayer::MotionFields, ParticleLayer::MotionFields);

  const float *state = 0, *mot = 0;
  int n = 0, mn = 0;
  VALUE keepS = particleFloats(data, stride, ParticleLayer::MaxParticles, &state, &n);
  VALUE keepM = particleFloats(motion, mstride, ParticleLayer::MaxParticles, &mot, &mn);

  if (mot && mn < n)
    n = mn;

  int stored = 0;
  GFX_GUARD_EXC(
    stored = append ? l->addParticles(state, stride, mot, mstride, n)
                    : l->setParticles(state, stride, mot, mstride, n);
  )

  RB_GC_GUARD(keepS);
  RB_GC_GUARD(keepM);

  return rb_fix_new(stored);
}

PL_METHOD(particleLayerSetParticles) {
  return particleLoad(argc, argv, self, false);
}
PL_METHOD_END

PL_METHOD(particleLayerAddParticles) {
  return particleLoad(argc, argv, self, true);
}
PL_METHOD_END

PL_METHOD(particleLayerClear) {
  RB_UNUSED_PARAM;

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);
  GFX_GUARD_EXC(l->clearParticles();)

  return self;
}
PL_METHOD_END

PL_METHOD(particleLayerUpdate) {
  VALUE framesV = Qnil;
  rb_scan_args(argc, argv, "01", &framesV);

  double frames = NIL_P(framesV) ? 1.0 : NUM2DBL(framesV);

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);

  int live = 0;
  GFX_GUARD_EXC(live = l->update((float) frames);)

  return rb_fix_new(live);
}
PL_METHOD_END

PL_METHOD(particleLayerCount) {
  RB_UNUSED_PARAM;

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);

  int v = 0;
  PL_GUARD(v = l->getCount();)

  return rb_fix_new(v);
}
PL_METHOD_END

PL_METHOD(particleLayerDrawnCount) {
  RB_UNUSED_PARAM;

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);

  int v = 0;
  PL_GUARD(v = l->getDrawnCount();)

  return rb_fix_new(v);
}
PL_METHOD_END

/* (w = nil, h = nil, ox = nil, oy = nil): with arguments sets the cell
 * size and its origin (default: centre), returns [w, h, ox, oy] */
PL_METHOD(particleLayerCell) {
  VALUE wV = Qnil, hV = Qnil, oxV = Qnil, oyV = Qnil;
  rb_scan_args(argc, argv, "04", &wV, &hV, &oxV, &oyV);

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);

  if (!NIL_P(wV)) {
    int w = NUM2INT(wV);
    int h = NIL_P(hV) ? w : NUM2INT(hV);
    int ox = NIL_P(oxV) ? -1 : NUM2INT(oxV);
    int oy = NIL_P(oyV) ? -1 : NUM2INT(oyV);
    GFX_GUARD_EXC(l->setCell(w, h, ox, oy);)
  }

  int w = 0, h = 0, ox = 0, oy = 0;
  PL_GUARD(
    w = l->getCellWidth(); h = l->getCellHeight();
    ox = l->getCellOX(); oy = l->getCellOY();
  )

  VALUE ary = rb_ary_new2(4);
  rb_ary_push(ary, rb_fix_new(w));
  rb_ary_push(ary, rb_fix_new(h));
  rb_ary_push(ary, rb_fix_new(ox));
  rb_ary_push(ary, rb_fix_new(oy));
  return ary;
}
PL_METHOD_END

/* Packed String of STATE_FIELDS floats per particle, as drawn now */
PL_METHOD(particleLayerExport) {
  RB_UNUSED_PARAM;

  ParticleLayer *l = getPrivateData<ParticleLayer>(self);

  int n = 0;
  PL_GUARD(n = l->getCount();)

  VALUE buf = rb_str_new(0, (long) n * ParticleLayer::StateFields * sizeof(float));
  float *dst = (float *) RSTRING_PTR(buf);
  GFX_GUARD_EXC(l->exportParticles(dst);)

  return buf;
}
PL_METHOD_END

PL_METHOD(particleLayerAvailable) {
  RB_UNUSED_PARAM;

  bool v = false;
  GFX_GUARD_EXC(v = ParticleLayer::available();)

  return rb_bool_new(v);
}
PL_METHOD_END

void particleLayerBindingInit() {
  VALUE klass = rb_define_class("ParticleLayer", rb_cObject);
#if RAPI_FULL > 187
  rb_define_alloc_func(klass, classAllocate<&ParticleLayerType>);
#else
  rb_define_alloc_func(klass, ParticleLayerAllocate);
#endif

  disposableBindingInit<ParticleLayer>(klass);
  viewportElementBindingInit<ParticleLayer>(klass);

  _rb_define_method(klass, "initialize", particleLayerInitialize);

  INIT_PROP_BIND(ParticleLayer, Bitmap, "bitmap");
  INIT_PROP_BIND(ParticleLayer, Opacity, "opacity");
  INIT_PROP_BIND(ParticleLayer, BlendType, "blend_type");
  INIT_PROP_BIND(ParticleLayer, X, "x");
  INIT_PROP_BIND(ParticleLayer, Y, "y");
  INIT_PROP_BIND(ParticleLayer, PixelSnap, "pixel_snap");
  INIT_PROP_BIND(ParticleLayer, Smooth, "smooth");
  INIT_PROP_BIND(ParticleLayer, Color, "color");
  INIT_PROP_BIND(ParticleLayer, Tone, "tone");

  _rb_define_method(klass, "cell", particleLayerCell);
  _rb_define_method(klass, "set_particles", particleLayerSetParticles);
  _rb_define_method(klass, "add_particles", particleLayerAddParticles);
  _rb_define_method(klass, "clear", particleLayerClear);
  _rb_define_method(klass, "update", particleLayerUpdate);
  _rb_define_method(klass, "count", particleLayerCount);
  _rb_define_method(klass, "drawn_count", particleLayerDrawnCount);
  _rb_define_method(klass, "export_particles", particleLayerExport);

  rb_define_class_method(klass, "available?", particleLayerAvailable);

  rb_define_const(klass, "MAX", INT2FIX(ParticleLayer::MaxParticles));
  rb_define_const(klass, "STATE_FIELDS", INT2FIX(ParticleLayer::StateFields));
  rb_define_const(klass, "MOTION_FIELDS", INT2FIX(ParticleLayer::MotionFields));
}
