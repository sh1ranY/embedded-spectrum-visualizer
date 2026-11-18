#pragma once
// Signal processing only: no Arduino dependency. ADC values are raw counts,
// not calibrated volts. DEMO is explicitly synthetic and is never ADC evidence.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mc2live {
constexpr size_t FFT_N = 512;
constexpr size_t BANDS = 16;
constexpr uint32_t SAMPLE_RATE = 20000;
constexpr size_t WAVE_SAMPLES = 128;
constexpr double DSP_PI = 3.14159265358979323846;
constexpr float LOW_HZ = 100.f, HIGH_HZ = 8000.f;
constexpr float DISPLAY_FULL_SCALE = 0.75f;
// Eight PCM units per raw ADC count preserves the entire possible 12-bit
// count-minus-frame-mean span without clipping it during signed conversion.
constexpr float ADC_COUNT_TO_PCM = 8.f;
enum class Source : uint8_t { NONE, DEMO, ADC };
enum class View : uint8_t { FREQ, TIME };
inline const char* sourceName(Source s) { return s == Source::ADC ? "ADC" : s == Source::DEMO ? "DEMO" : "NONE"; }
inline const char* viewName(View v) { return v == View::TIME ? "TIME" : "FREQ"; }

struct FrameStats {
  Source source = Source::NONE;
  bool adcValid = false;
  uint32_t samples = 0;
  uint16_t rawMin = 0, rawMax = 0;
  float rawMean = 0, rawAcRms = 0;
  uint32_t lowRail = 0, highRail = 0;
  uint64_t sequence = 0, firstSampleIndex = 0;
};

inline FrameStats convertAdcFrame(const uint16_t* raw, int16_t* pcm,
                                  size_t count = FFT_N) {
  FrameStats s;
  if (!raw || !pcm || !count || count > FFT_N) return s;
  s.source = Source::ADC; s.adcValid = true; s.samples = count;
  s.rawMin = 4095; uint64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t v = raw[i] > 4095 ? 4095 : raw[i];
    if (v < s.rawMin) s.rawMin = v;
    if (v > s.rawMax) s.rawMax = v;
    sum += v; s.lowRail += v <= 4; s.highRail += v >= 4091;
  }
  s.rawMean = static_cast<float>(static_cast<double>(sum) / count);
  double squares = 0;
  for (size_t i = 0; i < count; ++i) {
    const float value = static_cast<float>(raw[i] > 4095 ? 4095 : raw[i]) - s.rawMean;
    squares += value * value;
    pcm[i] = static_cast<int16_t>(std::lround(value * ADC_COUNT_TO_PCM));
  }
  s.rawAcRms = static_cast<float>(std::sqrt(squares / count));
  return s;
}

inline int frequencyBand(float hz) {
  if (!(hz >= LOW_HZ && hz <= HIGH_HZ)) return -1;
  int b = static_cast<int>(std::log(hz / LOW_HZ) / std::log(HIGH_HZ / LOW_HZ) * BANDS);
  return b >= static_cast<int>(BANDS) ? BANDS - 1 : b;
}
inline float bandEdge(size_t i) {
  return LOW_HZ * std::pow(HIGH_HZ / LOW_HZ, static_cast<float>(i) / BANDS);
}
struct Spectrum {
  float level[BANDS] = {};
  uint8_t height[BANDS] = {};
  int peakBand = -1;
  size_t peakBin = 0;
  float peakHz = 0;
};

