// Owl Ears - a small JSON reader (a tree: objects, arrays, strings, numbers, true / false / null)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>

namespace js {
struct V {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false; double n = 0; std::string s;
    std::vector<V> a; std::vector<std::pair<std::string, V>> o;
    const V& operator[](const std::string& k) const { static V none; if (t == OBJ) for (auto& p : o) if (p.first == k) return p.second; return none; }
    const V& operator[](size_t i) const { static V none; return t == ARR && i < a.size() ? a[i] : none; }
    size_t size() const { return t == ARR ? a.size() : t == OBJ ? o.size() : 0; }
    std::string str() const { if (t == STR) return s; if (t == NUM) { char b_[32]; snprintf(b_, sizeof b_, "%.15g", n); return b_; } return ""; }
    double num(double d = 0) const { return t == NUM ? n : t == STR ? atof(s.c_str()) : d; }
    bool ok() const { return t != NUL; }
};
struct P {
    const std::string& s; size_t i = 0; int depth = 0;
    explicit P(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    static void put(std::string& o, unsigned c) {
        if (c < 0x80) o += (char)c; else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    std::string str() {
        std::string o; i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++]; if (c != '\\') { o += c; continue; }
            if (i >= s.size()) break; char e = s[i++];
            switch (e) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': case 'b': case 'f': break;
                case 'u': {
                    if (i + 4 > s.size()) break; unsigned c1 = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4;
                    if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') { unsigned c2 = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16); i += 6; c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00); }
                    put(o, c1); break; }
                default: o += e;
            }
        }
        i++; return o;
    }
    V val() {
        V v; ws(); if (i >= s.size() || ++depth > 64) { depth--; return v; }
        char c = s[i];
        if (c == '"') { v.t = V::STR; v.s = str(); }
        else if (c == '{') {
            v.t = V::OBJ; i++;
            for (;;) { ws(); if (i >= s.size()) break; if (s[i] == '}') { i++; break; } if (s[i] == ',') { i++; continue; }
                       if (s[i] != '"') { i++; continue; } std::string k = str(); ws(); if (i < s.size() && s[i] == ':') i++; v.o.push_back({k, val()}); }
        } else if (c == '[') {
            v.t = V::ARR; i++;
            for (;;) { ws(); if (i >= s.size()) break; if (s[i] == ']') { i++; break; } if (s[i] == ',') { i++; continue; } v.a.push_back(val()); }
        } else if (c == 't' || c == 'f') { v.t = V::BOOL; v.b = c == 't'; i += c == 't' ? 4 : 5; }
        else if (c == 'n') i += 4;
        else { v.t = V::NUM; char* e = nullptr; v.n = strtod(s.c_str() + i, &e); i = e ? (size_t)(e - s.c_str()) : i + 1; }
        depth--; return v;
    }
};
static V parse(const std::string& s) { P p(s); return p.val(); }
static std::string esc(const std::string& s) {
    std::string o; for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += (char)c; } else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c; } return o;
}
}
