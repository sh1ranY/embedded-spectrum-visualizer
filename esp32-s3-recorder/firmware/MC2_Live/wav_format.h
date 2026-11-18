#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

namespace mc2record {
static constexpr uint32_t WAV_RATE = 48000;
static constexpr size_t WAV_HEADER = 44;
struct WavInfo { bool valid = false; uint32_t samples = 0; };
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (unsigned n = 0; n < 4; ++n) p[n] = uint8_t(v >> (8 * n)); }
inline void makeWavHeader(uint8_t* p, uint32_t samples) {
  std::memset(p, 0, WAV_HEADER);
  std::memcpy(p, "RIFF", 4); put32(p + 4, samples * 2 + 36);
  std::memcpy(p + 8, "WAVEfmt ", 8); put32(p + 16, 16);
  put16(p + 20, 1); put16(p + 22, 1); put32(p + 24, WAV_RATE);
  put32(p + 28, WAV_RATE * 2); put16(p + 32, 2); put16(p + 34, 16);
  std::memcpy(p + 36, "data", 4); put32(p + 40, samples * 2);
}
// The recorder deliberately writes canonical44-byte PCM WAV. Incomplete .part
// files may have their initial zero-length header, so their actual even payload
// is reviewable. A final .wav must agree exactly with its length/header.
inline WavInfo inspectWav(const uint8_t* p, size_t headerBytes, size_t fileBytes, bool partial) {
  WavInfo r;
  if (!p || headerBytes < WAV_HEADER || fileBytes < WAV_HEADER ||
      std::memcmp(p, "RIFF", 4) || std::memcmp(p + 8, "WAVEfmt ", 8) ||
      get32(p + 16) != 16 || get16(p + 20) != 1 || get16(p + 22) != 1 ||
      get32(p + 24) != WAV_RATE || get32(p + 28) != WAV_RATE * 2 ||
      get16(p + 32) != 2 || get16(p + 34) != 16 || std::memcmp(p + 36, "data", 4)) return r;
  const size_t actual = (fileBytes - WAV_HEADER) & ~size_t(1);
  if (actual > UINT32_MAX - 36) return r;
  if (!partial && (get32(p + 40) != actual || get32(p + 4) != actual + 36 ||
                   (fileBytes - WAV_HEADER) != actual)) return r;
  r.valid = true; r.samples = uint32_t(actual / 2); return r;
}
struct RecordingName { bool owned = false, demo = false, partial = false, legacy = false; uint16_t id = 0; };
inline RecordingName recordingName(const char* path) {
  RecordingName r;
  if (!path) return r;
  const char* s = std::strrchr(path, '/'); s = s ? s + 1 : path;
  const size_t n = std::strlen(s);
  const char* extension = std::strrchr(s, '.');
  if (!extension || (std::strcmp(extension, ".wav") && std::strcmp(extension, ".part"))) return r;
  r.partial = !std::strcmp(extension, ".part");
  size_t digits = 0, first = 0;
  if ((s[0] == 'D' || s[0] == 'M') && size_t(extension - s) == 5) {
    first = 1; digits = 4; r.demo = s[0] == 'D';
  } else if (!std::strncmp(s, "rec_", 4) && size_t(extension - s) == 7) {
    first = 4; digits = 3; r.legacy = true;
  } else return RecordingName{};
  (void)n;
  for (size_t i = first; i < first + digits; ++i) {
    if (s[i] < '0' || s[i] > '9') return RecordingName{};
    r.id = uint16_t(r.id * 10 + unsigned(s[i] - '0'));
  }
  r.owned = true; return r;
}
}  // namespace mc2record
