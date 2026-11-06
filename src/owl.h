// Owl Ears - THE OWL and the pictures: drawn into a plain ARGB pixel buffer (the window then shows it as it is).
//   The owl: straight lines on the dark, two amber eyes that glow with the beat of what plays (never quite out). While
//   it listens its ear tufts twitch with the sound and its pupils open wide; when nothing matched it blinks, slowly;
//   now and then it just blinks.
//   Pictures: drawn smooth (bilinear), fitted or filling; a cover's soft, dim double for behind it.
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace owl {
struct Pix {                                              // ARGB (0xAARRGGBB): in memory B, G, R, A - as Windows' 32-bit DIBs
    int W = 0, H = 0; std::vector<uint32_t> px;
    void resize(int w, int h) { if (w != W || h != H) { W = w; H = h; px.assign((size_t)w * h, 0xFF000000u); } }
    void fill(uint32_t c) { std::fill(px.begin(), px.end(), c); }
};
static inline uint32_t argb(float r, float g, float b) {
    auto c = [](float v) { return (uint32_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f); };
    return 0xFF000000u | c(r) << 16 | c(g) << 8 | c(b);
}
static inline void add(Pix& p, int x, int y, float r, float g, float b, float a) {   // light added (lines, glow)
    if (x < 0 || y < 0 || x >= p.W || y >= p.H || a <= 0.f) return;
    uint32_t& d = p.px[(size_t)y * p.W + x];
    d = argb(((d >> 16) & 255) / 255.f + r * a, ((d >> 8) & 255) / 255.f + g * a, (d & 255) / 255.f + b * a);
}
static inline void mixPx(Pix& p, int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= p.W || y >= p.H || a <= 0.f) return;
    uint32_t& d = p.px[(size_t)y * p.W + x];
    d = argb(((d >> 16) & 255) / 255.f * (1 - a) + r * a, ((d >> 8) & 255) / 255.f * (1 - a) + g * a, (d & 255) / 255.f * (1 - a) + b * a);
}
// an RGBA picture (bytes R, G, B, A) into the rect: crop = fill it (the overhang cut), else fit; dim: its brightness
static void blit(Pix& p, const uint8_t* px, int w, int h, float x0, float y0, float x1, float y1, float dim = 1.f, bool crop = true) {
    if (!px || w <= 0 || h <= 0) return;
    float rw = x1 - x0, rh = y1 - y0, sa = (float)w / h, ra = rw / rh;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (crop) { if (sa > ra) { float k = ra / sa; u0 = 0.5f - k * 0.5f; u1 = 0.5f + k * 0.5f; } else { float k = sa / ra; v0 = 0.5f - k * 0.5f; v1 = 0.5f + k * 0.5f; } }
    else { if (sa > ra) { float nh = rw / sa; y0 += (rh - nh) * 0.5f; rh = nh; y1 = y0 + rh; } else { float nw = rh * sa; x0 += (rw - nw) * 0.5f; rw = nw; x1 = x0 + rw; } }
    int X0 = std::max(0, (int)std::floor(x0)), X1 = std::min(p.W - 1, (int)std::ceil(x1)), Y0 = std::max(0, (int)std::floor(y0)), Y1 = std::min(p.H - 1, (int)std::ceil(y1));
    for (int y = Y0; y <= Y1; y++) for (int x = X0; x <= X1; x++) {
        float fx = (x + 0.5f - x0) / rw, fy = (y + 0.5f - y0) / rh; if (fx < 0 || fy < 0 || fx > 1 || fy > 1) continue;
        float sx = (u0 + fx * (u1 - u0)) * w - 0.5f, sy = (v0 + fy * (v1 - v0)) * h - 0.5f;
        int ix = std::max(0, std::min(w - 1, (int)std::floor(sx))), iy = std::max(0, std::min(h - 1, (int)std::floor(sy)));
        int ix1 = std::min(w - 1, ix + 1), iy1 = std::min(h - 1, iy + 1); float tx = std::max(0.f, sx - ix), ty = std::max(0.f, sy - iy);
        float c[4];
        for (int k = 0; k < 4; k++) {
            auto P = [&](int a, int b) { return px[((size_t)b * w + a) * 4 + k] / 255.f; };
            c[k] = (P(ix, iy) * (1 - tx) + P(ix1, iy) * tx) * (1 - ty) + (P(ix, iy1) * (1 - tx) + P(ix1, iy1) * tx) * ty;
        }
        mixPx(p, x, y, c[0] * dim, c[1] * dim, c[2] * dim, c[3]);
    }
}
// a picture averaged down to n x n: drawn up again it is a soft glow of its colours
static std::vector<uint8_t> tiny(const uint8_t* px, int w, int h, int n = 8) {
    std::vector<uint8_t> o((size_t)n * n * 4, 0);
    for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
        double acc[4] = {}; int cnt = 0;
        for (int y = j * h / n; y < (j + 1) * h / n; y += 2) for (int x = i * w / n; x < (i + 1) * w / n; x += 2) { for (int k = 0; k < 4; k++) acc[k] += px[((size_t)y * w + x) * 4 + k]; cnt++; }
        for (int k = 0; k < 4; k++) o[((size_t)j * n + i) * 4 + k] = (uint8_t)(cnt ? acc[k] / cnt : 0);
    }
    return o;
}
// an antialiased line of light, with a faint glow round it
static void line(Pix& p, float x0, float y0, float x1, float y1, float w, float r, float g, float b, float a = 1.f) {
    float gl = w * 3.f;
    int X0 = (int)std::floor(std::min(x0, x1) - gl), X1 = (int)std::ceil(std::max(x0, x1) + gl), Y0 = (int)std::floor(std::min(y0, y1) - gl), Y1 = (int)std::ceil(std::max(y0, y1) + gl);
    X0 = std::max(0, X0); Y0 = std::max(0, Y0); X1 = std::min(p.W - 1, X1); Y1 = std::min(p.H - 1, Y1);
    float dx = x1 - x0, dy = y1 - y0, L2 = std::max(1e-6f, dx * dx + dy * dy);
    for (int y = Y0; y <= Y1; y++) for (int x = X0; x <= X1; x++) {
        float px_ = x + 0.5f - x0, py = y + 0.5f - y0, t = std::max(0.f, std::min(1.f, (px_ * dx + py * dy) / L2));
        float ex = px_ - t * dx, ey = py - t * dy, d = std::sqrt(ex * ex + ey * ey);
        float core = std::min(1.f, std::max(0.f, w * 0.5f + 0.5f - d)), glow = std::exp(-d / (w * 1.2f)) * 0.22f;
        add(p, x, y, r, g, b, a * std::max(core, glow));
    }
}
static void ring(Pix& p, float cx, float cy, float rad, float w, float r, float g, float b, float a, float squash) {
    int n = std::max(24, (int)(rad * 0.9f));
    for (int i = 0; i < n; i++) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        line(p, cx + rad * std::cos(a0), cy + rad * std::sin(a0) * squash, cx + rad * std::cos(a1), cy + rad * std::sin(a1) * squash, w, r, g, b, a);
    }
}
static void glow(Pix& p, float cx, float cy, float rad, float r, float g, float b, float a, float squash = 1.f) {
    int R = (int)std::ceil(rad * 2.6f);
    for (int y = -R; y <= R; y++) for (int x = -R; x <= R; x++) {
        float yy = y / std::max(0.05f, squash), d = std::sqrt((float)(x * x) + yy * yy) / rad;
        float v = d < 1.f ? 1.f - 0.35f * d * d : std::exp(-(d - 1.f) * 2.2f) * 0.65f * std::max(0.f, 1.f - (d - 1.f) / 1.6f);
        add(p, (int)cx + x, (int)cy + y, r, g, b, a * v);
    }
}
// THE OWL, fitted in a square of side s around (cx, cy). eye 0..1: the eyes' glow (keep it above ~0.2); twitch: the
// ear tufts' kick; open: the pupils (0 calm .. 1 wide); blink: 0 open .. 1 shut; t: seconds (the twitch's wobble)
static void draw(Pix& pix, float cx, float cy, float s, float eye, float twitch, float open, float blink, float t) {
    const float L[3] = {0.66f, 0.80f, 0.88f};
    float u = s / 100.f, w = std::max(1.2f, 1.25f * u);
    auto ln = [&](float ax, float ay, float bx, float by, float k = 1.f) { line(pix, cx + ax * u, cy + ay * u, cx + bx * u, cy + by * u, w, L[0], L[1], L[2], 0.85f * k); };
    float tw = twitch * 4.f * std::sin(t * 31.f);
    ln(-30, -24, -38, -44 - tw); ln(-38, -44 - tw, -18, -30);      // the ear tufts
    ln(30, -24, 38, -44 + tw); ln(38, -44 + tw, 18, -30);
    ln(-18, -30, 18, -30);                                          // the brow
    ln(-30, -24, -34, 8); ln(30, -24, 34, 8);                       // the head
    ln(-34, 8, -22, 30); ln(34, 8, 22, 30);
    ln(-28, -6, -26, 6, 0.45f); ln(28, -6, 26, 6, 0.45f);           // the facial disc
    ln(-26, 6, -8, 14, 0.45f); ln(26, 6, 8, 14, 0.45f);
    ln(-4, -4, 0, 6); ln(4, -4, 0, 6);                              // the beak
    ln(-22, 30, -24, 52); ln(22, 30, 24, 52); ln(-24, 52, 24, 52);  // the body
    for (int k = 0; k < 3; k++) { float y = 32 + k * 6.f; ln(-10 + k * 1.5f, y, 0, y + 4, 0.55f); ln(10 - k * 1.5f, y, 0, y + 4, 0.55f); }
    ln(-10, 52, -12, 57, 0.7f); ln(-10, 52, -8, 57, 0.7f); ln(10, 52, 8, 57, 0.7f); ln(10, 52, 12, 57, 0.7f);
    for (int sd = -1; sd <= 1; sd += 2) {                           // the eyes
        float ex = cx + sd * 14.f * u, ey = cy - 14.f * u, sq = std::max(0.06f, 1.f - blink), E = std::max(0.f, std::min(1.f, eye));
        glow(pix, ex, ey, 9.5f * u, 1.0f, 0.58f, 0.16f, 0.30f + 0.70f * E, sq);
        glow(pix, ex, ey, 15.f * u, 1.0f, 0.45f, 0.10f, 0.10f * E, sq);
        ring(pix, ex, ey, 11.5f * u, w, L[0], L[1], L[2], 0.9f, sq);
        ring(pix, ex, ey, 13.8f * u, w * 0.8f, L[0], L[1], L[2], 0.35f, sq);
        if (sq > 0.3f) {
            float pr = (3.2f + 3.0f * open) * u; int R = (int)std::ceil(pr) + 1;
            for (int y = -R; y <= R; y++) for (int x = -R; x <= R; x++) {
                float d = std::sqrt((float)(x * x + y * y)), a = std::min(1.f, std::max(0.f, pr + 0.5f - d));
                int X = (int)ex + x, Y = (int)(ey + y * sq);
                if (X < 0 || Y < 0 || X >= pix.W || Y >= pix.H || a <= 0) continue;
                uint32_t& q = pix.px[(size_t)Y * pix.W + X]; float k = 1.f - 0.9f * a;
                q = argb(((q >> 16) & 255) / 255.f * k, ((q >> 8) & 255) / 255.f * k, (q & 255) / 255.f * k);
            }
            glow(pix, ex - pr * 0.42f, ey - pr * 0.42f * sq, std::max(0.8f, pr * 0.22f), 1.f, 0.95f, 0.85f, 0.8f);
        }
    }
}
}
