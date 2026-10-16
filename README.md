# TelephoneAI

## INMP441 Microphone Library

This document specifies the INMP441 digital MEMS microphone interface and a proposed acquisition library for ESP32 with ESP-IDF. Hardware specifications are taken from **InvenSense INMP441 Datasheet DS-INMP441-00, Rev. 1.1 (2014)**. Software architecture and defaults are design recommendations, not capabilities guaranteed by the microphone.

### 1. I2S communication

| Property | Specification |
|---|---|
| Interface | Standard Philips I2S |
| Microphone role | Slave; ESP32 supplies clocks |
| Audio sample resolution | 24 bits |
| Representation | Signed two's complement PCM |
| Bit order | MSB first |
| Word slot | 32 SCK cycles |
| Stereo frame | 64 SCK cycles (left + right) |
| Data alignment | First data bit one SCK cycle after WS transition |
| Channel selection | L/R pin selects left or right |
| Output when inactive | SD is high impedance (tri-state) |

One INMP441 provides **mono** audio in one of the two I2S slots. Two microphones can share SCK, WS, and SD when assigned opposite channels. The SD line should have a 100 kΩ pull-down. The unused part of a slot is **not guaranteed to be zero**.

```text
                    One stereo frame: 64 SCK cycles
        +---------------------------+---------------------------+
WS      |       LOW / LEFT          |       HIGH / RIGHT        |
        +---------------------------+---------------------------+
SCK     |        32 cycles          |         32 cycles         |
SD      | 1-cycle delay, 24 data bits, remaining clocks / Hi-Z |
```

### 2. Clock and timing

The microphone requires **64 SCK cycles per WS frame**:

`f_SCK = 64 × f_s`

| Timing parameter | Datasheet value |
|---|---|
| WS/sample frequency | 7.8–50 kHz |
| SCK frequency | 0.5–3.2 MHz |
| Minimum SCK high time | 50 ns |
| Minimum SCK low time | 50 ns |
| Minimum SCK period | 312 ns |
| Minimum WS setup | 0 ns |
| Minimum WS hold | 20 ns |

| Sample rate | SCK frequency |
|---:|---:|
| 8 kHz | 512 kHz |
| 16 kHz | 1.024 MHz |
| 24 kHz | 1.536 MHz |
| 32 kHz | 2.048 MHz |
| 44.1 kHz | 2.8224 MHz |
| 48 kHz | 3.072 MHz |

**TelephoneAI default recommendation:** 16 kHz mono for speech capture. This is an application choice; any downstream audio API must be configured to accept the actual rate and encoding.

### 3. Channel selection

| L/R | I2S channel | WS during data |
|---|---|---|
| LOW | Left | LOW |
| HIGH | Right | HIGH |

Configure ESP-IDF I2S RX to receive the selected channel. Confirm channel order and packing with the specific ESP-IDF version and ESP32 target.

### 4. Audio samples and conversion

Each valid sample is a 24-bit signed integer in two's complement:

`-8,388,608 <= sample <= 8,388,607`

The microphone outputs sample **amplitude**, not encoded sound files. To reconstruct audio, preserve the sample order and the sampling frequency.

If a correctly decoded signed 24-bit value is available in `int32_t pcm24`, a simple truncating PCM16 conversion is:

```c
int16_t pcm16 = (int16_t)(pcm24 >> 8);
```

If the I2S driver returns a **left-aligned** 24-bit value in a signed 32-bit container, first decode it with an arithmetic right shift of 8 bits. **Do not assume** this layout without checking the driver configuration and a capture. The I2S bit order (MSB first) and PCM16 byte order (typically little-endian in WAV and many streaming formats) are different concepts.

### 5. Hardware connections

| INMP441 pin | Signal | Purpose |
|---:|---|---|
| 1 | SCK | I2S serial clock input |
| 2 | SD | I2S data output |
| 3 | WS | I2S word select input |
| 4 | L/R | Channel selection |
| 5, 6, 9 | GND | Ground |
| 7 | VDD | Power |
| 8 | CHIPEN | Enable: HIGH = normal operation, LOW = power-down |

- Supply: **1.8–3.3 V** (recommended operating range); absolute maximum **3.63 V**.
- Place a **100 nF** decoupling capacitor close to VDD/GND.
- Use a **100 kΩ** pull-down on SD, especially with shared SD lines.
- Avoid driving active SCK/WS clocks continuously when the microphone is unpowered.
- Allow startup settling: the datasheet gives approximately **85 ms** after initial power-up with SCK at 3.072 MHz. Initial output samples can be zero.

