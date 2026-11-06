// Owl Ears - the SONG SIGNATURE: what Shazam's app sends instead of sound. A C++ port of shazamlite's
// signature.py (MIT, (c) 2026 Daniel Richard Todd Back), itself byte-identical to ShazamIO's generator - the
// format worked out by SongRec's author from Shazam's own app.
//
// The physics, in short: 16 kHz mono sound is cut into 2048-sample windows (128 ms), a new one every 128 samples
// (8 ms). Each window's power spectrum (1025 bins of 7.8 Hz) is compared with its neighbours in time and frequency;
// a bin that is the strongest in a wide box around it (about +-80 Hz, and from ~0.4 s before to ~2 s after) is a
// PEAK - a "star" of the song's constellation. Room echo, noise, a cheap mic change how loud everything is, but the
// brightest stars mostly stay the brightest, at the same time and pitch. Each peak keeps its window number, a log
// magnitude and its frequency refined to 1/64 of a bin (a parabola through the bin and its two neighbours). Peaks
// are kept in three bands (250-520, 520-1450, 1450-3500 Hz). The list, a few kB, is the signature; Shazam's server
// finds the song whose constellation lines up with it.
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <complex>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace sig {
struct Peak { int pass; int mag; int bin; };          // window number, log magnitude, frequency bin x 64

static uint32_t crc32(const uint8_t* d, size_t n) {
    static uint32_t T[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; } init = true; }
    uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = T[(c ^ d[i]) & 255] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}
static std::string base64(const std::vector<uint8_t>& d) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; size_t i = 0;
    for (; i + 2 < d.size(); i += 3) { uint32_t v = d[i] << 16 | d[i + 1] << 8 | d[i + 2]; o += A[v >> 18]; o += A[(v >> 12) & 63]; o += A[(v >> 6) & 63]; o += A[v & 63]; }
    if (i + 1 == d.size()) { uint32_t v = d[i] << 16; o += A[v >> 18]; o += A[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == d.size()) { uint32_t v = d[i] << 16 | d[i + 1] << 8; o += A[v >> 18]; o += A[(v >> 12) & 63]; o += A[(v >> 6) & 63]; o += '='; }
    return o;
}

// a 2048-point FFT (radix 2), double precision as numpy's
static void fft2048(std::vector<std::complex<double>>& a) {
    const int n = 2048;
    for (int i = 1, j = 0; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len; std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (int k = 0; k < len / 2; k++) { auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; }
        }
    }
}

struct Generator {
    static constexpr int RATE = 16000;
    double maxTime = 3.1; int maxPeaks = 255;
    std::vector<double> hann;
    int16_t ring[2048] = {}; int ringPos = 0;
    std::vector<std::vector<double>> fftOut, spread; int fftPos = 0, spreadPos = 0; long fftN = 0, spreadN = 0;
    std::vector<Peak> bands[3];
    long numberSamples = 0;
    Generator() {
        hann.resize(2048); for (int n = 1; n <= 2048; n++) hann[n - 1] = 0.5 - 0.5 * std::cos(2 * M_PI * n / 2049.0);   // np.hanning(2050)[1:-1]
        fftOut.assign(256, std::vector<double>(1025, 0.0)); spread.assign(256, std::vector<double>(1025, 0.0));
    }
    int peaks() const { return (int)(bands[0].size() + bands[1].size() + bands[2].size()); }
    static int md(int a, int m) { return ((a % m) + m) % m; }

