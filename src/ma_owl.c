// Owl Ears: miniaudio with its device part (WASAPI capture + loopback) and the decoders (MP3 / FLAC / WAV; OGG by stb_vorbis)
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "../third_party/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"
