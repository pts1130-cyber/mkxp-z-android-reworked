/*
** particlecore.h
**
** Particle storage, motion and quad building for ParticleLayer.
** No GL or Ruby dependencies so the same code can be tested standalone.
*/

#ifndef PARTICLECORE_H
#define PARTICLECORE_H

#include <math.h>
#include <string.h>
#include <vector>

namespace ParticleCore
{

enum
{
	MaxParticles = 4096,
	StateFields  = 9,   /* x, y, cell, scale, angle, opacity, r, g, b */
	MotionFields = 12,  /* vx, vy, ax, ay, life, fade_in, fade_out, spin, grow, drag, anim_speed, anim_frames */
	VertexFloats = 8,   /* pos.xy, tex.xy, color.rgba (matches struct Vertex) */
	QuadFloats   = VertexFloats * 4
};

struct Particle
{
	float x, y, cell, scale, angle, opacity, r, g, b;
	float vx, vy, ax, ay, life, fadeIn, fadeOut, spin, grow, drag, animSpeed, animFrames;
	float age;
	bool moving;
};

struct BuildParams
{
	float cellW, cellH;
	float originX, originY;
	int cols, cellCount;
	float offsetX, offsetY;
	float alpha;          /* layer opacity, 0..1 */
	bool snap;
	bool cull;
	float cullX0, cullY0, cullX1, cullY1;
};

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline bool finitef(float v)
{
	return v == v && v - v == 0.f;
}

class Store
{
public:
	Store() { parts.reserve(256); }

	int count() const { return (int) parts.size(); }

	void clear() { parts.clear(); }

	/* state: count*stateStride floats (stateStride 1..9),
	 * motion: count*motionStride floats or null (motionStride 1..12).
	 * Returns how many particles were stored. */
	int load(const float *state, int stateStride, const float *motion, int motionStride,
	         int n, bool append)
	{
		if (!append)
			parts.clear();

		if (n < 0)
			n = 0;

		int room = MaxParticles - (int) parts.size();
		if (n > room)
			n = room;

		static const float stateDef[StateFields] = { 0, 0, 0, 1, 0, 1, 1, 1, 1 };
		static const float motionDef[MotionFields] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1 };

		size_t base = parts.size();
		parts.resize(base + n);

		for (int i = 0; i < n; ++i)
		{
			float s[StateFields], m[MotionFields];

			for (int k = 0; k < StateFields; ++k)
				s[k] = (k < stateStride) ? state[(size_t) i * stateStride + k] : stateDef[k];

			for (int k = 0; k < MotionFields; ++k)
				m[k] = (motion && k < motionStride) ? motion[(size_t) i * motionStride + k] : motionDef[k];

			for (int k = 0; k < StateFields; ++k)
				if (!finitef(s[k])) s[k] = stateDef[k];

			for (int k = 0; k < MotionFields; ++k)
				if (!finitef(m[k])) m[k] = motionDef[k];

			Particle &p = parts[base + i];
			p.x = s[0]; p.y = s[1]; p.cell = s[2]; p.scale = s[3]; p.angle = s[4];
			p.opacity = clampf(s[5], 0.f, 1.f);
			p.r = clampf(s[6], 0.f, 2.f); p.g = clampf(s[7], 0.f, 2.f); p.b = clampf(s[8], 0.f, 2.f);

			p.vx = m[0]; p.vy = m[1]; p.ax = m[2]; p.ay = m[3];
			p.life = m[4]; p.fadeIn = m[5]; p.fadeOut = m[6];
			p.spin = m[7]; p.grow = m[8]; p.drag = m[9];
			p.animSpeed = m[10]; p.animFrames = m[11] < 1.f ? 1.f : m[11];
			p.age = 0.f;
			p.moving = motion != 0;
		}

		return n;
	}

	/* Advances moving particles by dt frames and removes expired ones
	 * (draw order of the survivors is kept). Returns the live count. */
	int update(float dt)
	{
		if (!(dt > 0.f) || !finitef(dt))
			return count();

		size_t w = 0;
		const size_t n = parts.size();

		for (size_t i = 0; i < n; ++i)
		{
			Particle p = parts[i];

			if (p.moving)
			{
				p.age += dt;

				if (p.life > 0.f && p.age >= p.life)
					continue;

				float d = (p.drag == 1.f) ? 1.f : (dt == 1.f ? p.drag : powf(p.drag < 0.f ? 0.f : p.drag, dt));
				p.vx = p.vx * d + p.ax * dt;
				p.vy = p.vy * d + p.ay * dt;
				p.x += p.vx * dt;
				p.y += p.vy * dt;
				p.angle += p.spin * dt;
				p.scale += p.grow * dt;

				if (p.angle >= 360.f || p.angle < -360.f)
					p.angle = fmodf(p.angle, 360.f);
			}

			parts[w++] = p;
		}

		parts.resize(w);

		return (int) w;
	}

