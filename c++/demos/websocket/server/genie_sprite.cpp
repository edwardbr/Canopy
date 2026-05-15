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
            constexpr int SPR_W = 140;
            constexpr int SPR_H = 180;

            struct rgba
            {
                uint8_t r, g, b, a;
            };

            inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

            // Source-over blend of one colour onto an existing RGBA pixel.
            void blend_px(std::vector<uint8_t>& buf, int x, int y, rgba c)
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

            void fill_ellipse(std::vector<uint8_t>& buf, int cx, int cy, int rx, int ry, rgba c)
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

            // Build the genie+lamp sprite once. Recognisable placeholder, not
            // art: a gold lamp, a translucent blue plume widening into a round
            // genie head with eyes. Replaceable later with real artwork.
            const std::vector<uint8_t>& sprite()
            {
                static const std::vector<uint8_t> data = [] {
                    std::vector<uint8_t> buf(static_cast<size_t>(SPR_W) * SPR_H * 4, 0);

                    const rgba gold{214, 170, 60, 255};
                    const rgba gold_dark{150, 115, 35, 255};
                    const rgba genie{90, 170, 255, 170};
                    const rgba genie_soft{120, 195, 255, 120};
                    const rgba white{255, 255, 255, 255};
                    const rgba pupil{20, 30, 60, 255};

                    // Lamp (bottom).
                    fill_ellipse(buf, 70, 150, 46, 22, gold);          // body
                    fill_ellipse(buf, 24, 146, 16, 9, gold);           // spout
                    fill_ellipse(buf, 70, 132, 11, 8, gold_dark);      // lid base
                    fill_ellipse(buf, 70, 124, 5, 6, gold);            // knob

                    // Plume rising from the lid, narrow -> wide going up.
                    for (int y = 120; y >= 86; y -= 2)
                    {
                        const int t = 120 - y;             // 0..34
                        const int rx = 6 + t / 2;          // widen upward
                        fill_ellipse(buf, 70, y, rx, 8, genie_soft);
                    }

                    // Genie head.
                    fill_ellipse(buf, 70, 56, 38, 40, genie);
                    // Eyes.
                    fill_ellipse(buf, 57, 50, 8, 9, white);
                    fill_ellipse(buf, 83, 50, 8, 9, white);
                    fill_ellipse(buf, 58, 51, 3, 4, pupil);
                    fill_ellipse(buf, 84, 51, 3, 4, pupil);
                    // Topknot.
                    fill_ellipse(buf, 70, 16, 6, 9, genie);

                    return buf;
                }();
                return data;
            }

            inline uint8_t rgb2y(int r, int g, int b) { return clamp8((77 * r + 150 * g + 29 * b) >> 8); }
            inline uint8_t rgb2u(int r, int g, int b) { return clamp8(((-43 * r - 84 * g + 127 * b) >> 8) + 128); }
            inline uint8_t rgb2v(int r, int g, int b) { return clamp8(((127 * r - 106 * g - 21 * b) >> 8) + 128); }

            inline uint8_t mix(uint8_t dst, uint8_t src, int a) { return clamp8((src * a + dst * (255 - a) + 127) / 255); }
        }

        void genie_sprite_native_size(int& w, int& h)
        {
            w = SPR_W;
            h = SPR_H;
        }

        void composite_genie_sprite(vpx_image_t* img, int ox, int oy, int draw_w, int draw_h)
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
