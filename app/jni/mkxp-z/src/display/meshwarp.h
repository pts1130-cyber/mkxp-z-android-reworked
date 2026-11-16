/*
 ** meshwarp.h
 **
 ** This file is part of mkxp.
 **
 ** mkxp is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 2 of the License, or
 ** (at your option) any later version.
 */

#ifndef MESHWARP_H
#define MESHWARP_H

#include <cstddef>
#include <vector>

/* Sprite grid deformation: (cols+1)*(rows+1) vertices, each displaced in
 * source pixel units, drawn as cols*rows quads. Every grid vertex is
 * computed once and copied into the quads that touch it, so neighbouring
 * cells meet on bit-identical edges. */
struct MeshWarp
{
    enum { MaxCells = 64 };

    int cols;
    int rows;
    std::vector<float> offsets;
    std::vector<float> uvOffsets;
    bool dirty;
    bool warped;

    MeshWarp()
    : cols(0), rows(0), dirty(false), warped(false)
    {}

    bool active() const { return cols > 0; }
    /* All-zero offsets draw through the plain quad, so they match it exactly */
    bool deforms() const { return cols > 0 && warped; }
    int vertexCount() const { return active() ? (cols + 1) * (rows + 1) : 0; }
    int quadCount() const { return cols * rows; }

    void setGrid(int c, int r)
    {
        c = clampi(c, 0, MaxCells);
        r = clampi(r, 0, MaxCells);
        if (c == 0 || r == 0)
            c = r = 0;
        if (c == cols && r == rows)
            return;

        cols = c;
        rows = r;
        offsets.assign(vertexCount() * 2, 0.f);
        uvOffsets.clear();
        warped = false;
        dirty = true;
    }

    void setOffsets(const float *values, int count)
    {
        copyIn(offsets, values, count);
        updateWarped();
        dirty = true;
    }

    /* values == 0 turns texture displacement off */
    void setUVOffsets(const float *values, int count)
    {
        if (!values || !active())
            uvOffsets.clear();
        else
            copyIn(uvOffsets, values, count);
        updateWarped();
        dirty = true;
    }

    /* (sx, sy, sw, sh): clamped src_rect in bitmap pixels. Offsets follow
     * the picture, so a mirrored sprite moves mirrored. */
    template<class V>
    void build(V *vert, float sx, float sy, float sw, float sh,
               bool mirrored, float texScaleX, float texScaleY) const
    {
        const int vc = cols + 1;
        const int n = vertexCount();
        const bool uv = !uvOffsets.empty();

        grid.resize(n * 4);
        for (int j = 0; j <= rows; ++j)
        {
            const float v = sh * (float) j / (float) rows;
            for (int i = 0; i <= cols; ++i)
            {
                const float u = sw * (float) i / (float) cols;
                const int k = j * vc + i;
                float *g = &grid[k * 4];

                float px = u + offsets[k * 2];
                const float py = v + offsets[k * 2 + 1];
                if (mirrored)
                    px = sw - px;

                float tu = u, tv = v;
                if (uv)
                {
                    tu = shiftTex(u, uvOffsets[k * 2], sw);
                    tv = shiftTex(v, uvOffsets[k * 2 + 1], sh);
                }

                g[0] = px;
                g[1] = py;
                g[2] = (sx + tu) * texScaleX;
                g[3] = (sy + tv) * texScaleY;
            }
        }

        /* Same corner order as Quad::setPosRect: TL, TR, BR, BL */
        static const int corner[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i)
                for (int c = 0; c < 4; ++c, ++vert)
                {
                    const float *g = &grid[((j + corner[c][1]) * vc + i + corner[c][0]) * 4];
                    vert->pos.x = g[0];
                    vert->pos.y = g[1];
                    vert->texPos.x = g[2];
                    vert->texPos.y = g[3];
                }
    }

    static int padQuadCount(int c, int r) { return 2 * (c + r) + 4; }

    /* After build(): a ring of quads `pad` picture pixels wide around the
     * warped edge, so a user shader with shader_pad can draw outside the
     * src_rect. Its texture side lies outside the src_rect (read as clear). */
    template<class V>
    void buildPad(V *vert, float pad, bool mirrored,
                  float texScaleX, float texScaleY) const
    {
        const int vc = cols + 1;
        auto put = [&](const float *g, int ux, int uy)
        {
            vert->pos.x = g[0] + (float) (mirrored ? -ux : ux) * pad;
            vert->pos.y = g[1] + (float) uy * pad;
            vert->texPos.x = g[2] + (float) ux * pad * texScaleX;
            vert->texPos.y = g[3] + (float) uy * pad * texScaleY;
            ++vert;
        };
        for (int i = 0; i < cols; ++i)
        {
            const float *a = &grid[i * 4], *b = &grid[(i + 1) * 4];
            put(a, 0, -1); put(b, 0, -1); put(b, 0, 0); put(a, 0, 0);
            a = &grid[(rows * vc + i) * 4]; b = &grid[(rows * vc + i + 1) * 4];
            put(a, 0, 0); put(b, 0, 0); put(b, 0, 1); put(a, 0, 1);
        }
        for (int j = 0; j < rows; ++j)
        {
            const float *a = &grid[(j * vc) * 4], *b = &grid[((j + 1) * vc) * 4];
            put(a, -1, 0); put(a, 0, 0); put(b, 0, 0); put(b, -1, 0);
            a = &grid[(j * vc + cols) * 4]; b = &grid[((j + 1) * vc + cols) * 4];
            put(a, 0, 0); put(a, 1, 0); put(b, 1, 0); put(b, 0, 0);
        }
        const float *tl = &grid[0], *tr = &grid[cols * 4];
        const float *bl = &grid[(rows * vc) * 4], *br = &grid[(rows * vc + cols) * 4];
        put(tl, -1, -1); put(tl, 0, -1); put(tl, 0, 0); put(tl, -1, 0);
        put(tr, 0, -1); put(tr, 1, -1); put(tr, 1, 0); put(tr, 0, 0);
        put(br, 0, 0); put(br, 1, 0); put(br, 1, 1); put(br, 0, 1);
        put(bl, -1, 0); put(bl, 0, 0); put(bl, 0, 1); put(bl, -1, 1);
    }

private:
    mutable std::vector<float> grid;

