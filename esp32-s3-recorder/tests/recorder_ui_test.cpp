#include <cstdlib>
#include <iostream>
#include "../firmware/MC2_Live/recorder_ui.h"

using mc2ui::Action;
using mc2ui::Controller;
using mc2ui::Mode;
static unsigned checks = 0;
static void check(bool ok, const char* label) {
  ++checks;
  if (!ok) { std::cerr << "FAIL " << label << '\n'; std::exit(1); }
}
static void expect(Controller& c, uint32_t t, bool g, bool r, Action action,
                   const char* label) {
  check(c.tick(t, g, r) == action, label);
}
static void arm(Controller& c, uint32_t t = 0) {
  expect(c, t, false, false, Action::NONE, "startup arm");
}
static void tapGreen(Controller& c, uint32_t t, Action action) {
  expect(c, t, true, false, Action::NONE, "green press");
  expect(c, t + 100, false, false, action, "green release");
}
static void tapRed(Controller& c, uint32_t t, Action action) {
  expect(c, t, false, true, Action::NONE, "red press");
  expect(c, t + 100, false, false, action, "red release");
}

int main() {
  Controller c;
  arm(c);
  tapGreen(c, 10, Action::RECORD_TOGGLE);
  c.setMode(Mode::RECORDING);
  tapRed(c, 200, Action::TOGGLE_VIEW);
  tapRed(c, 400, Action::TOGGLE_VIEW);
  tapGreen(c, 600, Action::RECORD_TOGGLE);
  c.setMode(Mode::FINALIZING);
  tapGreen(c, 800, Action::NONE);
  tapRed(c, 1000, Action::TOGGLE_VIEW);
  c.setMode(Mode::LIVE);
  expect(c, 1200, true, false, Action::NONE, "live hold press");
  expect(c, 2399, true, false, Action::NONE, "no early hold");
  expect(c, 2400, true, false, Action::ENTER_BROWSER, "live hold browser");
  c.setMode(Mode::BROWSE);
  expect(c, 2500, true, false, Action::NONE, "held state transition blocked");
  expect(c, 2600, false, false, Action::NONE, "browser release not next");
  tapGreen(c, 2700, Action::NEXT_FILE);
  tapRed(c, 2900, Action::TOGGLE_VIEW);
  expect(c, 3100, false, true, Action::NONE, "review hold press");
  expect(c, 4300, false, true, Action::START_REVIEW, "review starts after hold");
  c.setMode(Mode::REVIEW);
  expect(c, 4400, false, false, Action::NONE, "review hold release blocked");
  tapRed(c, 4500, Action::TOGGLE_VIEW);
  tapGreen(c, 4700, Action::NEXT_FILE);
  c.setMode(Mode::BROWSE);
  expect(c, 4900, true, false, Action::NONE, "exit hold press");
  expect(c, 6100, true, false, Action::EXIT_BROWSER, "exit hold");
  c.setMode(Mode::LIVE);
  expect(c, 6200, false, false, Action::NONE, "exit release cannot start record");
  tapGreen(c, 6300, Action::RECORD_TOGGLE);
  c.setMode(Mode::RECORDING);
  expect(c, 6500, true, false, Action::NONE, "record stop hold press");
  expect(c, 7700, true, false, Action::RECORD_TOGGLE, "hold during recording stops");
  c.setMode(Mode::FINALIZING);
  expect(c, 7800, false, false, Action::NONE, "stop hold release blocked");

  // Startup-held keys never start recording or switch a view on release.
  Controller held;
  expect(held, 0, true, false, Action::NONE, "held boot");
  expect(held, 2000, true, true, Action::NONE, "both held boot");
  expect(held, 3000, false, false, Action::NONE, "boot releases");
  tapGreen(held, 3100, Action::RECORD_TOGGLE);

  // Both overlapping buttons suppress their independent long/short gestures.
  Controller chord;
  arm(chord);
  chord.setMode(Mode::BROWSE);
  expect(chord, 10, true, false, Action::NONE, "chord green first");
  expect(chord, 100, true, true, Action::NONE, "chord second key");
  expect(chord, 1210, true, true, Action::NONE, "chord suppresses green hold");
  expect(chord, 1599, true, true, Action::NONE, "delete threshold not early");
  expect(chord, 1600, true, true, Action::REQUEST_DELETE, "delete prompt chord");
  expect(chord, 1700, true, true, Action::NONE, "no repeat request");
  chord.setMode(Mode::DELETE_CONFIRM);
  expect(chord, 1800, false, true, Action::NONE, "partial release cannot cancel");
  expect(chord, 1900, false, false, Action::NONE, "both release cannot confirm");
  tapRed(chord, 2000, Action::CANCEL_DELETE);
  chord.setMode(Mode::BROWSE);
  expect(chord, 2200, true, true, Action::NONE, "second chord starts");
  expect(chord, 3700, true, true, Action::REQUEST_DELETE, "second delete prompt");
  chord.setMode(Mode::DELETE_CONFIRM);
  expect(chord, 3800, false, false, Action::NONE, "release after second prompt");
  tapGreen(chord, 3900, Action::CONFIRM_DELETE);
  chord.setMode(Mode::BROWSE);
  expect(chord, 4100, false, false, Action::NONE, "no next after confirmation");

  // An aborted chord has no tail action when one key remains held.
  expect(chord, 4200, true, false, Action::NONE, "aborted chord first");
  expect(chord, 4300, true, true, Action::NONE, "aborted chord second");
  expect(chord, 4400, false, true, Action::NONE, "abort one release");
  expect(chord, 7000, false, true, Action::NONE, "no tail review hold");
  expect(chord, 7100, false, false, Action::NONE, "abort last release");
  tapRed(chord, 7200, Action::TOGGLE_VIEW);

  // Outside the file browser a two-button overlap does not delete or record.
  Controller liveChord;
  arm(liveChord);
  expect(liveChord, 10, true, true, Action::NONE, "live chord");
  expect(liveChord, 2000, true, true, Action::NONE, "live chord hold ignored");
  expect(liveChord, 2100, false, false, Action::NONE, "live chord release ignored");

  // Holds still work if polling skips directly from press to release.
  Controller skipped;
  arm(skipped);
  expect(skipped, 10, true, false, Action::NONE, "skipped press");
  expect(skipped, 1310, false, false, Action::ENTER_BROWSER, "skipped boundary hold");
  skipped.setMode(Mode::BROWSE);
  expect(skipped, 1400, false, true, Action::NONE, "skipped red press");
  expect(skipped, 2700, false, false, Action::START_REVIEW, "skipped red hold");

  // millis() subtraction crosses 32-bit wrap without early or late actions.
  Controller wrap;
  arm(wrap, 0xFFFFFF00u);
  expect(wrap, 0xFFFFFF10u, true, false, Action::NONE, "wrap press");
  expect(wrap, uint32_t(0xFFFFFF10u + 1199u), true, false, Action::NONE,
         "wrap no early action");
  expect(wrap, uint32_t(0xFFFFFF10u + 1200u), true, false, Action::ENTER_BROWSER,
         "wrap hold action");
  wrap.setMode(Mode::BROWSE);
  expect(wrap, 1200, false, false, Action::NONE, "wrap release blocked");

  // Slow/held confirmation buttons are not treated as a short destructive tap.
  Controller confirm;
  arm(confirm);
  confirm.setMode(Mode::DELETE_CONFIRM);
  expect(confirm, 10, true, false, Action::NONE, "confirm long press");
  expect(confirm, 1210, true, false, Action::NONE, "long confirm not action");
  expect(confirm, 1300, false, false, Action::NONE, "long confirm release ignored");
  tapGreen(confirm, 1400, Action::CONFIRM_DELETE);

  std::cout << "PASS " << checks << " recorder UI checks\n";
}
