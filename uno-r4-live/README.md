# Arduino UNO R4 WiFi live visualizer v0.1

At startup the firmware samples the microphone amplifier's analogue output on A0 and opens `ADC LIVE FREQ`. Press **D4 (green)** to switch between TIME and FREQ. Press **D3 (red)** to hold or resume the display. Sampling continues while the screen is held. This build displays frames in real time and has no recording or file browsing.

## Wiring

| Component | Signal | Arduino UNO R4 WiFi |
| --- | --- | --- |
| Microphone amplifier | Analogue output | A0 |
| I²C LCD1602 at `0x27` | SDA / SCL | A4 / A5 |
| Four MAX7219 modules | DIN / CS / CLK | D11 / D10 / D13 |
| Red button | HOLD / LIVE | D3 to GND when pressed |
| Green button | TIME / FREQ | D4 to GND when pressed |

The tested modules and LM358 microphone amplifier shared 5 V and ground. Verify common ground, supply capacity and that the amplifier output stays within the board's analogue input limits before connecting it. Both buttons use internal pull-ups. Keep the pin functions when substituting button colors.

The build preserves the firmware from the bench test. Two button colors were exchanged after that test, so the archived serial banner and internal `GREEN`/`RED` identifiers use the earlier labels. The table above gives the current physical mapping.

## Build and run

Open [MC2_R4.ino](firmware/MC2_R4/MC2_R4.ino) in Arduino IDE, keep the three headers beside it, and select **Arduino UNO R4 WiFi**. The tested Arduino Renesas UNO core is **1.6.0**. The equivalent Arduino CLI compile command from this directory is:

```sh
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi firmware/MC2_R4
```

Power the board with peripherals checked and connected. The optional 115200 baud Serial Monitor reports source, sampler state and I²C errors. `DEMO` selects the test signal, `TEST` displays a check pattern and `LIVE` restores A0 acquisition.

## Results and limits

- **Bench test, 6 November 2026.** The sketch compiled and was uploaded. Physical observations confirmed LCD text, upright digits in left-to-right order, both button actions and changes in the displayed waveform and spectrum in response to sound. The short run logged roughly 49–50 µs between samples, with no recorded ADC fault or I²C error. The [acceptance extract](evidence/archived-board-checks.json) records the tested revision and scope.
- **Host DSP check.** The nominal 20 kS/s sampler supplies 512 samples per FFT frame. The processor removes each frame's mean, applies a Hann window and displays 16 bands spanning 100–8,000 Hz. The 100, 750, 3,000 and 8,000 Hz synthetic inputs returned 117.188, 742.188, 3,007.812 and 7,968.750 Hz respectively, within one 39.06 Hz bin. The test also checks DC removal, source labels, ADC bounds and waveform pixels.
- **Limits.** The FFT peak is a bin estimate. No calibrated analogue sweep, anti-aliasing measurement or endurance test was performed. A replaced display frame indicates that an older frame was skipped for rendering. There is no claim of lossless streaming. The upload receipt was not paired with independent flash readback.

Run the host test from this directory with a C++17 compiler:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic tests/test_dsp.cpp -o /tmp/r4-spectrum-test
/tmp/r4-spectrum-test
```