### 6. Acoustic specifications

| Property | Datasheet value |
|---|---|
| Directivity | Omnidirectional |
| Frequency response (-3 dB points) | 60 Hz–15 kHz |
| Typical sensitivity | -26 dBFS at 1 kHz / 94 dB SPL |
| SNR | 61 dBA |
| Dynamic range | 87 dB |
| Maximum acoustic input | 120 dB SPL peak |
| ADC | Internal sigma-delta |
| Digital filtering | Internal high-pass and low-pass |
| Low-pass passband | Approximately `0.423 × f_s` |

At `f_s = 16 kHz`, the digital low-pass passband extends to about **6.77 kHz**.

### 7. Proposed ESP-IDF configuration

These values describe a **recommended implementation**, not fixed sensor requirements.

| Setting | Suggested value |
|---|---|
| Peripheral | ESP32 I2S RX |
| Clock master | ESP32 |
| Protocol | Philips I2S |
| Sampling rate | 16,000 Hz |
| Sensor sample width | 24-bit signed |
| Slot width | 32 bits |
| Channel | Mono, matching L/R |
| Acquisition | I2S driver + DMA |
| Application representation | PCM16 signed mono, optionally PCM24 |
| PCM16 serialization | Little-endian |
| Buffering | Configurable ring buffer |
| Overflow behavior | Explicitly selected and counted |

Verify ESP-IDF I2S slot settings, mono packing, and DMA layout for the exact chip and ESP-IDF version before implementing sample extraction.

### 8. Library architecture

```text
INMP441
   |  I2S: 24-bit samples in 32-clock slots
   v
ESP32 I2S RX + DMA
   |
   v
24-bit decoding / sign extension
   |
   v
Optional PCM24 -> PCM16 conversion
   |
   v
Ring buffer
   |                     |
   v                     v
WAV recording        Streaming component
                         |
                         v
                  WebSocket / audio server
```

Keep WebSocket transport and GPT-specific protocols **outside** the INMP441 driver. The driver should expose audio samples to independent consumers.

#### Proposed API (not yet implemented)

```c
esp_err_t inmp441_init(const inmp441_config_t *config);
esp_err_t inmp441_start(void);
esp_err_t inmp441_read(int16_t *samples, size_t max_samples,
                       size_t *samples_read, uint32_t timeout_ms);
esp_err_t inmp441_stop(void);
esp_err_t inmp441_deinit(void);
```

#### Implementation checklist

1. Initialize GPIO, I2S RX, sample clock, and channel selection.
2. Configure DMA and allocate receive buffers.
3. Start capture and account for microphone startup time.
4. Read complete slots and verify bit alignment/sign extension.
5. Convert to PCM16 if requested by the consumer.
6. Write samples to a bounded ring buffer.
7. Expose blocking/timed reads and the number of samples actually returned.
8. Synchronize producer/consumer access across FreeRTOS tasks.
9. Count overruns, dropped samples, timeouts, and driver errors.
10. Stop and release DMA, I2S, tasks, and buffers cleanly.

### 9. Buffer sizing

For **16 kHz mono PCM16**:

`16,000 samples/s × 2 bytes/sample = 32,000 bytes/s = 256 kbit/s`

| Buffer | Approximate duration |
|---:|---:|
| 512 B | 16 ms |
| 1,024 B | 32 ms |
| 2,048 B | 64 ms |
| 4,096 B | 128 ms |
| 8,192 B | 256 ms |
| 16,384 B | 512 ms |

If retaining one 32-bit container per mono sample instead, the rate is **64,000 bytes/s**, and a 16 KiB buffer holds about **256 ms**. Full stereo-frame storage doubles that rate; actual storage depends on ESP-IDF DMA packing.

### 10. Validation

1. Record at 16 kHz, using the configured mono channel.
2. Inspect sample values, signs, and whether the expected channel contains audio.
3. Confirm that no extra padding bits are interpreted as signal.
4. Convert correctly decoded samples to PCM16.
5. Save a WAV file with a matching header (sample rate, mono, 16-bit PCM).
6. Listen for clipping, distortion, incorrect speed, and discontinuities.
7. Run long-duration capture and monitor buffer overruns and DMA errors.

### Datasheet reference

**InvenSense, INMP441: Omnidirectional Microphone with Bottom Port and I2S Digital Output**, document **DS-INMP441-00**, revision **1.1**, dated **2014-05-21**.

Relevant pages: **4–5** electrical/timing; **8** pinout; **10** startup/power; **11–12** I2S data format and channel timing; **13** internal filters.
