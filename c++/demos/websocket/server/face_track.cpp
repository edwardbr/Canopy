// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "face_track.h"

#include <cmath>

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            // Empirical 8-bit YCbCr skin cluster (BT.601). Wide enough to
            // cover a range of skin tones; the mean/spread reduction below
            // tolerates the inevitable stray matches.
            constexpr int CB_MIN = 77;
            constexpr int CB_MAX = 127;
            constexpr int CR_MIN = 133;
            constexpr int CR_MAX = 173;
            constexpr int Y_MIN = 60; // ignore very dark pixels

            constexpr float EMA_ALPHA = 0.35f; // higher = snappier, lower = smoother
        }

        face_box face_tracker::track(const vpx_image_t* img)
        {
            face_box out;

            const int fw = static_cast<int>(img->d_w);
            const int fh = static_cast<int>(img->d_h);
            const int cw = (fw + 1) / 2;
            const int ch = (fh + 1) / 2;

            const unsigned char* yp = img->planes[VPX_PLANE_Y];
            const unsigned char* up = img->planes[VPX_PLANE_U];
            const unsigned char* vp = img->planes[VPX_PLANE_V];
            const int ys = img->stride[VPX_PLANE_Y];
            const int us = img->stride[VPX_PLANE_U];
            const int vs = img->stride[VPX_PLANE_V];

            // Accumulate skin pixels in chroma coords; track mean + variance
            // so we can take a robust spread instead of a min/max box that a
            // few stray matches would blow up.
            double n = 0.0;
            double sx = 0.0;
            double sy = 0.0;
            double sxx = 0.0;
            double syy = 0.0;
            for (int yy = 0; yy < ch; ++yy)
            {
                const unsigned char* urow = up + yy * us;
                const unsigned char* vrow = vp + yy * vs;
                const unsigned char* yrow = yp + (yy * 2) * ys;
                for (int xx = 0; xx < cw; ++xx)
                {
                    const int cb = urow[xx];
                    const int cr = vrow[xx];
                    if (cb < CB_MIN || cb > CB_MAX || cr < CR_MIN || cr > CR_MAX)
                        continue;
                    if (yrow[xx * 2] < Y_MIN)
                        continue;
                    n += 1.0;
                    sx += xx;
                    sy += yy;
                    sxx += static_cast<double>(xx) * xx;
                    syy += static_cast<double>(yy) * yy;
                }
            }

            // Need a meaningful blob (>= ~0.5% of chroma pixels) to trust it.
            const double min_n = (static_cast<double>(cw) * ch) / 200.0;
            if (n < min_n || n < 8.0)
            {
                out.valid = false;
                return out;
            }

            const double mx = sx / n;
            const double my = sy / n;
            const double vx = sxx / n - mx * mx;
            const double vy = syy / n - my * my;
            const double sdx = vx > 0.0 ? std::sqrt(vx) : 1.0;
            const double sdy = vy > 0.0 ? std::sqrt(vy) : 1.0;

            // Box = mean ± 1.5 sigma, mapped chroma -> full-frame (x2).
            const float raw_cx = static_cast<float>(mx * 2.0);
            const float raw_cy = static_cast<float>(my * 2.0);
            const float raw_w = static_cast<float>(sdx * 2.0 * 3.0); // 1.5 sigma each side, x2 chroma
            const float raw_h = static_cast<float>(sdy * 2.0 * 3.0);

            if (!initialised_)
            {
                ema_cx_ = raw_cx;
                ema_cy_ = raw_cy;
                ema_w_ = raw_w;
                ema_h_ = raw_h;
                initialised_ = true;
            }
            else
            {
                ema_cx_ += EMA_ALPHA * (raw_cx - ema_cx_);
                ema_cy_ += EMA_ALPHA * (raw_cy - ema_cy_);
                ema_w_ += EMA_ALPHA * (raw_w - ema_w_);
                ema_h_ += EMA_ALPHA * (raw_h - ema_h_);
            }

            out.cx = static_cast<int>(ema_cx_);
            out.cy = static_cast<int>(ema_cy_);
            out.w = static_cast<int>(ema_w_);
            out.h = static_cast<int>(ema_h_);
            out.valid = true;
            return out;
        }
    }
}
