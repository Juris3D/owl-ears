// Owl Ears - a song recogniser for Windows: what song is this? (Shazam's recognition, unofficial, the way shazamlite
// does it). With the owl: it watches while nothing is found, listens along, its eyes glow with the beat.
//   PC SOUND: what the PC plays (WASAPI loopback - Spotify, a browser, TA-8090...)
//   MICROPHONE: what the room plays (the hi-fi, the radio)
//   FILE: a music file (from a third into it)
// It listens in 3.1 s pieces (the length shazamlite sends): after ~3.5 s the first try, then a fresh piece every
// ~3.5 s, four tries at most. Only the signature (the peaks, a few kB) leaves the PC - never the sound.
// Thunder Owl / TA-8090 project. Signature: a port of shazamlite (MIT). UNOFFICIAL use of Shazam's service.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <ctime>
#include <algorithm>
#include "ears/signature.h"
#include "ears/shazam.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"
#include "../third_party/miniaudio.h"
#include "owl.h"

#define APP_TITLE L"Owl Ears - song recognition (v3.1)"
static const int RATE = 16000, PIECE = 49600;                  // 3.1 s at 16 kHz

// ------------------------------------------------------------------ text
static std::wstring W(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n); w.resize(n - 1); return w; }
static std::string U(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr); s.resize(n - 1); return s; }

