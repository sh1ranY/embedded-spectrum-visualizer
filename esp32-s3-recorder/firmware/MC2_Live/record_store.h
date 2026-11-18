#pragma once
// MC2 v0.5 local recording store. No network/camera/ADC/pin initialization.
// Mount never formats or removes files. Capture goes to PSRAM without flash
// writes: flash erases can stall BOTH ESP32 cores despite a separate worker.
// The worker saves only AFTER recording stops, while LCD reports SAVING.
#include <Arduino.h>
#include <LittleFS.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "wav_format.h"

namespace mc2record {
enum class State : uint8_t { UNAVAILABLE, IDLE, RECORDING, STOPPING };
struct Entry {
  char name[24] = {};
  uint32_t samples = 0;
  bool demo = false, partial = false, valid = false, legacy = false;
};
class RecordStore {
 public:
  static constexpr size_t FRAME_SAMPLES = 4096, MAX_FILES = 64;
  static constexpr uint32_t MAX_SECONDS = 10, SPACE_RESERVE = 65536;
  RecordStore() = default;
  RecordStore(const RecordStore&) = delete;
  RecordStore& operator=(const RecordStore&) = delete;
#ifdef MC2_RECORD_HOST_TEST
  ~RecordStore() { if (capture_) heap_caps_free(capture_); }
#endif

