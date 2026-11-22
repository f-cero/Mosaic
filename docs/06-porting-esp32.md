# Porting to ESP32 / building a console

## 1. Flash layout (default 4 MB cart)
| Region | Offset | Contents |
|---|---|---|
| bootloader | 0x000000 | ESP-IDF |
| partition table | 0x008000 | `ports/esp32/partitions.csv` |
| app | 0x010000 | firmware (core + port) |
| mosaic | 0x200000 | `game.mrom` payload (32-byte MOSROM01 header + cart), 4 MB max |

`mosaic rom game.mcart` emits the payload; flash it with
`esptool.py write_flash 0x200000 game.mrom`.

## 2. Port surface (`ports/esp32/`)
* `ms_port_esp32.c` — framebuffer push (`esp_lcd`), I2S/PWM audio sink,
  buttons (GPIO/ADC), SD mount, Wi-Fi/BLE manager, SPI link.
* `device.conf` on SD selects the topology, e.g.
  ```
  name=starfall-handheld
  chip=esp32s3
  wifi=1
  bt=1
  sd=1
  node main role=core cs=0
  node lcd role=ppu cs=10
  node dac role=audio cs=11
  ```
* The core calls `ms_core_run_frame` from a 60 Hz task pinned to one core;
  audio is pulled from a ring buffer filled by the same frame (no dynamic
  allocation).

## 3. Wi-Fi / BLE policy
Game-facing calls are non-blocking: `ms_net_poll()` drives the state machine;
`ms_net_fetch()` returns `MS_NET_BUSY` until complete. Connectivity is always
optional — a cart must boot and play with Wi-Fi and BLE disabled. Scans are
rate-limited; credentials live in NVS, never in the cart.

## 4. Memory budget
| Item | Bytes |
|---|---|
| Framebuffer 320×240×2 | 153,600 (PSRAM recommended) |
| QFA framebuffer | 76,800 less scratch 38,400 |
| PPU state | ~6 KB |
| Synth (16 voices) | ~2 KB |
| VM + entities | ~4 KB |
| Cart | flash-mapped, zero RAM copies |

Use QFA on boards without PSRAM; use external RAM for the scanout buffer on
the CYD and keep the engine state in internal RAM.
