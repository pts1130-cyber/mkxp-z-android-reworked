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

#endif // MESHWARP_H
