# Third-party code

| Part | Where | Licence |
|---|---|---|
| Signature algorithm | `src/ears/signature.h` is a C++ port of [shazamlite](https://github.com/danielytuk/shazamlite)'s `signature.py` (Copyright (c) 2026 Daniel Richard Todd Back) | MIT - see `shazamlite-MIT.txt` |
| miniaudio | `third_party/miniaudio.h` (David Reid) | public domain or MIT-0 (see the end of the file) |
| stb_image, stb_vorbis | `third_party/stb_image.h`, `third_party/stb_vorbis.c` (Sean Barrett) | public domain or MIT (see the end of each file) |

The signature format was first worked out by the [SongRec](https://github.com/marin-m/SongRec) project and implemented in Python by ShazamIO; shazamlite's version is byte-identical to ShazamIO's, and this port is byte-identical to shazamlite's (tested on tones, noise and music-like signals).
