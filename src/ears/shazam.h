// Owl Ears - talking to Shazam: the request its app sends (as shazamlite / ShazamIO make it) and what comes back.
// UNOFFICIAL: Shazam (Apple) never published this interface; it works because it looks like their app. It can
// change or be blocked any day.
#pragma once
#include "json.h"
#include <string>
#include <vector>
#include <random>
#include <ctime>

namespace shz {
static const char* UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
static const char* HOST = "amp.shazam.com";

struct Track {
    bool found = false;
    std::string title, artist, album, released, label, genre, cover, url, key, isrc;
    double offset = -1;                 // where in the song the heard piece was (s)
    double timeSkew = 0, freqSkew = 0;  // how the heard piece's speed / pitch differs from the record (fractions)
};

static std::string uuid4(bool upper) {
    static std::mt19937_64 rng((uint64_t)time(nullptr) ^ (uint64_t)(uintptr_t)&rng);
    uint8_t b[16]; for (auto& x : b) x = (uint8_t)rng();
    b[6] = (b[6] & 0x0F) | 0x40; b[8] = (b[8] & 0x3F) | 0x80;
    const char* H = upper ? "0123456789ABCDEF" : "0123456789abcdef"; std::string o;
    for (int i = 0; i < 16; i++) { if (i == 4 || i == 6 || i == 8 || i == 10) o += '-'; o += H[b[i] >> 4]; o += H[b[i] & 15]; }
    return o;
}
// the path (on HOST, https) and the body for a signature of `ms` milliseconds of sound
static std::string path(const std::string& lang = "en", const std::string& cc = "US") {
    return "/discovery/v5/" + lang + "/" + cc + "/android/-/tag/" + uuid4(true) + "/" + uuid4(false) +
           "?sync=true&webv3=true&sampling=true&connected=&shazamapiversion=v3&sharehub=true&video=v3";
}
static std::string body(const std::string& sigBase64, long ms) {
    long ts = (long)time(nullptr);
    return "{\"geolocation\": {\"altitude\": 300, \"latitude\": 45, \"longitude\": 2}, \"signature\": {\"samplems\": " + std::to_string(ms) +
           ", \"timestamp\": " + std::to_string(ts) + ", \"uri\": \"data:audio/vnd.shazam.sig;base64," + sigBase64 + "\"}, \"timestamp\": " +
           std::to_string(ts) + ", \"timezone\": \"UTC\"}";
}
static std::string headers() {
    return std::string("User-Agent: ") + UA + "\r\nAccept: application/json\r\nAccept-Language: en-US,en;q=0.9\r\nContent-Type: application/json\r\n"
           "Content-Language: en_US\r\nX-Shazam-Platform: IPHONE\r\nX-Shazam-AppVersion: 17.1.0\r\n";
}
// the answer: { matches: [{offset, timeskew, frequencyskew}], track: {title, subtitle, images, sections, genres, url} }
static Track parse(const std::string& answer) {
    Track t; js::V r = js::parse(answer);
    const js::V* tr = &r["track"];
    if (!tr->ok() && r["matches"].size()) tr = &r["matches"][0]["track"];
    if (!tr->ok()) return t;
    const js::V& T = *tr;
    t.found = !T["title"].str().empty();
    t.title = T["title"].str(); t.artist = T["subtitle"].str(); t.key = T["key"].str(); t.isrc = T["isrc"].str();
    const js::V& im = T["images"];
    for (const char* k : {"coverarthq", "coverart", "coverArtHQ", "coverArt", "background"}) if (t.cover.empty()) t.cover = im[k].str();
    t.url = T["url"].str(); if (t.url.empty()) t.url = T["share"]["href"].str();
    t.genre = T["genres"]["primary"].str();
    const js::V& secs = T["sections"];
    for (size_t i = 0; i < secs.size(); i++) {
        const js::V& md = secs[i]["metadata"];
        for (size_t k = 0; k < md.size(); k++) {
            std::string ti = md[k]["title"].str(), tx = md[k]["text"].str();
            if (ti == "Album" && t.album.empty()) t.album = tx;
            else if (ti == "Released" && t.released.empty()) t.released = tx;
            else if (ti == "Label" && t.label.empty()) t.label = tx;
        }
    }
    const js::V& m0 = r["matches"][0];
    if (m0.ok()) { t.offset = m0["offset"].num(-1); t.timeSkew = m0["timeskew"].num(0); t.freqSkew = m0["frequencyskew"].num(0); }
    return t;
}
}
