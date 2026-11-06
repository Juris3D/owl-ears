// Owl Ears - a song recogniser for Windows, a test: does Shazam's (unofficial) recognition, as shazamlite does it,
// work well enough to be worth building into TA-8090?
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

#define APP_TITLE L"Owl Ears - song recognition test (v2)"
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
static struct App {
    std::mutex m;
    std::string status = "Choose what to listen to.", note;
    int busy = 0;                                  // 0 idle, 1 listening, 2 asking, 3 waiting (KEEP LISTENING)
    int source = 0;                                // 0 PC sound, 1 microphone, 2 file
    std::atomic<float> level{0.f};
    float heard = 0.f;                             // seconds heard this time
    shz::Track track; std::vector<uint8_t> cover; int coverW = 0, coverH = 0;
    std::string stats;
    std::vector<Hist> hist;
    bool keep = false;
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
        if (p) { px.assign(p, p + (size_t)w * h * 4); stbi_image_free(p); for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]); }   // (RGBA -> BGRA)
    }
    std::lock_guard<std::mutex> lk(A.m);
    A.track = t; A.cover = px; A.coverW = w; A.coverH = h;
    std::string st = info;
    if (t.offset >= 0) { char b[96]; int o = (int)t.offset; snprintf(b, sizeof b, "\nheard %d:%02d into the song", o / 60, o % 60); st += b; }
    if (t.offset >= 0 && (std::fabs(t.timeSkew) > 1e-5 || std::fabs(t.freqSkew) > 1e-5)) { char b[96]; snprintf(b, sizeof b, " - speed %+.2f %%, pitch %+.2f %%", t.timeSkew * 100, t.freqSkew * 100); st += b; }
    A.stats = st;
    histAdd(t);
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
    { std::lock_guard<std::mutex> lk(A.m); A.stats = last; }
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
    { std::lock_guard<std::mutex> lk(A.m); A.stats = info; }
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
            bool keep; { std::lock_guard<std::mutex> lk(A.m); keep = A.keep && source != 2; }
            if (!keep || g_stop) break;
            { std::lock_guard<std::mutex> lk(A.m); A.busy = 3; }
            for (int i = 20; i > 0 && !g_stop; i--) { char b[64]; snprintf(b, sizeof b, "KEEP LISTENING: again in %d s", i); { std::lock_guard<std::mutex> lk(A.m); A.note = b; } poke(); Sleep(1000); }
            { std::lock_guard<std::mutex> lk(A.m); A.note.clear(); }
            if (g_stop) break;
        }
        { std::lock_guard<std::mutex> lk(A.m); A.busy = 0; A.note.clear(); }
        A.level = 0; g_running = false; poke();
        CoUninitialize();
    });
}

