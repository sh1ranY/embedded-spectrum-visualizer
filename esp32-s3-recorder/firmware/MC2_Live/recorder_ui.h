#pragma once

// Pure two-button gestures. Inputs are the display adapter's debounced levels,
// never raw GPIO. The application owns storage and accepts/rejects each action.
#include <cstdint>

namespace mc2ui {

enum class Mode : uint8_t {
  LIVE, RECORDING, FINALIZING, BROWSE, REVIEW, DELETE_CONFIRM
};

enum class Action : uint8_t {
  NONE, RECORD_TOGGLE, TOGGLE_VIEW, ENTER_BROWSER, EXIT_BROWSER, NEXT_FILE,
  START_REVIEW, REQUEST_DELETE, CONFIRM_DELETE, CANCEL_DELETE
};

class Controller {
 public:
  static constexpr uint32_t HOLD_MS = 1200;
  static constexpr uint32_t DELETE_HOLD_MS = 1500;

  Mode mode() const { return mode_; }

  // Switching state while a key is held cannot leak that key's eventual release
  // into the new state (especially deletion confirmation or returning to LIVE).
  void setMode(Mode next) {
    if (next == mode_) return;
    mode_ = next;
    waitingRelease_ = greenDown_ || redDown_;
    resetGesture();
  }

  Action tick(uint32_t now, bool greenDown, bool redDown) {
    const bool greenPress = greenDown && !greenDown_;
    const bool redPress = redDown && !redDown_;
    const bool greenRelease = !greenDown && greenDown_;
    const bool redRelease = !redDown && redDown_;
    greenDown_ = greenDown;
    redDown_ = redDown;

    // Also suppress a key held during startup. Both keys must first be up.
    if (waitingRelease_) {
      if (!greenDown && !redDown) {
        waitingRelease_ = false;
        resetGesture();
      }
      return Action::NONE;
    }

    if (greenPress) { greenAt_ = now; greenUsed_ = false; }
    if (redPress) { redAt_ = now; redUsed_ = false; }

    // Any overlap forms one chord and consumes both individual gestures. If it
    // is released early, no short action or one-key hold occurs afterwards.
    if (greenDown && redDown && !chord_) {
      chord_ = true;
      chordAt_ = now;
      chordUsed_ = false;
      greenUsed_ = redUsed_ = true;
    }
    if (chord_) {
      if (!greenDown && !redDown) {
        resetGesture();
        return Action::NONE;
      }
      if (greenDown && redDown && !chordUsed_ &&
          elapsed(now, chordAt_) >= DELETE_HOLD_MS && browsing()) {
        chordUsed_ = true;
        return Action::REQUEST_DELETE;
      }
      return Action::NONE;
    }

    if (greenDown && !greenUsed_ && elapsed(now, greenAt_) >= HOLD_MS) {
      greenUsed_ = true;
      if (mode_ == Mode::LIVE) return Action::ENTER_BROWSER;
      if (mode_ == Mode::RECORDING) return Action::RECORD_TOGGLE;
      if (browsing()) return Action::EXIT_BROWSER;
    }
    if (redDown && !redUsed_ && elapsed(now, redAt_) >= HOLD_MS) {
      redUsed_ = true;
      if (browsing()) return Action::START_REVIEW;
    }

    if (greenRelease && !greenUsed_) {
      greenUsed_ = true;
      if (elapsed(now, greenAt_) >= HOLD_MS) {
        // The caller may not have polled at the exact hold boundary.
        if (mode_ == Mode::LIVE) return Action::ENTER_BROWSER;
        if (mode_ == Mode::RECORDING) return Action::RECORD_TOGGLE;
        if (browsing()) return Action::EXIT_BROWSER;
      } else {
        if (mode_ == Mode::LIVE || mode_ == Mode::RECORDING)
          return Action::RECORD_TOGGLE;
        if (browsing()) return Action::NEXT_FILE;
        if (mode_ == Mode::DELETE_CONFIRM) return Action::CONFIRM_DELETE;
      }
    }
    if (redRelease && !redUsed_) {
      redUsed_ = true;
      if (elapsed(now, redAt_) >= HOLD_MS) {
        if (browsing()) return Action::START_REVIEW;
      } else {
        if (mode_ == Mode::DELETE_CONFIRM) return Action::CANCEL_DELETE;
        return Action::TOGGLE_VIEW;
      }
    }
    return Action::NONE;
  }

 private:
  Mode mode_ = Mode::LIVE;
  bool greenDown_ = false, redDown_ = false, waitingRelease_ = true;
  bool greenUsed_ = true, redUsed_ = true, chord_ = false, chordUsed_ = false;
  uint32_t greenAt_ = 0, redAt_ = 0, chordAt_ = 0;

  bool browsing() const { return mode_ == Mode::BROWSE || mode_ == Mode::REVIEW; }
  static uint32_t elapsed(uint32_t now, uint32_t from) { return now - from; }
  void resetGesture() {
    greenUsed_ = redUsed_ = true;
    chord_ = chordUsed_ = false;
  }
};

}  // namespace mc2ui