    void doFft(const int16_t* b) {
        for (int i = 0; i < 128; i++) ring[(ringPos + i) % 2048] = b[i];
        ringPos = (ringPos + 128) % 2048;
        std::vector<std::complex<double>> a(2048);
        for (int i = 0; i < 2048; i++) a[i] = hann[i] * ring[(ringPos + i) % 2048];      // oldest first
        fft2048(a);
        std::vector<double>& o = fftOut[fftPos];
        for (int i = 0; i < 1025; i++) o[i] = std::max((a[i].real() * a[i].real() + a[i].imag() * a[i].imag()) / (1 << 17), 1e-10);
        fftPos = (fftPos + 1) % 256; fftN++;
    }
    void doSpreading() {
        const std::vector<double>& o = fftOut[md(fftPos - 1, 256)];
        std::vector<double> s(1025);
        for (int i = 0; i < 1025; i++) s[i] = i < 1022 ? std::max(o[i], std::max(o[i + 1], o[i + 2])) : o[i];
        int i1 = md(spreadPos - 1, 256), i2 = md(spreadPos - 3, 256), i3 = md(spreadPos - 6, 256);
        for (int i = 0; i < 1025; i++) {
            double a = std::max(s[i], spread[i1][i]); double b = std::max(a, spread[i2][i]); double c = std::max(b, spread[i3][i]);
            spread[i1][i] = a; spread[i2][i] = b; spread[i3][i] = c;
        }
        spread[spreadPos] = s; spreadPos = (spreadPos + 1) % 256; spreadN++;
    }
    void doRecognition() {
        const std::vector<double>& f46 = fftOut[md(fftPos - 46, 256)];
        const std::vector<double>& f49 = spread[md(spreadPos - 49, 256)];
        static const int NB[] = {-10, -7, -4, -3, 1, 2, 5, 8};
        static const int OT[] = {-53, -45, 165, 172, 179, 186, 193, 200, 214, 221, 228, 235, 242, 249};
        for (int b = 10; b < 1015; b++) {
            if (!(f46[b] >= 1.0 / 64 && f46[b] >= f49[b - 1])) continue;
            double mn = 0; for (int o : NB) mn = std::max(f49[b + o], mn);
            if (!(f46[b] > mn)) continue;
            double mo = mn; for (int o : OT) mo = std::max(spread[md(spreadPos + o, 256)][b - 1], mo);
            if (!(f46[b] > mo)) continue;
            long fftNumber = spreadN - 46;
            double m = std::log(std::max(1.0 / 64, f46[b])) * 1477.3 + 6144;
            double mb = std::log(std::max(1.0 / 64, f46[b - 1])) * 1477.3 + 6144;
            double ma = std::log(std::max(1.0 / 64, f46[b + 1])) * 1477.3 + 6144;
            double v1 = m * 2 - mb - ma, v2 = (ma - mb) * 32 / v1;
            double corr = b * 64 + v2, hz = corr * (RATE / 2.0 / 1024 / 64);
            int band = hz > 250 && hz < 520 ? 0 : hz > 520 && hz < 1450 ? 1 : hz > 1450 && hz < 3500 ? 2 : -1;   // (3500-5500: never, as in the original)
            if (band < 0) continue;
            bands[band].push_back({(int)fftNumber, (int)m, (int)corr});
        }
    }
    // the whole input (16 kHz mono s16): processes it in 128-sample steps while it is under maxTime OR maxPeaks
    bool make(const int16_t* x, size_t n) {
        size_t done = 0;
        while (n - done >= 128 && ((double)numberSamples / RATE < maxTime || peaks() < maxPeaks)) {
            numberSamples += 128; doFft(x + done); doSpreading(); if (spreadN >= 46) doRecognition(); done += 128;
        }
        for (auto& v : bands) std::stable_sort(v.begin(), v.end(), [](const Peak& a, const Peak& b) { return a.pass < b.pass; });
        return peaks() > 0;
    }
    std::vector<uint8_t> encode() const {
        std::vector<uint8_t> body;
        auto u32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int k = 0; k < 4; k++) v.push_back((uint8_t)(x >> (8 * k))); };
        auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
        for (int band = 0; band < 3; band++) {
            if (bands[band].empty()) continue;
            std::vector<uint8_t> pk; int last = 0;
            for (const Peak& p : bands[band]) {
                if (p.pass - last >= 255) { pk.push_back(0xFF); u32(pk, (uint32_t)p.pass); last = p.pass; }
                pk.push_back((uint8_t)(p.pass - last)); u16(pk, (uint32_t)p.mag); u16(pk, (uint32_t)p.bin); last = p.pass;
            }
            u32(body, 0x60030040u + band); u32(body, (uint32_t)pk.size());
            body.insert(body.end(), pk.begin(), pk.end());
            while (pk.size() % 4) { body.push_back(0); pk.push_back(0); }
        }
        uint32_t sizeMinusHeader = (uint32_t)body.size() + 8;
        std::vector<uint8_t> fields;
        u32(fields, 0x94119C00u); u32(fields, 0); u32(fields, 0); u32(fields, 0); u32(fields, 3u << 27); u32(fields, 0); u32(fields, 0);
        u32(fields, (uint32_t)(long)(numberSamples + RATE * 0.24)); u32(fields, (15u << 19) + 0x40000u);
        std::vector<uint8_t> crcIn; u32(crcIn, sizeMinusHeader); crcIn.insert(crcIn.end(), fields.begin(), fields.end());
        u32(crcIn, 0x40000000u); u32(crcIn, sizeMinusHeader); crcIn.insert(crcIn.end(), body.begin(), body.end());
        std::vector<uint8_t> out; u32(out, 0xCAFE2580u); u32(out, crc32(crcIn.data(), crcIn.size()));
        out.insert(out.end(), crcIn.begin(), crcIn.end());
        return out;
    }
};
// a signature from 16 kHz mono samples (empty: too little sound)
static std::vector<uint8_t> make(const int16_t* x, size_t n, int* peaksOut = nullptr) {
    Generator g; if (!g.make(x, n)) return {};
    if (peaksOut) *peaksOut = g.peaks();
    return g.encode();
}
}
