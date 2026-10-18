# TFT Emulator (ESP32 SPI slave + web streaming)

An ESP32 (classic) that emulates an **ST7735-style TFT panel** (default 80x160, RGB565).
It wires into a host device's display connector — SCK / *(SDA)* MOSI / CS / DC / RES — runs as
an **SPI slave**, parses the command stream the master would send to a real panel, and streams
the framebuffer to a web UI over WebSocket.

## Wiring (defaults, all configurable via `idf.py menuconfig` → TFT Emulator, or via NVS/web)

| Signal | GPIO | Note |
|--------|------|------|
| SCK    | 18   | host SPI clock |
| MOSI ("SDA") | 23 | host → ESP data |
| MISO   | 19   | not used by panels, driven by slave |
| CS     | 5    |
| DC     | 21   | sampled per SPI transaction |
| RES    | 4    | reset pulse clears panel state |
| BOOT   | 0    | hold ~1 s in station mode → force AP on next boot |

## Built-in test generator (no host required)

The firmware can generate its own ST7735 command/pixel stream with an internal SPI **master**
(SPI3 by default) whose pins you jumper back to the emulator's slave pins. The data then flows
through the real SPI slave + protocol parser, so it exercises the whole capture path.

Jumper wires (test pin -> emulator pin):

| Test master (default) | Emulator slave (default) | Signal |
|-----------------------|--------------------------|--------|
| GPIO 14 (test SCK)  | GPIO 18 (SCK)  | SPI clock |
| GPIO 13 (test MOSI) | GPIO 23 (SDA)  | data |
| GPIO 27 (test CS)   | GPIO 5  (CS)   | chip select |
| GPIO 26 (test DC)   | GPIO 21 (DC)   | data/command |
| GPIO 25 (test RES)  | GPIO 4  (RES)  | reset pulse (optional) |

Enable it from the web UI (**Test generator** panel): choose a pattern (rainbow, progress bar,
color bars, checkerboard, solid sweep, gradient), frame rate, and the master SPI clock, then
**Start**. FPS/pattern changes apply live; changing the clock restarts the generator.

Both peripherals run at once (SPI2 slave + SPI3 master), each using one DMA channel. The test
master and an external host must not drive the emulator pins at the same time. The generator
survives display-size changes: it re-reads the panel size and re-allocates its pattern buffer,
so resizing via the web UI keeps the stream running.

## Code interface (no GPIO)

`POST /api/inject` accepts a raw binary body (Content-Type `application/octet-stream`) and feeds
it through the same ST7735 parser as real SPI traffic:

- Query `?dc=0` — payload is treated as *commands* (CASET / RASET / RAMWR / MADCTL …)
- Default (no `?dc=`) — payload is treated as *pixel data* (DC=1), RGB565, byte order following
  the *pixel byte order* setting. Data longer than 8192 bytes per request is not supported —
  send large frames in consecutive requests.
- Concurrent use with the GPIO test generator is not allowed (stop the generator first).

`GET /api/fb` returns the raw framebuffer for verification (RAM row order, hi byte first,
`panel_w * panel_h * 2` bytes — dimensions from `/api/status`).

Example (Python):

```python
import requests
IP = "http://192.168.4.1"
w, h = 80, 160
def win():
    requests.post(IP + "/api/inject?dc=0", bytes(
        [0x2A, 0, 0, (w - 1) >> 8, (w - 1) & 0xFF,
         0x2B, 0, 0, (h - 1) >> 8, (h - 1) & 0xFF, 0x2C]))
frame = bytes(2 * w * h)                 # RGB565 big-endian framebuffer
win()
requests.post(IP + "/api/inject", frame) # DC=1 pixel data
```

The web UI *Code interface* panel embeds the same logic as a scriptable demo: it
evaluates a user script with an `emu` helper object (`begin/w/h`, `pixel`, `rect`, `clear`,
`flush`) that pushes the protocol stream over `/api/inject` at any frame rate.

## Web UI

The device serves a live canvas of the emulated display plus settings pages at `http://<ip>/`:

- Display: resolution, view rotation, pixel byte order, 2/4-byte windowing (CASET/RASET), extra 180° flip
- WiFi: connect to an existing network (STA), disconnect and go back to AP mode
- Access point: set custom AP SSID/password, or switch to AP mode immediately

Boot logic: if it cannot connect to the configured WiFi network (or none is set, or the
BOOT button is held at boot / `Switch to AP mode now` is pressed on the UI), it hosts an
access point (SSID auto-generated as `TFT-EMU-XXXXXX`, default password `tftemu123`).

Settings (shown in `/api/status`) are persisted in NVS.

## Known constraints

- **SPI slave on ESP32: use mode 1 (default).** With mode 3 (CPOL=1, clock idling high) the slave
  DMA phase counter treats the idle-high clock as an already-passed sample edge and starts one bit
  late: every captured burst is shifted left by one bit and the trailing bit/byte is lost. This
  makes protocol commands unrecognizable (RAMWR shifts to 0x58 and never matches). Mode 1 uses the
  same DMA path without the phase ambiguity. If a real host must use mode 3, set pixel-stable
  framing on the host side and verify with the `mis` counter.

- **SPI modes 1/3 required for DMA.** The ESP32 classic SPI slave DMA only works when clock
  phase is flipped. The firmware reads the configured mode from NVS; if DMA is disabled the
  slave has to re-queue a transaction per CS burst, which is much slower and limits throughput.
- **Burst lengths must be multiples of 4 bytes on ESP32.** When DMA is enabled, an SPI burst that
  ends on a non-word boundary loses its trailing bytes (the RX DMA partial word is flushed between
  transactions). The built-in test generator pads its window command and pixel stream to stay
  4-byte aligned; an external host with sub-word burst tails (e.g. a single-byte RAMWR) may lose
  those byte(s), which shows up in `proto_err` / the `not a multiple of 32` warning in the logs.
- **Maximum throughput.** Per-CS-burst transfer is limited by the transaction buffer size
  (default 40960 bytes ≈ one full 80x160 frame). A host that sends the *entire* previous
  frame within a single burst and expects more bytes than that would be truncated (counter
  visible in `/api/status.spi_full`).
- **Clock.** The slave is safe up to ~10 MHz; the protocol parser does not require any
  specific timing.
- **Window arguments.** If your host writes CASET/RASET as two 16-bit values (2 bytes each)
  set *window parameter width = 2 bytes* in the Display settings; the default expects
  16-bit big-endian values sent as 4 bytes per write.

## Build / flash

```
idf.py build
idf.py -p <port> flash monitor
```
Default console: 115200 baud.

The web UI is embedded (www/index.html) into the firmware image. If you modify it, rebuild.
