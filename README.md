# Embedded Spectrum Visualizer

Two firmware prototypes for displaying an input signal as either a waveform or a frequency spectrum on a 32 × 8 LED matrix. A 16 × 2 LCD identifies the source and mode.

| Firmware | Input and features | Hardware evidence |
| --- | --- | --- |
| [Arduino UNO R4 WiFi v0.1](uno-r4-live/README.md) | Real microphone input, live waveform and spectrum, hold/resume | A short bench test confirmed sound response, the two controls and physical display operation |
| [ESP32-S3 recorder v0.5](esp32-s3-recorder/README.md) | Generated DEMO signal, WAV recording, file browser, visual review and deletion | 35 automated checks on the board with generated PCM, including an exact 10-second clip and review through its final sample |

The UNO buffers ADC frames for immediate display. The ESP32-S3 sends each PCM frame to the display and a PSRAM recording buffer, then writes the WAV after capture stops. This avoids the flash writes during acquisition that interrupted an earlier prototype.

See the guides for wiring, build settings, recorded hardware observations and validation limits. Both sketches compiled from the release sources and the included host tests passed. The [release check record](release-checks.json) lists the results. The code, test fixtures and selected evidence are from a student engineering prototype developed with AI assistance. Course handouts, teammates' analogue designs and recordings are kept separately. This release grants no additional reuse license.