// ------------------------------------------------------------------ HTTPS (WinHTTP)
static bool https(const std::wstring& method, const std::wstring& host, const std::wstring& path, const std::string& headers, const std::string& body,
                  std::string& out, int& status, std::string& err) {
    out.clear(); status = 0;
    HINTERNET s = WinHttpOpen(W(shz::UA).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(W(shz::UA).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "no network"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = false;
    if (r) {
        DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL; WinHttpSetOption(r, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);
        std::wstring h = W(headers);
        if (WinHttpSendRequest(r, h.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : h.c_str(), h.empty() ? 0 : (DWORD)-1L,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD st = 0, sz = sizeof st;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            for (;;) { char b[16384]; DWORD got = 0; if (!WinHttpReadData(r, b, sizeof b, &got) || !got) break; out.append(b, got); if (out.size() > (8u << 20)) break; }
            ok = true;
        } else { char b[64]; snprintf(b, sizeof b, "connection failed (%lu)", GetLastError()); err = b; }
    } else err = "connection failed";
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return ok;
}
static bool httpsGet(const std::string& url, std::string& out) {
    size_t a = url.find("://"); if (a == std::string::npos) return false;
    size_t p = url.find('/', a + 3); std::string host = url.substr(a + 3, p == std::string::npos ? std::string::npos : p - a - 3), path = p == std::string::npos ? "/" : url.substr(p);
    int st; std::string err; return https(L"GET", W(host), W(path), std::string("User-Agent: ") + shz::UA + "\r\n", "", out, st, err) && st == 200;
}


// ------------------------------------------------------------------ the state the window shows
struct Hist { std::string when, artist, title, url; };
enum { V_CLASSIC = 0, V_PICTURE, VIEW_N };
static const wchar_t* VIEW_NAMES[VIEW_N] = {L"CLASSIC", L"PICTURE"};
enum { FB_OWL = 0, FB_PICTURE, FB_FOLDER, FB_N };
static const wchar_t* FB_NAMES[FB_N] = {L"THE OWL", L"MY PICTURE", L"A FOLDER OF PICTURES"};
static const int EVERY_N = 4; static const int EVERY_S[EVERY_N] = {30, 60, 120, 300}; static const wchar_t* EVERY_NAMES[EVERY_N] = {L"30 S", L"1 MIN", L"2 MIN", L"5 MIN"};
static struct App {
    std::mutex m;
    std::string status = "Choose what to listen to.", note;
    int busy = 0;                                  // 0 idle, 1 listening, 2 asking, 3 waiting (KEEP LISTENING)
    int source = 0;                                // 0 PC sound, 1 microphone, 2 file
    std::atomic<float> level{0.f};
    float heard = 0.f;                             // seconds heard this time
    shz::Track track; std::vector<uint8_t> cover; int coverW = 0, coverH = 0;   // (RGBA)
    std::string stats;
    std::vector<Hist> hist;
    // settings (settings.ini in the data folder)
    bool keep = false, eyes = true; int every = 1, view = V_CLASSIC, fb = FB_OWL; std::wstring fbPath;
    DWORD noMatchT = 0;                            // the owl's slow blink
} A;
static HWND g_wnd; static std::atomic<bool> g_stop{false}; static std::thread g_worker; static std::atomic<bool> g_running{false};
static void poke() { if (g_wnd) PostMessageW(g_wnd, WM_APP + 1, 0, 0); }
static void say(const std::string& s) { { std::lock_guard<std::mutex> lk(A.m); A.status = s; } poke(); }

static std::wstring dataDir() {
    wchar_t* p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) d = std::wstring(p) + L"\\ThunderOwl\\OwlEars";
    if (p) CoTaskMemFree(p);
    if (!d.empty()) {
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        static bool moved = false;                   // (its first name was ThunderEar: its log comes along once)
        if (!moved) { moved = true; std::wstring old = d.substr(0, d.size() - 7) + L"ThunderEar\\history.txt";
                      if (GetFileAttributesW((d + L"\\history.txt").c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileW(old.c_str(), (d + L"\\history.txt").c_str(), TRUE); }
    }
    return d;
}
static void histLoad() {
    FILE* f = _wfopen((dataDir() + L"\\history.txt").c_str(), L"rb"); if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        std::vector<std::string> p; size_t a = 0; for (size_t b; (b = l.find('\t', a)) != std::string::npos; a = b + 1) p.push_back(l.substr(a, b - a)); p.push_back(l.substr(a));
        if (p.size() >= 3) A.hist.push_back({p[0], p[1], p[2], p.size() > 3 ? p[3] : ""});
    }
    fclose(f);
    if (A.hist.size() > 200) A.hist.erase(A.hist.begin(), A.hist.end() - 200);
}
static void histAdd(const shz::Track& t) {
    if (!A.hist.empty() && A.hist.back().artist == t.artist && A.hist.back().title == t.title) return;   // (KEEP LISTENING: the same song again)
    time_t now = time(nullptr); char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H:%M", localtime(&now));
    A.hist.push_back({b, t.artist, t.title, t.url});
    FILE* f = _wfopen((dataDir() + L"\\history.txt").c_str(), L"ab");
    if (f) { fprintf(f, "%s\t%s\t%s\t%s\n", b, t.artist.c_str(), t.title.c_str(), t.url.c_str()); fclose(f); }
}
// ---- the settings: a few lines "key=value" (UTF-8)
static void settingsLoad() {
    FILE* f = _wfopen((dataDir() + L"\\settings.ini").c_str(), L"rb"); if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t e = l.find('='); if (e == std::string::npos) continue;
        std::string k = l.substr(0, e), v = l.substr(e + 1); int n = atoi(v.c_str());
        if (k == "keep") A.keep = n != 0; else if (k == "eyes") A.eyes = n != 0;
        else if (k == "every") A.every = std::max(0, std::min(EVERY_N - 1, n)); else if (k == "view") A.view = std::max(0, std::min(VIEW_N - 1, n));
        else if (k == "fallback") A.fb = std::max(0, std::min(FB_N - 1, n)); else if (k == "fallback_path") A.fbPath = W(v);
    }
    fclose(f);
}
static void settingsSave() {
    FILE* f = _wfopen((dataDir() + L"\\settings.ini").c_str(), L"wb"); if (!f) return;
    fprintf(f, "keep=%d\neyes=%d\nevery=%d\nview=%d\nfallback=%d\nfallback_path=%s\n", A.keep ? 1 : 0, A.eyes ? 1 : 0, A.every, A.view, A.fb, U(A.fbPath).c_str());
    fclose(f);
}

// ------------------------------------------------------------------ asking Shazam for one piece
static double secsSince(LARGE_INTEGER t0) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)(t.QuadPart - t0.QuadPart) / f.QuadPart; }
static int ask(const int16_t* x, size_t n, shz::Track& out, std::string& info) {      // 1 found, 0 no match, -1 a failure (info says)
    int peaks = 0; std::vector<uint8_t> s = sig::make(x, n, &peaks);
    if (s.empty()) { info = "too quiet - no peaks"; return 0; }
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    std::string ans, err; int st = 0;
    bool ok = https(L"POST", W(shz::HOST), W(shz::path()), shz::headers(), shz::body(sig::base64(s), (long)(n * 1000 / RATE)), ans, st, err);
    double dt = secsSince(t0);
    char b[160]; snprintf(b, sizeof b, "%.1f s of sound - %d peaks - %.1f kB sent - answer in %.1f s", (double)n / RATE, peaks, s.size() * 4 / 3 / 1024.0, dt);
    info = b;
    if (!ok) { info = "Shazam did not answer: " + err; return -1; }
    if (st == 403 || st == 405) { info = "Shazam refused (HTTP " + std::to_string(st) + ") - it blocks VPNs, proxies and datacenter addresses"; return -1; }
    if (st == 429) { info = "Shazam: too many requests (HTTP 429) - wait a while"; return -1; }
    if (st != 200) { info = "Shazam answered HTTP " + std::to_string(st); return -1; }
    out = shz::parse(ans);
    return out.found ? 1 : 0;
}
static void showFound(const shz::Track& t, const std::string& info) {
    std::vector<uint8_t> px; int w = 0, h = 0;
    std::string img;
    if (!t.cover.empty() && httpsGet(t.cover, img)) {
        int n; unsigned char* p = stbi_load_from_memory((const unsigned char*)img.data(), (int)img.size(), &w, &h, &n, 4);
        if (p) { px.assign(p, p + (size_t)w * h * 4); stbi_image_free(p); }
    }
    std::lock_guard<std::mutex> lk(A.m);
    A.track = t; A.cover = px; A.coverW = w; A.coverH = h;
    std::string st = info;
    if (t.offset >= 0) { char b[96]; int o = (int)t.offset; snprintf(b, sizeof b, "\nheard %d:%02d into the song", o / 60, o % 60); st += b; }
    if (t.offset >= 0 && (std::fabs(t.timeSkew) > 1e-5 || std::fabs(t.freqSkew) > 1e-5)) { char b[96]; snprintf(b, sizeof b, " - speed %+.2f %%, pitch %+.2f %%", t.timeSkew * 100, t.freqSkew * 100); st += b; }
    A.stats = st;
    histAdd(t);
}