class SpectrumProcessor {
 public:
  SpectrumProcessor() {
    for (size_t i = 0; i < FFT_N; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(2.0 * DSP_PI * i / (FFT_N - 1));
  }
  // visualGain changes only pixel heights. Returned levels/peak remain actual
  // normalized PCM measurements; it does not fabricate signal sensitivity.
  Spectrum analyse(const int16_t* pcm, size_t valid = FFT_N, float visualGain = 1.f) {
    Spectrum out;
    if (!pcm || !valid || valid > FFT_N) return out;
    double mean = 0; float wsum = 0, wsq = 0;
    for (size_t i = 0; i < valid; ++i) { mean += pcm[i]; wsum += window_[i]; wsq += window_[i] * window_[i]; }
    if (!(wsum > 0)) return out;
    const float enbw = FFT_N * wsq / (wsum * wsum); mean /= valid;
    for (size_t i = 0; i < FFT_N; ++i) {
      re_[i] = i < valid ? static_cast<float>((pcm[i] - mean) / 32768.0) * window_[i] : 0;
      im_[i] = 0;
    }
    fft(); float peak = 0;
    for (size_t bin = 1; bin < FFT_N / 2; ++bin) {
      const float hz = bin * static_cast<float>(SAMPLE_RATE) / FFT_N;
      const int b = frequencyBand(hz); if (b < 0) continue;
      const float m = 2.f * std::sqrt(re_[bin] * re_[bin] + im_[bin] * im_[bin]) / wsum;
      out.level[b] += m * m;
      if (m > peak) { peak = m; out.peakBin = bin; out.peakHz = hz; out.peakBand = b; }
    }
    if (!(visualGain >= 0) || !std::isfinite(visualGain)) visualGain = 1;
    for (size_t b = 0; b < BANDS; ++b) {
      out.level[b] = std::sqrt(out.level[b] / enbw);
      const float height = out.level[b] * visualGain / DISPLAY_FULL_SCALE * 8.f;
      out.height[b] = static_cast<uint8_t>(height >= 8 ? 8 : height <= 0 ? 0 : std::floor(height + 0.5f));
    }
    if (peak < 0.0001f) { out.peakBand = -1; out.peakBin = 0; out.peakHz = 0; }
    return out;
  }
 private:
  float re_[FFT_N] = {}, im_[FFT_N] = {}, window_[FFT_N] = {};
  void fft() {
    for (size_t i = 1, j = 0; i < FFT_N; ++i) {
      size_t bit = FFT_N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
      if (i < j) { float t = re_[i]; re_[i] = re_[j]; re_[j] = t; t = im_[i]; im_[i] = im_[j]; im_[j] = t; }
    }
    for (size_t length = 2; length <= FFT_N; length <<= 1) {
      const float sr = std::cos(-2.0 * DSP_PI / length), si = std::sin(-2.0 * DSP_PI / length);
      for (size_t start = 0; start < FFT_N; start += length) {
        float wr = 1, wi = 0;
        for (size_t j = 0; j < length / 2; ++j) {
          const size_t a = start + j, b = a + length / 2;
          const float r = re_[b] * wr - im_[b] * wi, q = re_[b] * wi + im_[b] * wr;
          re_[b] = re_[a] - r; im_[b] = im_[a] - q; re_[a] += r; im_[a] += q;
          const float next = wr * sr - wi * si; wi = wr * si + wi * sr; wr = next;
        }
      }
    }
  }
};

struct PixelFrame {
  uint8_t rows[8][4] = {};
  bool at(size_t x, size_t y) const { return x < 32 && y < 8 && (rows[y][x / 8] & (1u << (7 - x % 8))); }
  void set(size_t x, size_t y) { if (x < 32 && y < 8) rows[y][x / 8] |= 1u << (7 - x % 8); }
};
inline PixelFrame frequencyPixels(const Spectrum& s) {
  PixelFrame f;
  for (size_t x = 0; x < 32; ++x)
    for (size_t y = 0; y < 8; ++y) if (s.height[x / 2] >= 8 - y) f.set(x, y);
  return f;
}
inline uint8_t sampleRow(int16_t sample, float gain = 1.f) {
  const float y = 3.5f - static_cast<float>(sample) * gain * 3.5f / 32768.f;
  return static_cast<uint8_t>(y <= 0 ? 0 : y >= 7 ? 7 : std::lround(y));
}
inline PixelFrame timePixels(const int16_t* pcm, size_t valid = FFT_N,
                             float visualGain = 1.f, bool trigger = true) {
  PixelFrame f; if (!pcm || !valid) return f;
  if (!(visualGain >= 0) || !std::isfinite(visualGain)) visualGain = 1;
  size_t offset = 0;
  if (trigger && valid > WAVE_SAMPLES) {
    for (size_t i = 1; i <= valid - WAVE_SAMPLES; ++i)
      if (pcm[i - 1] <= 0 && pcm[i] > 0) { offset = i; break; }
  }
  const size_t available = valid - offset < WAVE_SAMPLES ? valid - offset : WAVE_SAMPLES;
  for (size_t x = 0; x < 32; ++x) {
    const size_t a = x * (WAVE_SAMPLES / 32); if (a >= available) break;
    const size_t b = a + (WAVE_SAMPLES / 32) < available ? a + (WAVE_SAMPLES / 32) : available;
    int16_t lo = pcm[offset + a], hi = lo;
    for (size_t i = a + 1; i < b; ++i) { if (pcm[offset + i] < lo) lo = pcm[offset + i]; if (pcm[offset + i] > hi) hi = pcm[offset + i]; }
    const uint8_t top = sampleRow(hi, visualGain), bottom = sampleRow(lo, visualGain);
    for (size_t y = top; y <= bottom; ++y) f.set(x, y);
  }
  return f;
}

constexpr size_t DEMO_MODES = 5;
constexpr float DEMO_TONES[4] = {187.5f, 750.f, 3000.f, 6000.f};
inline const char* demoName(size_t mode) {
  static const char* names[DEMO_MODES] = {"187.5 Hz", "750 Hz", "3000 Hz", "6000 Hz", "750+3000 Hz"};
  return names[mode % DEMO_MODES];
}
inline void fillTone(int16_t* out, size_t count, uint64_t start, float hz,
                     float amplitude = 0.45f, int16_t dc = 0) {
  if (!out) return;
  for (size_t i = 0; i < count; ++i) {
    const double phase = 2.0 * DSP_PI * hz * static_cast<double>(start + i) / SAMPLE_RATE;
    const int32_t v = static_cast<int32_t>(std::lround(std::sin(phase) * amplitude * 32767.0)) + dc;
    out[i] = static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
  }
}
inline FrameStats fillDemo(int16_t* out, size_t mode, uint64_t start,
                           size_t count = FFT_N) {
  FrameStats s; if (!out || !count || count > FFT_N) return s;
  mode %= DEMO_MODES;
  if (mode < 4) fillTone(out, count, start, DEMO_TONES[mode]);
  else for (size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(start + i) / SAMPLE_RATE;
    out[i] = static_cast<int16_t>(std::lround(32767.0 * (0.225 * std::sin(2 * DSP_PI * 750 * t) + 0.225 * std::sin(2 * DSP_PI * 3000 * t))));
  }
  s.source = Source::DEMO; s.samples = count; s.firstSampleIndex = start;
  // adcValid remains false: these are generated PCM, not microphone readings.
  return s;
}
} // namespace mc2live