    void updateWarped()
    {
        warped = false;
        for (size_t i = 0; i < offsets.size() && !warped; ++i)
            warped = offsets[i] != 0.f;
        for (size_t i = 0; i < uvOffsets.size() && !warped; ++i)
            warped = uvOffsets[i] != 0.f;
    }

    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    /* A moved texture corner stays half a texel inside the src_rect, so
     * nearest sampling never reaches the next cell of a sheet */
    static float shiftTex(float base, float d, float size)
    {
        if (d == 0.f)
            return base;
        return size > 1.f ? clampf(base + d, 0.5f, size - 0.5f) : size * 0.5f;
    }

    void copyIn(std::vector<float> &dst, const float *values, int count) const
    {
        const int n = vertexCount() * 2;
        dst.assign(n, 0.f);
        if (!values)
            return;
        if (count > n)
            count = n;
        for (int i = 0; i < count; ++i)
        {
            const float f = values[i];
            /* NaN reads as 0, anything huge is held to a sane range */
            dst[i] = (f == f) ? clampf(f, -16384.f, 16384.f) : 0.f;
        }
    }
};

/* Dot mode (mesh_dot = k): the mesh is drawn into an off-screen target
 * where one pixel is one dot (k bitmap pixels), and that target is then
 * drawn k times larger with nearest sampling, so dots stay square. */
struct MeshDot
{
    enum { MaxDot = 4 };

    /* Target area in dots, on the dot grid of the src_rect */
    int x0, y0, w, h;

    MeshDot()
    : x0(0), y0(0), w(0), h(0)
    {}

    /* Holds every vertex position, at most one src_rect size beyond each
     * side and maxDots across */
    template<class V>
    void fit(const V *vert, size_t count, int k, float sw, float sh, int maxDots)
    {
        float minx = 0.f, miny = 0.f, maxx = sw, maxy = sh;
        if (count > 0)
        {
            minx = maxx = vert[0].pos.x;
            miny = maxy = vert[0].pos.y;
        }
        for (size_t i = 1; i < count; ++i)
        {
            const float x = vert[i].pos.x, y = vert[i].pos.y;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
        minx = minx < -sw ? -sw : minx;
        miny = miny < -sh ? -sh : miny;
        maxx = maxx > 2.f * sw ? 2.f * sw : maxx;
        maxy = maxy > 2.f * sh ? 2.f * sh : maxy;

        x0 = floorDiv(minx, k);
        y0 = floorDiv(miny, k);
        w = ceilDiv(maxx, k) - x0;
        h = ceilDiv(maxy, k) - y0;
        w = w < 1 ? 1 : (w > maxDots ? maxDots : w);
        h = h < 1 ? 1 : (h > maxDots ? maxDots : h);
    }

    /* picture pixels -> target pixels */
    void targetMatrix(float m[16], int k) const
    {
        identity(m);
        m[0] = m[5] = 1.f / (float) k;
        m[12] = (float) -x0;
        m[13] = (float) -y0;
    }

    /* Second pass quad: position in sprite space (x, y, w, h) and
     * texture rect in target pixels, mirrored like the plain quad */
    void posRect(float r[4], int k, float sw, bool mirrored) const
    {
        const float px0 = (float) (x0 * k), px1 = (float) ((x0 + w) * k);
        r[0] = mirrored ? sw - px1 : px0;
        r[1] = (float) (y0 * k);
        r[2] = px1 - px0;
        r[3] = (float) (h * k);
    }

    void texRect(float r[4], bool mirrored) const
    {
        r[0] = mirrored ? (float) w : 0.f;
        r[1] = 0.f;
        r[2] = mirrored ? (float) -w : (float) w;
        r[3] = (float) h;
    }

    /* Clip position of a picture point once drawn: proj * sprite * mirror */
    static void screenMatrix(float out[16], int vpW, int vpH, const float sprite[16],
                             float sw, bool mirrored)
    {
        float proj[16], mir[16], tmp[16];
        identity(proj);
        proj[0] = 2.f / vpW;
        proj[5] = 2.f / vpH;
        proj[10] = -2.f;
        proj[12] = proj[13] = proj[14] = -1.f;
        identity(mir);
        if (mirrored)
        {
            mir[0] = -1.f;
            mir[12] = sw;
        }
        mul(tmp, sprite, mir);
        mul(out, proj, tmp);
    }

private:
    static int floorDiv(float v, int k)
    {
        const float q = v / (float) k;
        int i = (int) q;
        return ((float) i > q) ? i - 1 : i;
    }

    static int ceilDiv(float v, int k)
    {
        const float q = v / (float) k;
        int i = (int) q;
        return ((float) i < q) ? i + 1 : i;
    }

    static void identity(float m[16])
    {
        for (int i = 0; i < 16; ++i)
            m[i] = (i % 5 == 0) ? 1.f : 0.f;
    }

    /* column-major out = a * b */
    static void mul(float out[16], const float a[16], const float b[16])
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                float v = 0.f;
                for (int i = 0; i < 4; ++i)
                    v += a[i * 4 + r] * b[c * 4 + i];
                out[c * 4 + r] = v;
            }
    }
};

#endif // MESHWARP_H