// ------------------------------------------------------------------ the owl's ears: what the PC plays, all the time (its eyes keep the beat)
// A kick drum is a jump of the low end's energy over its own recent average: the sound (16 kHz mono) low-passed at
// ~150 Hz, its energy per ~20 ms block compared with a slow average of the same; 1.6 times over it (and 0.28 s since
// the last one) is a beat.
static std::atomic<unsigned long long> g_lastBeat{0}; static std::atomic<float> g_pcLevel{0.f};
static struct BeatState { float lp = 0, e = 0, avg = 0; int n = 0; unsigned long long last = 0; } g_bs;
static void eyesCb(ma_device*, void*, const void* in, ma_uint32 n) {
    const int16_t* s = (const int16_t*)in; float pk = 0;
    const float k = 1.f - std::exp(-2.f * 3.14159f * 150.f / RATE);
    for (ma_uint32 i = 0; i < n; i++) {
        float x = s[i] / 32768.f; pk = std::max(pk, std::fabs(x));
        g_bs.lp += (x - g_bs.lp) * k; g_bs.e += g_bs.lp * g_bs.lp; g_bs.n++;
        if (g_bs.n >= 320) {                                          // a 20 ms block
            float e = g_bs.e / g_bs.n; g_bs.e = 0; g_bs.n = 0;
            unsigned long long now = GetTickCount64();
            if (e > g_bs.avg * 1.6f && e > 1e-6f && now - g_bs.last > 280) { g_bs.last = now; g_lastBeat = now; }
            g_bs.avg += (e - g_bs.avg) * 0.04f;
        }
    }
    g_pcLevel = std::max(pk, g_pcLevel.load() * 0.9f);
}
static ma_context g_eyeCtx; static ma_device g_eyeDev; static bool g_eyesOn = false;
static void eyesSet(bool on) {
    if (on == g_eyesOn) return;
    if (on) {
        ma_backend be[] = {ma_backend_wasapi};
        if (ma_context_init(be, 1, nullptr, &g_eyeCtx) != MA_SUCCESS) return;
        ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
        cfg.capture.format = ma_format_s16; cfg.capture.channels = 1; cfg.sampleRate = RATE; cfg.dataCallback = eyesCb;
        if (ma_device_init(&g_eyeCtx, &cfg, &g_eyeDev) != MA_SUCCESS) { ma_context_uninit(&g_eyeCtx); return; }
        ma_device_start(&g_eyeDev); g_eyesOn = true;
    } else { ma_device_uninit(&g_eyeDev); ma_context_uninit(&g_eyeCtx); g_eyesOn = false; g_lastBeat = 0; }
}

// ------------------------------------------------------------------ listening (PC sound / microphone)
struct Cap { std::mutex m; std::vector<int16_t> x; };
static void capCb(ma_device* d, void*, const void* in, ma_uint32 n) {
    Cap* c = (Cap*)d->pUserData; const int16_t* s = (const int16_t*)in;
    float pk = 0; for (ma_uint32 i = 0; i < n; i++) pk = std::max(pk, (float)std::abs((int)s[i]) / 32768.f);
    A.level = std::max(pk, A.level.load() * 0.85f);
    std::lock_guard<std::mutex> lk(c->m); c->x.insert(c->x.end(), s, s + n);
}
static void listenOnce(int source) {
    ma_context ctx; ma_backend be[] = {ma_backend_wasapi};
    if (ma_context_init(be, 1, nullptr, &ctx) != MA_SUCCESS) { say("No Windows audio (WASAPI)."); return; }
    ma_device_config cfg = ma_device_config_init(source == 0 ? ma_device_type_loopback : ma_device_type_capture);
    cfg.capture.format = ma_format_s16; cfg.capture.channels = 1; cfg.sampleRate = RATE;
    cfg.dataCallback = capCb; Cap cap; cfg.pUserData = &cap;
    ma_device dev;
    if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS) { say(source == 0 ? "Cannot listen to the PC's sound." : "No microphone (or Windows privacy settings block it)."); ma_context_uninit(&ctx); return; }
    ma_device_start(&dev);
    { std::lock_guard<std::mutex> lk(A.m); A.busy = 1; A.heard = 0; }
    say(source == 0 ? "Listening to the PC's sound..." : "Listening (microphone)...");
    shz::Track t; std::string info, last; int tries = 0, failed = 0; size_t next = PIECE + RATE / 2;   // (a moment more than a piece: the first try at ~3.6 s)
    DWORD t0 = GetTickCount();
    while (!g_stop && tries < 4) {
        Sleep(50);
        size_t have; { std::lock_guard<std::mutex> lk(cap.m); have = cap.x.size(); }
        { std::lock_guard<std::mutex> lk(A.m); A.heard = (float)have / RATE; }
        poke();
        if (have == 0 && GetTickCount() - t0 > 6000 && source == 0) { say("Nothing is playing on the PC (silence sends nothing)."); t0 = GetTickCount() + 600000; }
        if (have < next) continue;
        std::vector<int16_t> piece; { std::lock_guard<std::mutex> lk(cap.m); piece.assign(cap.x.end() - PIECE, cap.x.end()); }
        double e = 0; for (int16_t v : piece) e += std::abs((int)v); e /= piece.size();
        next = have + PIECE + RATE / 2 - 8000;     // (the next piece: fresh sound, ~3 s on)
        if (e < 60) { say("Too quiet - waiting for sound..."); continue; }
        tries++;
        { std::lock_guard<std::mutex> lk(A.m); A.busy = 2; }
        char b[64]; snprintf(b, sizeof b, "Asking Shazam (try %d of 4)...", tries); say(b);
        int r = ask(piece.data(), piece.size(), t, info); last = info;
        { std::lock_guard<std::mutex> lk(A.m); A.busy = 1; }
        if (r == 1) { ma_device_uninit(&dev); ma_context_uninit(&ctx); showFound(t, info); say("Found."); return; }
        if (r < 0 && ++failed >= 2) break;         // (refused / no network twice: no use going on)
        say(r < 0 ? info : "No match yet - listening on...");
    }
    ma_device_uninit(&dev); ma_context_uninit(&ctx);
    if (g_stop) { say("Stopped."); return; }
    { std::lock_guard<std::mutex> lk(A.m); A.stats = last; A.noMatchT = GetTickCount(); }
    say(failed ? last : "No match. (Speech, a live version, a DJ mix or a very new / rare song can do that.)");
}
// ------------------------------------------------------------------ a file: the loudest pieces from a third into it
static void fromFile(const std::wstring& path) {
    { std::lock_guard<std::mutex> lk(A.m); A.busy = 1; A.heard = 0; }
    say("Reading the file...");
    ma_decoder_config dc = ma_decoder_config_init(ma_format_s16, 1, RATE); ma_decoder dec;
    if (ma_decoder_init_file_w(path.c_str(), &dc, &dec) != MA_SUCCESS) { say("Cannot read that file (MP3, FLAC, WAV, OGG)."); return; }
    ma_uint64 len = 0; ma_decoder_get_length_in_pcm_frames(&dec, &len);
    if (len > (ma_uint64)RATE * 30) ma_decoder_seek_to_pcm_frame(&dec, len / 3);
    std::vector<int16_t> x((size_t)RATE * 20); ma_uint64 got = 0; ma_decoder_read_pcm_frames(&dec, x.data(), x.size(), &got); x.resize((size_t)got);
    ma_decoder_uninit(&dec);
    if (x.size() < PIECE) { say("The file is too short."); return; }
    std::vector<std::pair<double, size_t>> w;               // pieces by loudness
    for (size_t o = 0; o + PIECE <= x.size(); o += RATE * 2) { double e = 0; for (size_t i = o; i < o + PIECE; i++) e += std::abs((int)x[i]); w.push_back({e, o}); }
    std::sort(w.rbegin(), w.rend());
    shz::Track t; std::string info;
    for (size_t k = 0; k < std::min<size_t>(3, w.size()) && !g_stop; k++) {
        { std::lock_guard<std::mutex> lk(A.m); A.busy = 2; }
        char b[64]; snprintf(b, sizeof b, "Asking Shazam (piece %d of 3)...", (int)k + 1); say(b);
        int r = ask(&x[w[k].second], PIECE, t, info);
        if (r == 1) { showFound(t, info); say("Found."); return; }
        if (r < 0) { std::lock_guard<std::mutex> lk(A.m); A.stats = info; say(info); return; }
    }
    { std::lock_guard<std::mutex> lk(A.m); A.stats = info; A.noMatchT = GetTickCount(); }
    say("No match.");
}
static void startWork(int source, const std::wstring& file = L"") {
    if (g_running) return;
    if (g_worker.joinable()) g_worker.join();
    g_stop = false; g_running = true;
    { std::lock_guard<std::mutex> lk(A.m); A.source = source; }
    g_worker = std::thread([source, file]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            if (source == 2) fromFile(file); else listenOnce(source);
            bool keep; int every; { std::lock_guard<std::mutex> lk(A.m); keep = A.keep && source != 2; every = EVERY_S[A.every]; }
            if (!keep || g_stop) break;
            { std::lock_guard<std::mutex> lk(A.m); A.busy = 3; }
            for (int i = every; i > 0 && !g_stop; i--) {
                { std::lock_guard<std::mutex> lk(A.m); if (!A.keep) break; char b[64]; snprintf(b, sizeof b, "KEEP LISTENING: again in %d s", i); A.note = b; }
                poke(); Sleep(1000);
            }
            { std::lock_guard<std::mutex> lk(A.m); A.note.clear(); if (!A.keep) break; }
            if (g_stop) break;
        }
        { std::lock_guard<std::mutex> lk(A.m); A.busy = 0; A.note.clear(); }
        A.level = 0; g_running = false; poke();
        CoUninitialize();
    });
}