	float effectiveAlpha(const Particle &p) const
	{
		float a = p.opacity;

		if (!p.moving)
			return a;

		if (p.fadeIn > 0.f && p.age < p.fadeIn)
			a *= p.age / p.fadeIn;

		if (p.fadeOut > 0.f && p.life > 0.f)
		{
			float left = p.life - p.age;
			if (left < p.fadeOut)
				a *= left / p.fadeOut;
		}

		return a;
	}

	int effectiveCell(const Particle &p) const
	{
		int c = (int) floorf(p.cell);

		if (p.moving && p.animFrames > 1.f && p.animSpeed != 0.f)
		{
			int frames = (int) p.animFrames;
			int step = (int) floorf(p.age * p.animSpeed) % frames;
			if (step < 0)
				step += frames;
			c += step;
		}

		return c;
	}

	/* Writes up to count() quads (QuadFloats each) and returns how many. */
	int build(float *out, const BuildParams &bp) const
	{
		if (bp.cellW <= 0.f || bp.cellH <= 0.f || bp.cols <= 0 || bp.cellCount <= 0 || !(bp.alpha > 0.f))
			return 0;

		const float cw = bp.cellW, ch = bp.cellH;
		const float ox = bp.originX, oy = bp.originY;
		const float ext = fabsf(ox) + fabsf(oy) + cw + ch;

		int q = 0;
		const size_t n = parts.size();

		for (size_t i = 0; i < n; ++i)
		{
			const Particle &p = parts[i];

			const float alpha = effectiveAlpha(p) * bp.alpha;
			if (!(alpha > 0.f))
				continue;

			const int ci = effectiveCell(p);
			if (ci < 0 || ci >= bp.cellCount)
				continue;

			if (p.scale == 0.f)
				continue;

			float px = p.x + bp.offsetX;
			float py = p.y + bp.offsetY;

			if (bp.snap)
			{
				px = floorf(px + 0.5f);
				py = floorf(py + 0.5f);
			}

			if (bp.cull)
			{
				const float r = ext * fabsf(p.scale);
				if (px + r < bp.cullX0 || px - r > bp.cullX1 ||
				    py + r < bp.cullY0 || py - r > bp.cullY1)
					continue;
			}

			/* Same arithmetic as Transform::updateMatrix */
			float rot = p.angle;
			if (rot >= 360.f || rot < -360.f)
				rot = fmodf(rot, 360.f);

			float sxc, syc, sxs, sys;
			if (rot == 0.f)
			{
				sxc = syc = p.scale;
				sxs = sys = 0.f;
			}
			else
			{
				const float ang = rot * 3.141592654f / 180.0f;
				const float c = (float) cos(ang);
				const float s = (float) sin(ang);
				sxc = p.scale * c; syc = p.scale * c;
				sxs = p.scale * s; sys = p.scale * s;
			}

			const float tx = -ox * sxc - oy * sys + px;
			const float ty =  ox * sxs - oy * syc + py;

			const float u0 = (float) (ci % bp.cols) * cw;
			const float v0 = (float) (ci / bp.cols) * ch;

			static const float lx[4] = { 0, 1, 1, 0 };
			static const float ly[4] = { 0, 0, 1, 1 };

			float *v = out + (size_t) q * QuadFloats;

			for (int k = 0; k < 4; ++k)
			{
				const float x = lx[k] * cw;
				const float y = ly[k] * ch;
				v[0] = sxc * x + sys * y + tx;
				v[1] = -sxs * x + syc * y + ty;
				v[2] = u0 + x;
				v[3] = v0 + y;
				v[4] = p.r;
				v[5] = p.g;
				v[6] = p.b;
				v[7] = alpha;
				v += VertexFloats;
			}

			++q;
		}

		return q;
	}

	/* StateFields floats per particle, with motion applied (alpha/cell as drawn) */
	void exportState(float *out) const
	{
		for (size_t i = 0; i < parts.size(); ++i)
		{
			const Particle &p = parts[i];
			float *o = out + i * StateFields;
			o[0] = p.x; o[1] = p.y; o[2] = (float) effectiveCell(p); o[3] = p.scale; o[4] = p.angle;
			o[5] = effectiveAlpha(p); o[6] = p.r; o[7] = p.g; o[8] = p.b;
		}
	}

private:
	std::vector<Particle> parts;
};

} // namespace ParticleCore

#endif // PARTICLECORE_H