  bool begin() {
    if (ready()) return true;
    if (!LittleFS.begin(false, "/mc2", 10, "spiffs")) {
      state_ = State::UNAVAILABLE; reason_ = Reason::MOUNT_FAILED; return false;
    }
    if (!capture_) capture_ = static_cast<int16_t*>(heap_caps_malloc(MAX_SECONDS * WAV_RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#ifndef MC2_RECORD_HOST_TEST
    if (!worker_ && xTaskCreatePinnedToCore(workerEntry, "mc2-wav", 6144, this, 1, &worker_, 0) != pdPASS) {
      reason_ = Reason::NO_MEMORY; return false;
    }
#endif
    clearPcmCopies();
    state_ = State::IDLE; reason_ = capture_ ? Reason::READY : Reason::NEED_PSRAM; scan(); return true;
  }
  // Explicit initialization is allowed only when EVERY byte of the named
  // partition is erased. A failed mount alone never grants format permission.
  bool formatBlankPartition() {
    if (busy() || ready()) return false;
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!p) { reason_ = Reason::NO_PARTITION; return false; }
    uint8_t bytes[256];
    for (size_t offset = 0; offset < p->size; offset += sizeof(bytes)) {
      const size_t length = std::min(sizeof(bytes), size_t(p->size - offset));
      if (esp_partition_read(p, offset, bytes, length) != ESP_OK) { reason_ = Reason::READ_FAILED; return false; }
      for (size_t n = 0; n < length; ++n) if (bytes[n] != 0xFF) { reason_ = Reason::NOT_BLANK; return false; }
    }
    // begin(false) selects the intended partition label, even on failed mount.
    if (!LittleFS.format()) { reason_ = Reason::FORMAT_FAILED; return false; }
    return begin();
  }
  bool ready() const { return state_.load() != State::UNAVAILABLE; }
  State state() const { return state_.load(); }
  bool recording() const { return state() == State::RECORDING; }
  bool busy() const { return recording() || state() == State::STOPPING; }
  uint32_t writtenSamples() const { return written_.load(); }
  uint32_t acceptedSamples() const { return submitted_; }
  bool recordingAvailable() const { return ready() && capture_; }
  uint32_t queuedSamples() const { return submitted_ >= writtenSamples() ? submitted_ - writtenSamples() : 0; }
  const char* activeName() const { return activeName_; }
  const char* lastReason() const { return reasonName(reason_.load()); }
  size_t count() const { return count_; }
  bool listOverflow() const { return listOverflow_; }
  const Entry& entry(size_t i) const {
    static const Entry empty{};
    if (i >= count_) return empty;
    RecordStore* self = const_cast<RecordStore*>(this);
    if (i / MAX_FILES != cachePage_) self->loadPage(i / MAX_FILES);
    return i % MAX_FILES < cachedCount_ ? entries_[i % MAX_FILES] : empty;
  }
  size_t storageTotalBytes() const { return totalBytes_; }
  size_t storageUsedBytes() const { return usedBytes_; }
  size_t freeBytes() const { return totalBytes_ >= usedBytes_ ? totalBytes_ - usedBytes_ : 0; }

  bool start(bool demo) {
    if (!ready()) { reason_ = Reason::MOUNT_FAILED; return false; }
    if (busy()) { return false; }
    if (!capture_) { reason_ = Reason::NEED_PSRAM; return false; }
    closeReview(); scan();
    if (count_ >= MAX_FILES || listOverflow_) { reason_ = Reason::FILE_LIMIT; return false; }
    if (freeBytes() < SPACE_RESERVE + WAV_HEADER + FRAME_SAMPLES * 2) { reason_ = Reason::FULL; return false; }
    uint16_t id = 0;
    for (uint16_t attempt = 1; attempt <= 9999; ++attempt) {
      char d[24], m[24];
      snprintf(d, sizeof(d), "/D%04u.wav", unsigned(attempt)); snprintf(m, sizeof(m), "/M%04u.wav", unsigned(attempt));
      bool exists = LittleFS.exists(d) || LittleFS.exists(m);
      snprintf(d, sizeof(d), "/D%04u.part", unsigned(attempt)); snprintf(m, sizeof(m), "/M%04u.part", unsigned(attempt));
      if (!exists && !LittleFS.exists(d) && !LittleFS.exists(m)) { id = attempt; break; }
    }
    if (!id) { reason_ = Reason::FILE_LIMIT; return false; }
    snprintf(activeName_, sizeof(activeName_), "%c%04u.wav", demo ? 'D' : 'M', unsigned(id));
    snprintf(activePart_, sizeof(activePart_), "/%c%04u.part", demo ? 'D' : 'M', unsigned(id));
    snprintf(activeFinal_, sizeof(activeFinal_), "/%s", activeName_);
    // No open("w"), header write, flush or erase while RECORDING. Name and
    // capacity are reserved; the only filesystem owner saves after STOPPING.
    submitted_ = 0; written_ = 0;
    maxSamples_ = uint32_t(std::min<size_t>(MAX_SECONDS * WAV_RATE, (freeBytes() - SPACE_RESERVE - WAV_HEADER) / 2));
    saveOpened_ = false; clearPcmCopies();
    dirty_ = false; reason_ = Reason::RECORDING; state_ = State::RECORDING; return true;
  }
  bool enqueue(const int16_t* pcm, size_t samples) {
    if (!recording() || !pcm || samples == 0 || samples > FRAME_SAMPLES) return false;
    const uint32_t remaining = maxSamples_ - submitted_;
    const size_t keep = std::min(samples, size_t(remaining));
    if (!keep) { requestStop("TIME LIMIT"); return false; }
    std::memcpy(capture_ + submitted_, pcm, keep * sizeof(int16_t));
    submitted_ += keep;
    if (submitted_ >= maxSamples_) requestStop(maxSamples_ == MAX_SECONDS * WAV_RATE ? "TIME LIMIT" : "STORAGE FULL");
    return true;
  }
  void requestStop(const char* reason = "STOPPED") {
    if (!recording()) return;
    // A worker write error may finish concurrently. Do not replace its reason
    // or resurrect STOPPING after it has published IDLE.
    Reason expectedReason = Reason::RECORDING;
    reason_.compare_exchange_strong(expectedReason, parseReason(reason));
    State expectedState = State::RECORDING;
    state_.compare_exchange_strong(expectedState, State::STOPPING);
  }
  void tick() {
#ifdef MC2_RECORD_HOST_TEST
    workerStep();
#endif
    if (!busy() && dirty_.exchange(false)) {
      // Scrub in the producer/main task after the worker has release-published
      // IDLE. Scrubbing from the worker on write failure could race an enqueue
      // that had just observed RECORDING. This covers successful stops and
      // all write/finalization failures without waiting for delete.
      clearPcmCopies(); scan();
    }
  }
  bool openReview(size_t index) {
    if (!ready() || busy() || index >= count_) return false;
    closeReview();
    const Entry& e = entry(index);
    if (!e.valid || !e.samples) { reason_ = Reason::INVALID_WAV; return false; }
    snprintf(reviewName_, sizeof(reviewName_), "/%s", e.name);
    review_ = LittleFS.open(reviewName_, "r");
    if (!review_ || !review_.seek(WAV_HEADER)) { closeReview(); reason_ = Reason::READ_FAILED; return false; }
    reviewTotal_ = e.samples; reviewRead_ = 0; return true;
  }
  size_t readReviewFrame(int16_t* pcm, size_t capacity) {
    if (!pcm || !capacity) return 0;
    std::memset(pcm, 0, capacity * sizeof(int16_t));
    if (!review_ || busy() || reviewRead_ >= reviewTotal_) return 0;
    const size_t count = std::min(capacity, size_t(reviewTotal_ - reviewRead_));
    const size_t bytes = review_.read(reinterpret_cast<uint8_t*>(pcm), count * 2);
    if (bytes != count * 2) {
      std::memset(pcm, 0, capacity * sizeof(int16_t));
      reason_ = Reason::READ_FAILED; closeReview(); return 0;
    }
    reviewRead_ += count; return count;
  }
  bool rewindReview() {
    if (!review_ || busy()) return false;
    if (!review_.seek(WAV_HEADER)) { reason_ = Reason::READ_FAILED; return false; }
    reviewRead_ = 0; return true;
  }
  void closeReview() {
    if (review_) review_.close();
    reviewRead_ = reviewTotal_ = 0;
    std::memset(reviewName_, 0, sizeof(reviewName_));
  }
  uint32_t reviewSamplesRead() const { return reviewRead_; }
  uint32_t reviewTotalSamples() const { return reviewTotal_; }
  bool deleteAt(size_t index) {
    if (!ready() || busy() || index >= count_) return false;
    closeReview();
    char path[32]; snprintf(path, sizeof(path), "/%s", entry(index).name);
    if (!recordingName(path).owned || !LittleFS.remove(path)) { reason_ = Reason::DELETE_FAILED; return false; }
    clearPcmCopies(); scan(); reason_ = Reason::DELETED; return true;
  }
  bool deleteAll() {
    if (!ready() || busy()) return false;
    closeReview();
    // Re-scan after each removal. This also reaches recordings beyond the
    // bounded browse cache; arbitrary non-recording files are never removed.
    do {
      scan();
      if (!count_) break;
      for (size_t i = 0; i < cachedCount_; ++i) {
        char path[32]; snprintf(path, sizeof(path), "/%s", entries_[i].name);
        if (!LittleFS.remove(path)) { reason_ = Reason::DELETE_FAILED; scan(); return false; }
      }
    } while (true);
    clearPcmCopies(); reason_ = Reason::DELETED; return true;
  }

 private:
  enum class Reason : uint8_t { READY, RECORDING, STOPPED, LIMIT, FULL, QUEUE_FULL, INPUT_GAP, INPUT_ERROR, TIMING_GAP,
    MOUNT_FAILED, NO_MEMORY, NEED_PSRAM, OPEN_FAILED, WRITE_FAILED, FINALIZE_FAILED, VERIFY_FAILED, RENAME_FAILED,
    READ_FAILED, INVALID_WAV, FILE_LIMIT, DELETE_FAILED, DELETED, NO_PARTITION, NOT_BLANK, FORMAT_FAILED };
  std::atomic<State> state_{State::UNAVAILABLE};
  std::atomic<Reason> reason_{Reason::MOUNT_FAILED};
  std::atomic<uint32_t> written_{0};
  std::atomic<bool> dirty_{false};
  uint32_t submitted_ = 0, maxSamples_ = 0;
  Entry entries_[MAX_FILES]{};
  size_t count_ = 0, cachedCount_ = 0, cachePage_ = 0, totalBytes_ = 0, usedBytes_ = 0;
  bool listOverflow_ = false;
  char activeName_[24]{}, activePart_[32]{}, activeFinal_[32]{}, reviewName_[32]{};
  File writer_, review_;
  uint32_t reviewRead_ = 0, reviewTotal_ = 0;
  TaskHandle_t worker_ = nullptr;
  int16_t* capture_ = nullptr;
  // Stage a block in internal DRAM before LittleFS writes/flash-cache stalls.
  int16_t staging_[FRAME_SAMPLES]{};
  bool saveOpened_ = false;
  static const char* reasonName(Reason r) {
    switch (r) {
      case Reason::READY: return "READY"; case Reason::RECORDING: return "RECORDING";
      case Reason::STOPPED: return "STOPPED"; case Reason::LIMIT: return "TIME LIMIT";
      case Reason::FULL: return "STORAGE FULL"; case Reason::QUEUE_FULL: return "QUEUE FULL";
      case Reason::INPUT_GAP: return "INPUT GAP"; case Reason::INPUT_ERROR: return "INPUT ERROR";
      case Reason::TIMING_GAP: return "TIMING GAP";
      case Reason::MOUNT_FAILED: return "STORAGE OFF"; case Reason::NO_MEMORY: return "NO MEMORY";
      case Reason::NEED_PSRAM: return "NEED PSRAM";
      case Reason::OPEN_FAILED: return "OPEN FAILED"; case Reason::WRITE_FAILED: return "WRITE FAILED";
      case Reason::FINALIZE_FAILED: return "HEADER FAILED"; case Reason::RENAME_FAILED: return "RENAME FAILED";
      case Reason::VERIFY_FAILED: return "VERIFY FAILED";
      case Reason::READ_FAILED: return "READ FAILED"; case Reason::INVALID_WAV: return "INVALID WAV";
      case Reason::FILE_LIMIT: return "FILE LIMIT"; case Reason::DELETE_FAILED: return "DELETE FAILED";
      case Reason::DELETED: return "DELETED"; case Reason::NO_PARTITION: return "NO PARTITION";
      case Reason::NOT_BLANK: return "NOT BLANK"; case Reason::FORMAT_FAILED: return "FORMAT FAILED";
    }
    return "UNKNOWN";
  }
  static Reason parseReason(const char* s) {
    if (!s) return Reason::STOPPED;
    if (!std::strcmp(s, "TIME LIMIT")) return Reason::LIMIT;
    if (!std::strcmp(s, "STORAGE FULL")) return Reason::FULL;
    if (!std::strcmp(s, "QUEUE FULL")) return Reason::QUEUE_FULL;
    if (!std::strcmp(s, "INPUT GAP")) return Reason::INPUT_GAP;
    if (!std::strcmp(s, "INPUT ERROR")) return Reason::INPUT_ERROR;
    if (!std::strcmp(s, "TIMING GAP")) return Reason::TIMING_GAP;
    return Reason::STOPPED;
  }
  void clearPcmCopies() {
    std::memset(staging_, 0, sizeof(staging_));
    if (capture_) std::memset(capture_, 0, MAX_SECONDS * WAV_RATE * sizeof(int16_t));
  }
  static bool before(const Entry& a, const Entry& b) {
    const auto x = recordingName(a.name), y = recordingName(b.name);
    if (x.id != y.id) return x.id < y.id;
    return std::strcmp(a.name, b.name) < 0;
  }
  // Keep the lowest64 sorted entries strictly after anchor, while counting ALL
  // recordings. Historical stores larger than the new-file cap remain fully
  // browseable without a heap allocation or hiding their later recordings.
  void scanWindow(const Entry& anchor, bool afterAnchor) {
    count_ = cachedCount_ = 0;
    std::memset(entries_, 0, sizeof(entries_));
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        const RecordingName name = recordingName(f.name());
        if (!f.isDirectory() && name.owned) {
          ++count_;
          Entry e;
            const char* base = std::strrchr(f.name(), '/'); base = base ? base + 1 : f.name();
            snprintf(e.name, sizeof(e.name), "%s", base);
            e.demo = name.demo; e.partial = name.partial; e.legacy = name.legacy;
          if (!afterAnchor || before(anchor, e)) {
            const size_t at = size_t(std::lower_bound(entries_, entries_ + cachedCount_, e, before) - entries_);
            if (at < MAX_FILES) {
              uint8_t header[WAV_HEADER];
              const size_t n = f.read(header, sizeof(header));
              const WavInfo info = inspectWav(header, n, f.size(), e.partial);
              e.valid = info.valid; e.samples = info.samples;
              const size_t newCount = std::min(cachedCount_ + 1, MAX_FILES);
              for (size_t move = newCount - 1; move > at; --move) entries_[move] = entries_[move - 1];
              entries_[at] = e; cachedCount_ = newCount;
            }
          }
        }
        f.close(); f = root.openNextFile();
      }
      root.close();
    }
    listOverflow_ = count_ > MAX_FILES;
  }
  void loadPage(size_t page) {
    Entry anchor;
    for (size_t current = 0; current <= page; ++current) {
      scanWindow(anchor, current != 0);
      cachePage_ = current;
      if (!cachedCount_) return;
      anchor = entries_[cachedCount_ - 1];
    }
  }
  void scan() {
    if (busy()) return;
    loadPage(0);
    totalBytes_ = LittleFS.totalBytes(); usedBytes_ = LittleFS.usedBytes();
  }
  static void workerEntry(void* context) {
    RecordStore* self = static_cast<RecordStore*>(context);
    for (;;) { self->workerStep(); vTaskDelay(pdMS_TO_TICKS(1)); }
  }
  void workerStep() {
    // A second task alone does not prevent SPI-flash erase from stalling the
    // sampling/display core. Never touch flash while capture is running.
    if (state() != State::STOPPING) return;
    if (!saveOpened_) {
      if (LittleFS.exists(activePart_) || LittleFS.exists(activeFinal_)) {
        reason_ = Reason::OPEN_FAILED; finish(false); return;
      }
      writer_ = LittleFS.open(activePart_, "w");
      if (!writer_) { reason_ = Reason::OPEN_FAILED; finish(false); return; }
      // Capture has already stopped, so its exact accepted length is known.
      // Write the FINAL header once. Seeking back after a large LittleFS file
      // has been committed can require a whole-file copy-on-write allocation.
      uint8_t header[WAV_HEADER]; makeWavHeader(header, submitted_);
      if (writer_.write(header, sizeof(header)) != sizeof(header)) {
        reason_ = Reason::WRITE_FAILED; finish(false); return;
      }
      saveOpened_ = true;
    }
    const uint32_t written = writtenSamples();
    const size_t count = std::min<size_t>(FRAME_SAMPLES, submitted_ - written);
    if (count) {
      std::memcpy(staging_, capture_ + written, count * sizeof(int16_t));
      const size_t expected = count * sizeof(int16_t);
      const size_t bytes = writer_.write(reinterpret_cast<const uint8_t*>(staging_), expected);
      if (bytes != expected) {
        // Any even written prefix remains recoverable in .part. Do not append
        // future frames across a storage failure or pretend they were saved.
        written_.fetch_add(uint32_t(bytes / 2));
        reason_ = Reason::WRITE_FAILED; finish(false); return;
      }
      written_.fetch_add(uint32_t(count));
      std::memset(staging_, 0, sizeof(staging_));
    }
    if (writtenSamples() == submitted_) finish(true);
  }
  void finish(bool complete) {
    const uint32_t samples = writtenSamples();
    if (writer_) writer_.flush();
    writer_.close();
    if (complete) {
      // flush()/close() return no useful commit status. Reopen the actual file
      // before rename, checking its complete canonical header and exact size.
      // A .part may remain recoverable even when the committed size is short.
      File check = LittleFS.open(activePart_, "r");
      uint8_t header[WAV_HEADER];
      const size_t n = check ? check.read(header, sizeof(header)) : 0;
      const WavInfo info = inspectWav(header, n, check ? check.size() : 0, false);
      if (check) check.close();
      if (!info.valid || info.samples != submitted_ || samples != submitted_) {
        reason_ = Reason::VERIFY_FAILED; complete = false;
      }
    }
    if (complete && !LittleFS.rename(activePart_, activeFinal_)) reason_ = Reason::RENAME_FAILED;
    dirty_ = true;
    // Release-publish all file operations before main is allowed to scan/open.
    state_ = State::IDLE;
  }
};
}  // namespace mc2record
