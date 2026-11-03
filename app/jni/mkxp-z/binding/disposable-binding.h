/*
** disposable-binding.h
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

#ifndef DISPOSABLEBINDING_H
#define DISPOSABLEBINDING_H

#include "disposable.h"
#include "binding-util.h"
#include "graphics.h"

/* 'Children' are disposables that are disposed together
 * with their parent. Currently this is only used by Viewport
 * in RGSS1. A child leaves the list when it is disposed. */
inline void
disposableAddChild(VALUE disp, VALUE child)
{
	if (NIL_P(disp) || NIL_P(child))
		return;

	GFX_LOCK;

	VALUE children = rb_iv_get(disp, "children");

	if (NIL_P(children))
	{
		children = rb_ary_new();
		rb_iv_set(disp, "children", children);
	}
	else
	{
		for (long i = RARRAY_LEN(children) - 1; i >= 0; --i)
		{
			if (RARRAY_AREF(children, i) == child)
			{
				GFX_UNLOCK;
				return;
			}
		}
	}

	rb_ary_push(children, child);
	GFX_UNLOCK;
}

inline void
disposableRemoveChild(VALUE disp, VALUE child)
{
	if (NIL_P(disp) || NIL_P(child))
		return;

	GFX_LOCK;

	VALUE children = rb_iv_get(disp, "children");

	if (!NIL_P(children))
	{
		for (long i = RARRAY_LEN(children) - 1; i >= 0; --i)
		{
			if (RARRAY_AREF(children, i) == child)
			{
				rb_ary_delete_at(children, i);
				break;
			}
		}
	}

	GFX_UNLOCK;
}

inline void
disposableDisposeChildren(VALUE disp)
{
	VALUE children = rb_iv_get(disp, "children");

	if (NIL_P(children))
		return;

	ID dispFun = rb_intern("_mkxp_dispose_alias");

	/* A child removes itself from the parent's list when it is disposed,
	 * so walk a copy */
	VALUE copy = rb_ary_dup(children);

	for (long i = 0; i < RARRAY_LEN(copy); ++i)
		rb_funcall2(rb_ary_entry(copy, i), dispFun, 0, 0);
}

template<class C>
RB_METHOD(disposableDispose)
{
	RB_UNUSED_PARAM;

	C *d = getPrivateData<C>(self);

	if (!d)
		return Qnil;

	/* Nothing to do if already disposed */
	if (d->isDisposed())
		return Qnil;

	if (rgssVer == 1)
	{
		disposableDisposeChildren(self);

		/* Leave the viewport's child list, otherwise every disposed sprite
		 * (and the bitmap it still points at) stays alive as long as the
		 * viewport does */
		VALUE parent = rb_iv_get(self, "viewport");

		if (!NIL_P(parent))
			disposableRemoveChild(parent, self);
	}

    GFX_LOCK;
	d->dispose();
    GFX_UNLOCK;

	return Qnil;
}

template<class C>
RB_METHOD(disposableIsDisposed)
{
	RB_UNUSED_PARAM;

	C *d = getPrivateData<C>(self);

	if (!d)
		return Qtrue;

	return rb_bool_new(d->isDisposed());
}

template<class C>
static void disposableBindingInit(VALUE klass)
{
	_rb_define_method(klass, "dispose", disposableDispose<C>);
	_rb_define_method(klass, "disposed?", disposableIsDisposed<C>);

	/* Make sure we always have access to the original method, even
	 * if it is overridden by user scripts */
	if (rgssVer == 1)
		rb_define_alias(klass, "_mkxp_dispose_alias", "dispose");
}

template<class C>
inline void
checkDisposed(VALUE self)
{
	if (disposableIsDisposed<C>(0, 0, self) == Qtrue)
		raiseDisposedAccess(self);
}

#endif // DISPOSABLEBINDING_H
