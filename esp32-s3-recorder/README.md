# ESP32-S3 recording and visual review v0.5

This prototype starts in `DEMO LIVE FREQ`, using generated PCM. Capture stores a WAV on the board, and visual review reads the samples from that saved file for display in TIME or FREQ. A speaker output is outside this prototype's scope.

## Build and wiring

Open [MC2_Live.ino](firmware/MC2_Live/MC2_Live.ino) with all six adjacent headers. The tested configuration is **Arduino-ESP32 3.3.12** on an ESP32-S3 with **8 MB flash** and **8 MB OPI PSRAM**, the `default_8MB` partition scheme, and USB CDC, MSC and DFU on boot disabled. The test used the CH343 USB-to-UART connector and left native USB disconnected because GPIO19 and GPIO20 are used by the buttons.

| Component | Signal | ESP32-S3 |
| --- | --- | --- |
| Green button | Record / browse | GPIO19 to ground when pressed |
| Red button | TIME / FREQ | GPIO20 to ground when pressed |
| Four MAX7219 modules | DIN / CS / CLK | GPIO21 / GPIO14 / GPIO47 |
| I²C LCD1602 at `0x27` | SDA / SCL | GPIO41 / GPIO42 |

Verify board pin availability, peripheral supply voltages and logic levels before wiring. Camera and ADC acquisition are disabled by default. From this directory, compile with:

```sh
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,UploadSpeed=230400' \
  firmware/MC2_Live
```

Use compatible flash and partition settings when uploading to your own board. A previously provisioned compatible LittleFS partition is mounted at boot. If the partition is blank, `STORAGE INIT BLANK` in the 115200 baud serial console initializes it only after checking for erased bytes. Mount failure never triggers an automatic format.

## Controls

| Mode | Green | Red |
| --- | --- | --- |
| LIVE | Tap to start recording, hold about 1.2 s to browse files | Tap to toggle TIME/FREQ |
| REC | Tap to stop and save | Tap to toggle TIME/FREQ |
| FILES | Tap for next file, hold to return to LIVE | Tap to choose TIME/FREQ, hold to review |
| PLAY or END | Tap for next file, hold to return to LIVE | Tap to toggle TIME/FREQ, hold to restart review |

In FILES, PLAY or END, hold both buttons about 1.5 s to open deletion confirmation. Release both first, then tap green to confirm or red to cancel. The file browser's last entry, ALL FILES, applies the same confirmation to supported audio files. The LCD marks generated recordings with a `D` prefix.

## Storage design and evidence

The same 4,096-sample frame feeds the visualizer and the PSRAM buffer. A 960,000-byte buffer holds up to 10 seconds of 48 kHz mono 16-bit PCM. Flash writes during capture caused a timing gap in an earlier board test, so this version defers them until SAVE. Rewriting the WAV header after writing a long clip also caused a space failure. The recorder now writes the final header once, verifies the closed file, then renames the verified part file.

- **Board run, 5 November 2026.** The [archived 35 checks](evidence/archived-board-checks.json) passed with generated PCM. They include two recordings without overwrite, review of saved 750 Hz data while the live generator was set to 3,000 Hz, persistence across reset, delete/cancel flows and an exact 480,000-sample recording read through EOF.
- **Host tests.** Run `python3 run_host_checks.py` in this directory with Python 3 and Clang installed. The suites exercise signal processing, the UI controller, storage fault handling, display command generation and the actual sketch with mocked peripherals. They cannot establish physical timing or electrical performance.
- **Limits.** The approximately 1.5 MiB LittleFS partition is shared by all recordings and stores roughly 15 seconds in total, with overhead. Files are never automatically overwritten. Unsaved PSRAM data disappears on power loss. Deletion is a logical file operation. The complete physical button/LCD workflow, microphone capture, clipping, noise and long-term storage durability remain unverified for this branch.
