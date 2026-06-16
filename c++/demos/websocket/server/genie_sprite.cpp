// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "genie_sprite.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            constexpr int SPR_W = 200;
            constexpr int SPR_H = 130;

            struct rgba
            {
                uint8_t r, g, b, a;
            };

            inline uint8_t clamp8(int v)
            {
                return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            }

            // Source-over blend of one colour onto an existing RGBA pixel.
            void blend_px(
                std::vector<uint8_t>& buf,
                int x,
                int y,
                rgba c)
            {
                if (x < 0 || y < 0 || x >= SPR_W || y >= SPR_H || c.a == 0)
                    return;
                uint8_t* p = &buf[(static_cast<size_t>(y) * SPR_W + x) * 4];
                const int a = c.a;
                p[0] = clamp8((c.r * a + p[0] * (255 - a) + 127) / 255);
                p[1] = clamp8((c.g * a + p[1] * (255 - a) + 127) / 255);
                p[2] = clamp8((c.b * a + p[2] * (255 - a) + 127) / 255);
                p[3] = static_cast<uint8_t>(std::min(255, p[3] + a));
            }

            void fill_ellipse(
                std::vector<uint8_t>& buf,
                int cx,
                int cy,
                int rx,
                int ry,
                rgba c)
            {
                if (rx <= 0 || ry <= 0)
                    return;
                for (int y = cy - ry; y <= cy + ry; ++y)
                    for (int x = cx - rx; x <= cx + rx; ++x)
                    {
                        const double nx = (x - cx) / static_cast<double>(rx);
                        const double ny = (y - cy) / static_cast<double>(ry);
                        if (nx * nx + ny * ny <= 1.0)
                            blend_px(buf, x, y, c);
                    }
            }

            void fill_rect(
                std::vector<uint8_t>& buf,
                int x0,
                int y0,
                int x1,
                int y1,
                rgba c)
            {
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        blend_px(buf, x, y, c);
            }

            // Filled triangle via edge-function sign test over the bounding box.
            void fill_triangle(
                std::vector<uint8_t>& buf,
                int ax,
                int ay,
                int bx,
                int by,
                int cx,
                int cy,
                rgba col)
            {
                const int minx = std::min({ax, bx, cx});
                const int maxx = std::max({ax, bx, cx});
                const int miny = std::min({ay, by, cy});
                const int maxy = std::max({ay, by, cy});
                auto edge = [](int x0, int y0, int x1, int y1, int px, int py)
                { return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0); };
                for (int y = miny; y <= maxy; ++y)
                    for (int x = minx; x <= maxx; ++x)
                    {
                        const int w0 = edge(ax, ay, bx, by, x, y);
                        const int w1 = edge(bx, by, cx, cy, x, y);
                        const int w2 = edge(cx, cy, ax, ay, x, y);
                        if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                            blend_px(buf, x, y, col);
                    }
            }

            // Build the crown sprite once. Procedural (no asset file) so the
            // build stays deterministic: a gold band with five jewelled points
            // and a pearl trim. Replaceable later with real artwork without
            // touching callers. Drawn wide-then-short so the aspect ratio reads
            // as a crown when the caller scales it to head width.
            const std::vector<uint8_t>& sprite()
            {
                static const std::vector<uint8_t> data = []
                {
                    std::vector<uint8_t> buf(static_cast<size_t>(SPR_W) * SPR_H * 4, 0);

                    const rgba gold{235, 190, 70, 255};
                    const rgba gold_dark{165, 125, 35, 255};
                    const rgba pearl{255, 250, 235, 255};
                    const rgba ruby{220, 40, 60, 255};
                    const rgba sapphire{60, 110, 225, 255};
                    const rgba emerald{40, 180, 120, 255};

                    // Five points rising from the band top (y = 78). Centre
                    // tallest, outer ones lowest, so the silhouette reads as a
                    // crown. Base edges tile [16,184] into five segments.
                    const int base_y = 78;
                    const int ex[6] = {16, 50, 84, 116, 150, 184};
                    const int apex_x[5] = {33, 67, 100, 133, 167};
                    const int apex_y[5] = {44, 26, 8, 26, 44};
                    for (int i = 0; i < 5; ++i)
                        fill_triangle(buf, ex[i], base_y, ex[i + 1], base_y, apex_x[i], apex_y[i], gold);

                    // Band.
                    fill_rect(buf, 16, base_y, 184, 116, gold);
                    // Darker base trim.
                    fill_rect(buf, 16, 112, 184, 122, gold_dark);

                    // Jewel ball at each point's tip.
                    const rgba tip[5] = {sapphire, emerald, ruby, emerald, sapphire};
                    for (int i = 0; i < 5; ++i)
                        fill_ellipse(buf, apex_x[i], apex_y[i] + 4, 7, 7, tip[i]);

                    // Band jewels: ruby centred, sapphires either side.
                    fill_ellipse(buf, 100, 95, 11, 12, ruby);
                    fill_ellipse(buf, 58, 96, 8, 9, sapphire);
                    fill_ellipse(buf, 142, 96, 8, 9, sapphire);

                    // Pearl trim along the bottom edge.
                    for (int x = 24; x <= 176; x += 17)
                        fill_ellipse(buf, x, 124, 5, 5, pearl);

                    return buf;
                }();
                return data;
            }

            inline uint8_t rgb2y(
                int r,
                int g,
                int b)
            {
                return clamp8((77 * r + 150 * g + 29 * b) >> 8);
            }
            inline uint8_t rgb2u(
                int r,
                int g,
                int b)
            {
                return clamp8(((-43 * r - 84 * g + 127 * b) >> 8) + 128);
            }
            inline uint8_t rgb2v(
                int r,
                int g,
                int b)
            {
                return clamp8(((127 * r - 106 * g - 21 * b) >> 8) + 128);
            }

            inline uint8_t mix(
                uint8_t dst,
                uint8_t src,
                int a)
            {
                return clamp8((src * a + dst * (255 - a) + 127) / 255);
            }
        }

        void genie_sprite_native_size(
            int& w,
            int& h)
        {
            w = SPR_W;
            h = SPR_H;
        }

        void composite_genie_sprite(
            vpx_image_t* img,
            int ox,
            int oy,
            int draw_w,
            int draw_h)
        {
            const int fw = static_cast<int>(img->d_w);
            const int fh = static_cast<int>(img->d_h);
            if (draw_w <= 0 || draw_h <= 0)
                return;

            unsigned char* yp = img->planes[VPX_PLANE_Y];
            unsigned char* up = img->planes[VPX_PLANE_U];
            unsigned char* vp = img->planes[VPX_PLANE_V];
            const int ys = img->stride[VPX_PLANE_Y];
            const int us = img->stride[VPX_PLANE_U];
            const int vs = img->stride[VPX_PLANE_V];

            const std::vector<uint8_t>& spr = sprite();

            // Iterate destination pixels in the target rect; nearest-neighbour
            // sample the fixed-size source sprite. Clipped to the frame.
            for (int ddy = 0; ddy < draw_h; ++ddy)
            {
                const int dy = oy + ddy;
                if (dy < 0 || dy >= fh)
                    continue;
                const int sy = ddy * SPR_H / draw_h;
                for (int ddx = 0; ddx < draw_w; ++ddx)
                {
                    const int dx = ox + ddx;
                    if (dx < 0 || dx >= fw)
                        continue;
                    const int sx = ddx * SPR_W / draw_w;
                    const uint8_t* px = &spr[(static_cast<size_t>(sy) * SPR_W + sx) * 4];
                    const int a = px[3];
                    if (a == 0)
                        continue;

                    yp[dy * ys + dx] = mix(yp[dy * ys + dx], rgb2y(px[0], px[1], px[2]), a);

                    const int cx = dx >> 1;
                    const int cy = dy >> 1;
                    up[cy * us + cx] = mix(up[cy * us + cx], rgb2u(px[0], px[1], px[2]), a);
                    vp[cy * vs + cx] = mix(vp[cy * vs + cx], rgb2v(px[0], px[1], px[2]), a);
                }
            }
        }
    }
}