// ------------------------------------------------------------------ the window (drawn by hand: a dark hi-fi panel)
static float g_k = 1.f;                                        // DPI scale
static int S(float v) { return (int)std::lround(v * g_k); }
struct Btn { RECT r; int id; const wchar_t* label; };
static std::vector<Btn> g_btns; static int g_hover = -1, g_down = -1;
enum { B_PC = 1, B_MIC, B_FILE, B_STOP, B_KEEP, B_OPEN, B_COPY, B_HIST };
static const COLORREF BG = RGB(22, 24, 27), PANEL = RGB(31, 34, 38), LINE = RGB(52, 56, 62), TXT = RGB(232, 232, 232), DIM = RGB(150, 156, 164), AMBER = RGB(255, 179, 71), RED = RGB(255, 90, 70);
static HFONT font(int px, int weight, const wchar_t* face = L"Segoe UI") { return CreateFontW(-S((float)px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face); }
static void fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void rrect(HDC dc, RECT r, COLORREF f, COLORREF e, int rad) {
    HBRUSH b = CreateSolidBrush(f); HPEN p = CreatePen(PS_SOLID, std::max(1, S(1)), e);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p); RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fl = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX) {
    HGDIOBJ o = SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); DrawTextW(dc, s.c_str(), -1, &r, fl); SelectObject(dc, o);
}
static void paint(HWND h, HDC wdc) {
    RECT cr; GetClientRect(h, &cr);
    HDC dc = CreateCompatibleDC(wdc); HBITMAP bm = CreateCompatibleBitmap(wdc, cr.right, cr.bottom); HGDIOBJ obm = SelectObject(dc, bm);
    fill(dc, cr, BG);
    std::lock_guard<std::mutex> lk(A.m);
    HFONT fH = font(13, FW_BOLD), fSmall = font(12, FW_NORMAL), fTitle = font(26, FW_BOLD), fArtist = font(19, FW_SEMIBOLD), fBody = font(14, FW_NORMAL), fBtn = font(12, FW_BOLD), fMono = font(12, FW_NORMAL, L"Consolas");
    int pad = S(20);
    text(dc, L"OWL EARS", {pad, S(14), S(400), S(34)}, fH, AMBER, DT_LEFT | DT_SINGLELINE);
    text(dc, L"song recognition - a test of Shazam's (unofficial) service", {pad + S(80), S(15), cr.right - pad, S(34)}, fSmall, DIM, DT_LEFT | DT_SINGLELINE);
    // ---- the cover (or the ear, its rings breathing with the sound while listening)
    RECT cv = {pad, S(46), pad + S(280), S(46) + S(280)};
    rrect(dc, cv, PANEL, LINE, S(8));
    if (!A.cover.empty() && A.track.found && A.busy != 1 && A.busy != 2) {         // (listening again: the ear)
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = A.coverW; bi.bmiHeader.biHeight = -A.coverH; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, cv.left + S(6), cv.top + S(6), S(268), S(268), 0, 0, A.coverW, A.coverH, A.cover.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        int cx = (cv.left + cv.right) / 2, cy = (cv.top + cv.bottom) / 2; float lv = A.level, t = GetTickCount() / 1000.f;
        for (int k = 0; k < 6; k++) {
            float rr = S(26 + 20.f * k + (A.busy ? lv * 14.f * (1.f - k / 7.f) : 0.f));
            float a = A.busy ? 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 5.f - k * 0.9f)) * std::min(1.f, lv * 3.f + 0.2f) : 0.35f;
            COLORREF c = RGB((int)(31 + (255 - 31) * a * (A.busy ? 1.f : 0.45f)), (int)(34 + (179 - 34) * a * (A.busy ? 1.f : 0.45f)), (int)(38 + (71 - 38) * a * (A.busy ? 1.f : 0.45f)));
            HPEN p = CreatePen(PS_SOLID, S(3), c); HGDIOBJ op = SelectObject(dc, p); HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Arc(dc, (int)(cx - rr), (int)(cy - rr), (int)(cx + rr), (int)(cy + rr), cx + (int)rr, cy - (int)(rr * 0.9f), cx + (int)rr, cy + (int)(rr * 0.9f));
            SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
        }
        HBRUSH b = CreateSolidBrush(A.busy ? AMBER : DIM); HGDIOBJ ob = SelectObject(dc, b); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - S(10), cy - S(10), cx + S(10), cy + S(10)); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
    }
    // ---- the song
    int x0 = cv.right + S(24), x1 = cr.right - pad, y = S(46);
    COLORREF stc = A.status.rfind("Shazam refused", 0) == 0 || A.status.rfind("Shazam did not", 0) == 0 || A.status.rfind("No ", 0) == 0 || A.status.rfind("Cannot", 0) == 0 ? RED : AMBER;
    std::string st = A.status; if (A.busy == 1 || A.busy == 2) { char b[32]; snprintf(b, sizeof b, "   %.1f s", A.heard); st += b; }
    text(dc, W(st), {x0, y, x1, y + S(40)}, fBody, stc);
    y += S(44);
    if (A.track.found) {
        text(dc, W(A.track.title), {x0, y, x1, y + S(36)}, fTitle, TXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); y += S(38);
        text(dc, W(A.track.artist), {x0, y, x1, y + S(28)}, fArtist, TXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); y += S(30);
        std::string a = A.track.album; if (!A.track.released.empty()) a += (a.empty() ? "" : "  -  ") + A.track.released;
        if (!A.track.genre.empty()) a += (a.empty() ? "" : "  -  ") + A.track.genre; if (!A.track.label.empty()) a += (a.empty() ? "" : "  -  ") + A.track.label;
        text(dc, W(a), {x0, y, x1, y + S(40)}, fBody, DIM); y += S(42);
    } else { text(dc, L"Play something, then press PC SOUND (what this PC plays) or MICROPHONE (what the room plays) - or choose a FILE.", {x0, y, x1, y + S(60)}, fBody, DIM); y += S(62); }
    text(dc, W(A.stats), {x0, y, x1, y + S(36)}, fSmall, DIM); y += S(40);
    if (!A.note.empty()) text(dc, W(A.note), {x0, y - S(2), x1, y + S(16)}, fSmall, AMBER, DT_LEFT | DT_SINGLELINE);
    // ---- the keys
    g_btns.clear();
    int by = S(260), bw = (x1 - x0 - S(16)) / 3, bh = S(34);
    bool run = A.busy != 0;
    g_btns.push_back({{x0, by, x0 + bw, by + bh}, run && A.source == 0 ? B_STOP : B_PC, run && A.source == 0 ? L"STOP" : L"PC SOUND"});
    g_btns.push_back({{x0 + bw + S(8), by, x0 + 2 * bw + S(8), by + bh}, run && A.source == 1 ? B_STOP : B_MIC, run && A.source == 1 ? L"STOP" : L"MICROPHONE"});
    g_btns.push_back({{x0 + 2 * (bw + S(8)), by, x1, by + bh}, run && A.source == 2 ? B_STOP : B_FILE, run && A.source == 2 ? L"STOP" : L"FILE..."});
    by += bh + S(10);
    g_btns.push_back({{x0, by, x0 + bw, by + bh}, B_KEEP, L"KEEP LISTENING"});
    g_btns.push_back({{x0 + bw + S(8), by, x0 + 2 * bw + S(8), by + bh}, B_OPEN, L"OPEN IN SHAZAM"});
    g_btns.push_back({{x0 + 2 * (bw + S(8)), by, x1, by + bh}, B_COPY, L"COPY"});
    for (size_t i = 0; i < g_btns.size(); i++) {
        const Btn& b = g_btns[i];
        bool dis = (b.id == B_OPEN || b.id == B_COPY) && !A.track.found;
        bool lit = (b.id == B_KEEP && A.keep) || b.id == B_STOP;
        COLORREF f = (int)i == g_down ? RGB(20, 22, 25) : (int)i == g_hover && !dis ? RGB(48, 52, 58) : RGB(40, 43, 48);
        rrect(dc, b.r, f, lit ? AMBER : LINE, S(6));
        if (b.id == B_KEEP || b.id == B_STOP) {          // a lamp
            int lx = b.r.left + S(14), ly = (b.r.top + b.r.bottom) / 2;
            HBRUSH lb = CreateSolidBrush(lit ? (b.id == B_STOP ? (GetTickCount() % 1000 < 600 ? RED : RGB(90, 30, 25)) : AMBER) : RGB(60, 60, 60)); HGDIOBJ ob = SelectObject(dc, lb); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, lx - S(4), ly - S(4), lx + S(4), ly + S(4)); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(lb);
        }
        text(dc, b.label, b.r, fBtn, dis ? RGB(90, 94, 100) : TXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // ---- the level
    by += bh + S(12);
    RECT lr = {x0, by, x1, by + S(6)}; fill(dc, lr, PANEL);
    RECT lv = lr; lv.right = lr.left + (int)((lr.right - lr.left) * std::min(1.f, A.level.load() * 1.4f)); fill(dc, lv, A.level > 0.7f ? RED : AMBER);
    // ---- what was found before
    int hy = S(372);
    text(dc, L"FOUND BEFORE", {pad, hy, S(400), hy + S(18)}, fH, DIM, DT_LEFT | DT_SINGLELINE);
    hy += S(22);
    RECT hp = {pad, hy, cr.right - pad, cr.bottom - S(36)}; rrect(dc, hp, PANEL, LINE, S(6));
    int lh = S(19), rows = (hp.bottom - hp.top - S(12)) / lh;
    for (int k = 0; k < rows && k < (int)A.hist.size(); k++) {
        const Hist& e = A.hist[A.hist.size() - 1 - k];
        int yy = hp.top + S(6) + k * lh;
        text(dc, W(e.when), {hp.left + S(10), yy, hp.left + S(150), yy + lh}, fMono, DIM, DT_LEFT | DT_SINGLELINE);
        text(dc, W(e.artist + "  -  " + e.title), {hp.left + S(150), yy, hp.right - S(10), yy + lh}, fBody, k == 0 ? TXT : DIM, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (A.hist.empty()) text(dc, L"(nothing yet - the songs found are kept in %APPDATA%\\ThunderOwl\\OwlEars\\history.txt)", {hp.left + S(10), hp.top + S(8), hp.right, hp.top + S(30)}, fSmall, DIM, DT_LEFT | DT_SINGLELINE);
    g_btns.push_back({{pad, hy - S(22), S(400), hy}, B_HIST, L""});
    text(dc, L"Only the signature (the song's strongest peaks, a few kB) leaves the PC - never the sound. Unofficial use of Shazam's service: it may stop working any day.",
         {pad, cr.bottom - S(28), cr.right - pad, cr.bottom - S(8)}, fSmall, RGB(110, 116, 124), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (HFONT f : {fH, fSmall, fTitle, fArtist, fBody, fBtn, fMono}) DeleteObject(f);
    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm); DeleteObject(bm); DeleteDC(dc);
}
static int hitBtn(int x, int y) { for (size_t i = 0; i < g_btns.size(); i++) { const RECT& r = g_btns[i].r; if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i; } return -1; }
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
        case B_KEEP: { std::lock_guard<std::mutex> lk(A.m); A.keep = !A.keep; } break;
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
        case WM_TIMER: { bool anim; { std::lock_guard<std::mutex> lk(A.m); anim = A.busy != 0; } if (anim) InvalidateRect(h, nullptr, FALSE); else A.level = A.level * 0.8f; return 0; }
        case WM_APP + 1: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE: { int k = hitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); if (k != g_hover) { g_hover = k; InvalidateRect(h, nullptr, FALSE); }
                             TRACKMOUSEEVENT te = {sizeof te, TME_LEAVE, h, 0}; TrackMouseEvent(&te); SetCursor(LoadCursor(nullptr, k >= 0 ? IDC_HAND : IDC_ARROW)); return 0; }
        case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: g_down = hitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONUP: { int k = hitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); ReleaseCapture(); int d = g_down; g_down = -1;
                             if (k >= 0 && k == d) clicked(h, g_btns[k].id); InvalidateRect(h, nullptr, FALSE); return 0; }
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
        case WM_DPICHANGED: { g_k = HIWORD(w) / 96.f; RECT* r = (RECT*)l; SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
        case WM_DESTROY: g_stop = true; if (g_worker.joinable()) g_worker.join(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    typedef BOOL(WINAPI * Dpi)(HANDLE);
    if (auto f = (Dpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)-4); else SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    histLoad();
    WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"OwlEars"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hbrBackground = nullptr; RegisterClassW(&wc);
    HDC sdc = GetDC(nullptr); g_k = GetDeviceCaps(sdc, LOGPIXELSX) / 96.f; ReleaseDC(nullptr, sdc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(800), S(560)}; AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(L"OwlEars", APP_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    typedef UINT(WINAPI * GDW)(HWND);
    if (auto f = (GDW)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) {
        float k = f(g_wnd) / 96.f;
        if (std::fabs(k - g_k) > 0.01f) { g_k = k; RECT rr = {0, 0, S(800), S(560)}; AdjustWindowRect(&rr, style, FALSE); SetWindowPos(g_wnd, nullptr, 0, 0, rr.right - rr.left, rr.bottom - rr.top, SWP_NOMOVE | SWP_NOZORDER); }
    }
    BOOL dark = TRUE; typedef HRESULT(WINAPI * DSWA)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dm = LoadLibraryW(L"dwmapi.dll")) if (auto f = (DSWA)GetProcAddress(dm, "DwmSetWindowAttribute")) f(g_wnd, 20, &dark, sizeof dark);   // (a dark title bar)
    ShowWindow(g_wnd, show); UpdateWindow(g_wnd);
    SetTimer(g_wnd, 1, 33, nullptr);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