// ------------------------------------------------------------------ its own picture (instead of the owl): one, or a folder's in turn
struct Pic { std::vector<uint8_t> px; int w = 0, h = 0; std::wstring path; };
static Pic g_fbPic;
static void fbUpdate() {
    std::wstring want;
    if (A.fb == FB_PICTURE) want = A.fbPath;
    else if (A.fb == FB_FOLDER && !A.fbPath.empty()) {
        static std::vector<std::wstring> pics; static std::wstring seen; static DWORD listT = 0;
        if (seen != A.fbPath || GetTickCount() - listT > 60000) {
            seen = A.fbPath; listT = GetTickCount(); pics.clear();
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((A.fbPath + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) { do { std::wstring n = fd.cFileName; std::wstring e = n.size() > 4 ? n.substr(n.size() - 4) : L""; for (auto& c : e) c = (wchar_t)towlower(c);
                                                  if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (e == L".jpg" || e == L"jpeg" || e == L".png")) pics.push_back(A.fbPath + L"\\" + n); } while (FindNextFileW(h, &fd)); FindClose(h); }
            std::sort(pics.begin(), pics.end());
        }
        if (!pics.empty()) want = pics[(GetTickCount64() / 20000) % pics.size()];
    }
    if (want == g_fbPic.path) return;
    g_fbPic = Pic(); g_fbPic.path = want;
    if (want.empty()) return;
    FILE* f = _wfopen(want.c_str(), L"rb"); if (!f) return;
    std::vector<uint8_t> b; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (40 << 20)) { b.resize((size_t)n); if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); }
    fclose(f);
    int w, h, c; unsigned char* p = b.empty() ? nullptr : stbi_load_from_memory(b.data(), (int)b.size(), &w, &h, &c, 4);
    if (!p) return;
    int st = std::max(1, std::max(w, h) / 900);                       // (a big photo: every n-th pixel - the window is ~800 px)
    g_fbPic.w = w / st; g_fbPic.h = h / st; g_fbPic.px.resize((size_t)g_fbPic.w * g_fbPic.h * 4);
    for (int y = 0; y < g_fbPic.h; y++) for (int x = 0; x < g_fbPic.w; x++) memcpy(&g_fbPic.px[((size_t)y * g_fbPic.w + x) * 4], p + ((size_t)y * st * w + x * st) * 4, 4);
    stbi_image_free(p);
}

