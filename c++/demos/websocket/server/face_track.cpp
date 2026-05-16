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

            // Accumulate skin-chroma pixel stats (mean + variance) over a
            // region. cxw/cyw/rxw/ryw is an optional gate: when valid, only
            // pixels inside that chroma-space window are counted. A first
            // ungated pass finds the cluster centre; a second pass gated to
            // ~1.2 sigma around it discards the diffuse background tail
            // (e.g. wood panelling shares skin's CbCr cluster) so the box
            // hugs the dominant blob instead of being blown up by it.
            struct stats
            {
                double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0;
            };
            auto accumulate = [&](double cxw, double cyw, double rxw, double ryw, bool gated) {
                stats s;
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
                        if (gated && (std::fabs(xx - cxw) > rxw || std::fabs(yy - cyw) > ryw))
                            continue;
                        s.n += 1.0;
                        s.sx += xx;
                        s.sy += yy;
                        s.sxx += static_cast<double>(xx) * xx;
                        s.syy += static_cast<double>(yy) * yy;
                    }
                }
                return s;
            };

            stats st = accumulate(0, 0, 0, 0, false);

            // Need a meaningful blob (>= ~0.5% of chroma pixels) to trust it.
            const double min_n = (static_cast<double>(cw) * ch) / 200.0;
            if (st.n < min_n || st.n < 8.0)
            {
                out.valid = false;
                return out;
            }

            // Re-estimate over a trimmed window around the first-pass centre
            // so a large off-face skin-toned region (wood, hands) can't
            // inflate the spread. Keep the trimmed result only if it still
            // holds enough pixels to be the real blob.
            {
                const double mx0 = st.sx / st.n;
                const double my0 = st.sy / st.n;
                const double vx0 = st.sxx / st.n - mx0 * mx0;
                const double vy0 = st.syy / st.n - my0 * my0;
                const double sdx0 = vx0 > 0.0 ? std::sqrt(vx0) : 1.0;
                const double sdy0 = vy0 > 0.0 ? std::sqrt(vy0) : 1.0;
                stats trimmed = accumulate(mx0, my0, 1.2 * sdx0, 1.2 * sdy0, true);
                if (trimmed.n >= min_n && trimmed.n >= st.n * 0.25)
                    st = trimmed;
            }

            const double mx = st.sx / st.n;
            const double my = st.sy / st.n;
            const double vx = st.sxx / st.n - mx * mx;
            const double vy = st.syy / st.n - my * my;
            const double sdx = vx > 0.0 ? std::sqrt(vx) : 1.0;
            const double sdy = vy > 0.0 ? std::sqrt(vy) : 1.0;

            // Box = mean ± 1.5 sigma, mapped chroma -> full-frame (x2), then
            // clamped to a plausible face extent so background pollution can
            // never produce a wildly oversized box.
            const float raw_cx = static_cast<float>(mx * 2.0);
            const float raw_cy = static_cast<float>(my * 2.0);
            float raw_w = static_cast<float>(sdx * 2.0 * 3.0); // 1.5 sigma each side, x2 chroma
            float raw_h = static_cast<float>(sdy * 2.0 * 3.0);
            const float max_w = fw * 0.45f;
            const float max_h = fh * 0.55f;
            const float min_w = fw * 0.06f;
            const float min_h = fh * 0.06f;
            raw_w = raw_w > max_w ? max_w : (raw_w < min_w ? min_w : raw_w);
            raw_h = raw_h > max_h ? max_h : (raw_h < min_h ? min_h : raw_h);

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