// ------------------------------------------------------------------ the window (drawn by hand)
static float g_k = 1.f;                                        // DPI scale
static int S(float v) { return (int)std::lround(v * g_k); }
static const int WIN_W = 800, WIN_H = 650;
struct Btn { RECT r; int id; std::wstring label; bool small = false; };
static std::vector<Btn> g_btns; static int g_hover = -1, g_down = -1;
enum { B_PC = 1, B_MIC, B_FILE, B_STOP, B_KEEP, B_OPEN, B_COPY, B_HIST, B_VIEW, B_EVERY, B_FB, B_PICK, B_FOLDER, B_EYES };
static const COLORREF BG = RGB(22, 24, 27), PANEL = RGB(31, 34, 38), LINE = RGB(52, 56, 62), TXT = RGB(232, 232, 232), DIM = RGB(150, 156, 164), AMBER = RGB(255, 179, 71), RED = RGB(255, 90, 70);
static HFONT font(int px, int weight, const wchar_t* face = L"Segoe UI") { return CreateFontW(-S((float)px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face); }
static void fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void rrect(HDC dc, RECT r, COLORREF f, COLORREF e, int rad, bool hollow = false) {
    HBRUSH b = CreateSolidBrush(f); HPEN p = CreatePen(PS_SOLID, std::max(1, S(1)), e);
    HGDIOBJ ob = SelectObject(dc, hollow ? GetStockObject(NULL_BRUSH) : b), op = SelectObject(dc, p); RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fl = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX) {
    HGDIOBJ o = SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); DrawTextW(dc, s.c_str(), -1, &r, fl); SelectObject(dc, o);
}
static void textShadow(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fl) {   // (over a picture: a soft dark edge)
    for (int k = 0; k < 4; k++) { RECT q = r; OffsetRect(&q, k & 1 ? S(1) : -S(1), k & 2 ? S(1) : -S(1)); text(dc, s, q, f, RGB(0, 0, 0), fl); }
    text(dc, s, r, f, c, fl);
}
static void show(HDC dc, const owl::Pix& p, int x, int y) {
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = p.W; bi.bmiHeader.biHeight = -p.H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(dc, x, y, p.W, p.H, 0, 0, 0, p.H, p.px.data(), &bi, DIB_RGB_COLORS);
}
static bool g_owlShown = false;                                // (the owl is on show: the window redraws ~30 times a second)
static void paint(HWND h, HDC wdc) {
    RECT cr; GetClientRect(h, &cr);
    HDC dc = CreateCompatibleDC(wdc); HBITMAP bm = CreateCompatibleBitmap(wdc, cr.right, cr.bottom); HGDIOBJ obm = SelectObject(dc, bm);
    fill(dc, cr, BG);
    std::lock_guard<std::mutex> lk(A.m);
    HFONT fH = font(13, FW_BOLD), fSmall = font(12, FW_NORMAL), fTitle = font(26, FW_BOLD), fArtist = font(19, FW_SEMIBOLD), fBody = font(14, FW_NORMAL), fBtn = font(12, FW_BOLD), fMono = font(12, FW_NORMAL, L"Consolas");
    int pad = S(20);
    text(dc, L"OWL EARS", {pad, S(14), S(400), S(34)}, fH, AMBER, DT_LEFT | DT_SINGLELINE);
    text(dc, L"what song is this?  -  Shazam's recognition, unofficial", {pad + S(80), S(15), cr.right - pad, S(34)}, fSmall, DIM, DT_LEFT | DT_SINGLELINE);
    // ---- the picture area: the cover, the owl, or its own picture
    RECT D = {pad, S(46), cr.right - pad, S(326)};
    int dw = D.right - D.left, dh = D.bottom - D.top;
    bool listening = A.busy == 1 || A.busy == 2;
    bool haveCover = A.track.found && !A.cover.empty();
    const uint8_t* pic = nullptr; int pw = 0, ph = 0;
    if (haveCover && !listening) { pic = A.cover.data(); pw = A.coverW; ph = A.coverH; }
    else if (!A.track.found && !listening && A.fb != FB_OWL && !g_fbPic.px.empty()) { pic = g_fbPic.px.data(); pw = g_fbPic.w; ph = g_fbPic.h; }
    g_owlShown = pic == nullptr;
    static owl::Pix P; P.resize(dw, dh); P.fill(0xFF101215u);
    // the owl's life: the eyes with the beat of what plays (never quite out; without its ears on the PC sound, a slow
    // breath), the tufts and the pupils with the sound while it listens, a slow blink after NO MATCH, now and then a blink
    float tnow = GetTickCount64() / 1000.f;
    unsigned long long lb = g_lastBeat;
    float eye = (A.eyes && lb) ? 0.22f + 0.78f * std::exp(-(GetTickCount64() - lb) / 250.f) : 0.30f + 0.12f * std::sin(tnow * 1.3f);
    float lv = std::min(1.f, A.level.load() * 3.f);
    float sb = (GetTickCount() - A.noMatchT) / 1000.f, blink = A.noMatchT && sb < 1.4f ? std::sin(std::min(1.f, sb / 1.4f) * 3.14159f) : 0.f;
    float ib = std::fmod(tnow, 7.3f); if (ib < 0.18f) blink = std::max(blink, std::sin(ib / 0.18f * 3.14159f));
    auto drawOwl = [&](float cx, float cy, float s) { owl::draw(P, cx, cy, s, eye, listening ? lv : 0.f, listening ? 0.55f + 0.45f * lv : 0.12f, blink, tnow); };
    int side = dh - S(12);
    if (A.view == V_PICTURE) {
        if (pic) {
            static std::vector<uint8_t> tn; static const uint8_t* tnFor = nullptr; static int tnW = 0;
            if (tnFor != pic || tnW != pw) { tn = owl::tiny(pic, pw, ph); tnFor = pic; tnW = pw; }
            owl::blit(P, tn.data(), 8, 8, 0, 0, (float)dw, (float)dh, 0.34f, true);          // the picture's soft glow, filling the area behind it
            float x0 = (dw - side) * 0.5f, y0 = (float)S(6);
            for (int k = 8; k > 0; k--) for (int y = (int)y0 - k + S(6); y < (int)(y0 + side) + k + S(6); y++) for (int x = (int)x0 - k + S(4); x < (int)(x0 + side) + k + S(4); x += 1)
                if (y == (int)y0 - k + S(6) || y == (int)(y0 + side) + k + S(6) - 1 || x == (int)x0 - k + S(4) || x == (int)(x0 + side) + k + S(4) - 1) owl::mixPx(P, x, y, 0, 0, 0, 0.07f);   // (its shadow)
            owl::blit(P, pic, pw, ph, x0, y0, x0 + side, y0 + side, 1.f, true);
        } else drawOwl(dw * 0.5f, dh * 0.47f, (float)side);
        for (int y = dh - S(70); y < dh; y++) { float a = 0.75f * (y - (dh - S(70))) / (float)S(70); for (int x = 0; x < dw; x++) owl::mixPx(P, x, y, 0, 0, 0, a); }   // (under the words)
    } else {
        if (pic) owl::blit(P, pic, pw, ph, (float)S(6), (float)S(6), (float)(S(6) + side), (float)(S(6) + side), 1.f, true);
        else drawOwl(S(6) + side * 0.5f, dh * 0.47f, side * 0.95f);
    }
    show(dc, P, D.left, D.top);
    rrect(dc, D, PANEL, LINE, S(8), true);
    // ---- the words
    std::string st = A.status; if (listening) { char b[32]; snprintf(b, sizeof b, "   %.1f s", A.heard); st += b; }
    COLORREF stc = A.status.rfind("Shazam refused", 0) == 0 || A.status.rfind("Shazam did not", 0) == 0 || A.status.rfind("No ", 0) == 0 || A.status.rfind("Cannot", 0) == 0 ? RED : AMBER;
    if (A.view == V_PICTURE) {
        textShadow(dc, W(st), {D.left + S(12), D.top + S(8), D.right - S(12), D.top + S(30)}, fBody, stc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (A.track.found) {
            text(dc, W(A.track.title), {D.left + S(14), D.bottom - S(62), D.right - S(14), D.bottom - S(32)}, fTitle, TXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            std::string a = A.track.artist; if (!A.track.album.empty()) a += "  -  " + A.track.album; if (!A.track.released.empty()) a += "  -  " + A.track.released;
            text(dc, W(a), {D.left + S(14), D.bottom - S(30), D.right - S(14), D.bottom - S(8)}, fBody, RGB(205, 208, 212), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else text(dc, L"Play something, then PC SOUND or MICROPHONE - or a FILE.", {D.left + S(14), D.bottom - S(30), D.right - S(14), D.bottom - S(8)}, fBody, DIM, DT_LEFT | DT_SINGLELINE);
    } else {
        int x0 = D.left + S(6) + side + S(22), x1 = D.right - S(14), y = D.top + S(10);
        text(dc, W(st), {x0, y, x1, y + S(40)}, fBody, stc);
        y += S(44);
        if (A.track.found) {
            text(dc, W(A.track.title), {x0, y, x1, y + S(36)}, fTitle, TXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); y += S(38);
            text(dc, W(A.track.artist), {x0, y, x1, y + S(28)}, fArtist, TXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); y += S(30);
            std::string a = A.track.album; if (!A.track.released.empty()) a += (a.empty() ? "" : "  -  ") + A.track.released;
            if (!A.track.genre.empty()) a += (a.empty() ? "" : "  -  ") + A.track.genre; if (!A.track.label.empty()) a += (a.empty() ? "" : "  -  ") + A.track.label;
            text(dc, W(a), {x0, y, x1, y + S(40)}, fBody, DIM); y += S(44);
        } else { text(dc, L"Play something, then press PC SOUND (what this PC plays) or MICROPHONE (what the room plays) - or choose a FILE.", {x0, y, x1, y + S(60)}, fBody, DIM); y += S(64); }
        text(dc, W(A.stats), {x0, y, x1, y + S(36)}, fSmall, DIM); y += S(40);
        if (!A.note.empty()) text(dc, W(A.note), {x0, y, x1, y + S(18)}, fSmall, AMBER, DT_LEFT | DT_SINGLELINE);
    }
    if (A.view == V_PICTURE && !A.note.empty()) textShadow(dc, W(A.note), {D.left + S(12), D.top + S(30), D.right - S(12), D.top + S(48)}, fSmall, AMBER, DT_LEFT | DT_SINGLELINE);
    // ---- the level
    RECT lr = {pad, S(334), cr.right - pad, S(339)}; fill(dc, lr, PANEL);
    RECT lvr = lr; lvr.right = lr.left + (int)((lr.right - lr.left) * std::min(1.f, A.level.load() * 1.4f)); fill(dc, lvr, A.level > 0.7f ? RED : AMBER);
    // ---- the keys
    g_btns.clear();
    bool run = A.busy != 0;
    int bx0 = pad, bx1 = cr.right - pad, gap = S(8), by = S(350), bh = S(34);
    auto row = [&](std::vector<std::pair<int, std::wstring>> ks, int y, int hgt, bool small) {
        int n = (int)ks.size(), w = (bx1 - bx0 - gap * (n - 1)) / n;
        for (int i = 0; i < n; i++) g_btns.push_back({{bx0 + i * (w + gap), y, i == n - 1 ? bx1 : bx0 + i * (w + gap) + w, y + hgt}, ks[i].first, ks[i].second, small});
    };
    row({{run && A.source == 0 ? B_STOP : B_PC, run && A.source == 0 ? L"STOP" : L"PC SOUND"}, {run && A.source == 1 ? B_STOP : B_MIC, run && A.source == 1 ? L"STOP" : L"MICROPHONE"},
         {run && A.source == 2 ? B_STOP : B_FILE, run && A.source == 2 ? L"STOP" : L"FILE..."}, {B_VIEW, std::wstring(L"VIEW: ") + VIEW_NAMES[A.view]}}, by, bh, false);
    by += bh + S(8);
    row({{B_KEEP, L"KEEP LISTENING"}, {B_EVERY, std::wstring(L"EVERY ") + EVERY_NAMES[A.every]}, {B_OPEN, L"OPEN IN SHAZAM"}, {B_COPY, L"COPY"}}, by, bh, false);
    by += bh + S(8);
    row({{B_FB, std::wstring(L"NOTHING FOUND: ") + FB_NAMES[A.fb]}, {B_PICK, L"CHOOSE PICTURE..."}, {B_FOLDER, L"CHOOSE FOLDER..."}, {B_EYES, L"EYES ON THE BEAT"}}, by, S(28), true);
    for (size_t i = 0; i < g_btns.size(); i++) {
        const Btn& b = g_btns[i];
        bool dis = (b.id == B_OPEN || b.id == B_COPY) && !A.track.found;
        bool lit = (b.id == B_KEEP && A.keep) || b.id == B_STOP || (b.id == B_EYES && A.eyes);
        COLORREF f = (int)i == g_down ? RGB(20, 22, 25) : (int)i == g_hover && !dis ? RGB(48, 52, 58) : RGB(40, 43, 48);
        rrect(dc, b.r, f, lit ? AMBER : LINE, S(6));
        if (b.id == B_KEEP || b.id == B_STOP || b.id == B_EYES) {          // a lamp
            int lx = b.r.left + S(14), ly = (b.r.top + b.r.bottom) / 2;
            HBRUSH lb_ = CreateSolidBrush(lit ? (b.id == B_STOP ? (GetTickCount() % 1000 < 600 ? RED : RGB(90, 30, 25)) : AMBER) : RGB(60, 60, 60)); HGDIOBJ ob = SelectObject(dc, lb_); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, lx - S(4), ly - S(4), lx + S(4), ly + S(4)); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(lb_);
        }
        text(dc, b.label, b.r, b.small ? fSmall : fBtn, dis ? RGB(90, 94, 100) : TXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // ---- what was found before
    int hy = by + S(28) + S(14);
    text(dc, L"FOUND BEFORE", {pad, hy, S(400), hy + S(18)}, fH, DIM, DT_LEFT | DT_SINGLELINE);
    g_btns.push_back({{pad, hy, S(400), hy + S(18)}, B_HIST, L"", true});
    hy += S(22);
    RECT hp = {pad, hy, cr.right - pad, cr.bottom - S(32)}; rrect(dc, hp, PANEL, LINE, S(6));
    int lh = S(19), rows = (hp.bottom - hp.top - S(10)) / lh;
    for (int k = 0; k < rows && k < (int)A.hist.size(); k++) {
        const Hist& e = A.hist[A.hist.size() - 1 - k];
        int yy = hp.top + S(5) + k * lh;
        text(dc, W(e.when), {hp.left + S(10), yy, hp.left + S(150), yy + lh}, fMono, DIM, DT_LEFT | DT_SINGLELINE);
        text(dc, W(e.artist + "  -  " + e.title), {hp.left + S(150), yy, hp.right - S(10), yy + lh}, fBody, k == 0 ? TXT : DIM, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (A.hist.empty()) text(dc, L"(nothing yet - the songs found are kept in %APPDATA%\\ThunderOwl\\OwlEars\\history.txt)", {hp.left + S(10), hp.top + S(8), hp.right, hp.top + S(30)}, fSmall, DIM, DT_LEFT | DT_SINGLELINE);
    text(dc, L"Only the signature (the song's strongest peaks, about 1 kB) leaves the PC - never the sound. Unofficial use of Shazam's service: it may stop working any day.",
         {pad, cr.bottom - S(26), cr.right - pad, cr.bottom - S(6)}, fSmall, RGB(110, 116, 124), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (HFONT f : {fH, fSmall, fTitle, fArtist, fBody, fBtn, fMono}) DeleteObject(f);
    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm); DeleteObject(bm); DeleteDC(dc);
}
static int hitBtn(int x, int y) { for (size_t i = 0; i < g_btns.size(); i++) { const RECT& r = g_btns[i].r; if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i; } return -1; }
static std::wstring pickDialog(HWND h, bool folder) {
    std::wstring out; IFileDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)) && dlg) {
        DWORD o = 0; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        dlg->SetTitle(folder ? L"Owl Ears - a folder of pictures (shown in turn)" : L"Owl Ears - a picture for when nothing is found");
        if (!folder) { COMDLG_FILTERSPEC fs[2] = {{L"Pictures (JPG, PNG)", L"*.jpg;*.jpeg;*.png"}, {L"All files", L"*.*"}}; dlg->SetFileTypes(2, fs); }
        if (SUCCEEDED(dlg->Show(h))) { IShellItem* it = nullptr; if (SUCCEEDED(dlg->GetResult(&it)) && it) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); } it->Release(); } }
        dlg->Release();
    }
    return out;
}
static void clicked(HWND h, int id) {
    switch (id) {
        case B_PC: startWork(0); break;
        case B_MIC: startWork(1); break;
        case B_FILE: {
            wchar_t f[MAX_PATH * 4] = {}; OPENFILENAMEW of = {}; of.lStructSize = sizeof of; of.hwndOwner = h; of.lpstrFile = f; of.nMaxFile = MAX_PATH * 4;
            of.lpstrFilter = L"Music (MP3, FLAC, WAV, OGG)\0*.mp3;*.flac;*.wav;*.ogg;*.oga\0All files\0*.*\0"; of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
            if (GetOpenFileNameW(&of)) startWork(2, f);
        } break;
        case B_STOP: g_stop = true; break;
        case B_KEEP: {                       // on while idle: listen now (the last of PC SOUND / MICROPHONE), then every EVERY
            bool on; int src; { std::lock_guard<std::mutex> lk(A.m); A.keep = !A.keep; on = A.keep; src = A.source == 1 ? 1 : 0; }
            settingsSave(); if (on && !g_running) startWork(src);
        } break;
        case B_EVERY: { std::lock_guard<std::mutex> lk(A.m); A.every = (A.every + 1) % EVERY_N; } settingsSave(); break;
        case B_VIEW: { std::lock_guard<std::mutex> lk(A.m); A.view = (A.view + 1) % VIEW_N; } settingsSave(); break;
        case B_EYES: { bool on; { std::lock_guard<std::mutex> lk(A.m); A.eyes = !A.eyes; on = A.eyes; } eyesSet(on); settingsSave(); } break;
        case B_FB: {
            bool needPick = false;
            { std::lock_guard<std::mutex> lk(A.m); A.fb = (A.fb + 1) % FB_N; needPick = A.fb != FB_OWL && A.fbPath.empty(); }
            if (needPick) { std::wstring p = pickDialog(h, A.fb == FB_FOLDER); std::lock_guard<std::mutex> lk(A.m); if (p.empty()) A.fb = FB_OWL; else A.fbPath = p; }
            settingsSave(); fbUpdate();
        } break;
        case B_PICK: case B_FOLDER: {
            std::wstring p = pickDialog(h, id == B_FOLDER);
            if (!p.empty()) { std::lock_guard<std::mutex> lk(A.m); A.fbPath = p; A.fb = id == B_FOLDER ? FB_FOLDER : FB_PICTURE; }
            settingsSave(); fbUpdate();
        } break;
        case B_OPEN: { std::string u; { std::lock_guard<std::mutex> lk(A.m); u = A.track.url; } if (!u.empty()) ShellExecuteW(h, L"open", W(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL); } break;
        case B_COPY: {
            std::wstring s; { std::lock_guard<std::mutex> lk(A.m); if (!A.track.found) break; s = W(A.track.artist + " - " + A.track.title); }
            if (OpenClipboard(h)) { EmptyClipboard(); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * 2); memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * 2); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); say("Copied: " + U(s)); }
        } break;
        case B_HIST: { std::wstring f = dataDir() + L"\\history.txt"; ShellExecuteW(h, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL); } break;
    }
    InvalidateRect(h, nullptr, FALSE);
}
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(h, dc); EndPaint(h, &ps); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: {
            bool anim; { std::lock_guard<std::mutex> lk(A.m); anim = A.busy != 0 || g_owlShown; fbUpdate(); }
            if (!anim) A.level = A.level * 0.8f;
            if (anim && !IsIconic(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_APP + 1: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE: { int k = hitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); if (k != g_hover) { g_hover = k; InvalidateRect(h, nullptr, FALSE); }
                             TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, h, 0}; TrackMouseEvent(&te); SetCursor(LoadCursor(nullptr, k >= 0 ? IDC_HAND : IDC_ARROW)); return 0; }
        case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            g_down = hitBtn(x, y); SetCapture(h);
            RECT D = {S(20), S(46), S(WIN_W - 20), S(326)};             // (a click on the picture: the other view)
            if (g_down < 0 && x >= D.left && x < D.right && y >= D.top && y < D.bottom) { { std::lock_guard<std::mutex> lk(A.m); A.view = (A.view + 1) % VIEW_N; } settingsSave(); }
            InvalidateRect(h, nullptr, FALSE); return 0;
        }
        case WM_LBUTTONUP: { int k = hitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); ReleaseCapture(); int d = g_down; g_down = -1;
                             if (k >= 0 && k == d) clicked(h, g_btns[k].id); InvalidateRect(h, nullptr, FALSE); return 0; }
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
        case WM_DPICHANGED: { g_k = HIWORD(w) / 96.f; RECT* r = (RECT*)l; SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
        case WM_DESTROY: g_stop = true; if (g_worker.joinable()) g_worker.join(); eyesSet(false); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    typedef BOOL(WINAPI * Dpi)(HANDLE);
    if (auto f = (Dpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)-4); else SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    histLoad(); settingsLoad(); fbUpdate();
    WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"OwlEars"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hbrBackground = nullptr; RegisterClassW(&wc);
    HDC sdc = GetDC(nullptr); g_k = GetDeviceCaps(sdc, LOGPIXELSX) / 96.f; ReleaseDC(nullptr, sdc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(WIN_W), S(WIN_H)}; AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(L"OwlEars", APP_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    typedef UINT(WINAPI * GDW)(HWND);
    if (auto f = (GDW)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) {
        float k = f(g_wnd) / 96.f;
        if (std::fabs(k - g_k) > 0.01f) { g_k = k; RECT rr = {0, 0, S(WIN_W), S(WIN_H)}; AdjustWindowRect(&rr, style, FALSE); SetWindowPos(g_wnd, nullptr, 0, 0, rr.right - rr.left, rr.bottom - rr.top, SWP_NOMOVE | SWP_NOZORDER); }
    }
    BOOL dark = TRUE; typedef HRESULT(WINAPI * DSWA)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dm = LoadLibraryW(L"dwmapi.dll")) if (auto f = (DSWA)GetProcAddress(dm, "DwmSetWindowAttribute")) f(g_wnd, 20, &dark, sizeof dark);   // (a dark title bar)
    if (A.eyes) eyesSet(true);
    ShowWindow(g_wnd, show); UpdateWindow(g_wnd);
    if (A.keep) startWork(0);                // KEEP LISTENING was left on: it listens to the PC from the start
    SetTimer(g_wnd, 1, 33, nullptr);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
