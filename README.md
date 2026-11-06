# CG721 de-cloud

Local, cloud-free firmware augmentation for the **CG721** battery IP camera
(Ingenic T31Z + GC2063, HiSilicon Hi3861L Wi-Fi, stock "addx"/VicoHome cloud),
integrated into **Home Assistant** — with live RTSP, an always-available
"last view" tile, and automatic event-clip archiving, all on the LAN.

> **Status:** working on one unit, shared as a reference/draft. Flashing a camera
> can brick it. Read [`docs/FIRMWARE.md`](docs/FIRMWARE.md) and keep a full flash
> dump + an SPI programmer before you touch anything. No vendor binaries or
> firmware images are included — you work from your own device's dump.

## What it does

- **Keeps the stock firmware and the closed Wi-Fi blob** (augment, not replace).
  OpenIPC/Thingino have no host driver for the Hi3861L SDIO Wi-Fi.
- **Local RTSP** — an `LD_PRELOAD` tee on the IMP encoder exposes H.265 on TCP
  `:8900` (1080p main) / `:8901` (sub); go2rtc/Frigate transcode to H.264 for
  browser live view.
- **Always-available last-view tile** — the camera publishes its newest event
  thumbnail (retained) to MQTT on each wake → `camera.cg721` in HA, shown even
  while the camera sleeps (like the Vico app).
- **Automatic event clips in HA** — the camera pushes the `.mp4` clips the stock
  firmware already records to a tiny LAN receiver that drops them into HA's
  Media browser. Battery-friendly: push-on-wake, retry-until-confirmed, no event
  skipped.
- **No cloud** — the addx cloud can be firewalled off entirely.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the data-flow diagram.

## Repo layout

```
camera/            code that runs on the camera
  clipup.c           HTTP/1.0 clip uploader (LD_PRELOAD constructor .so)
  mqttpub.c          MQTT 3.1.1 thumbnail publisher (LD_PRELOAD constructor .so)
  h264tee.c          RTSP tee: hooks IMP_Encoder_GetStream -> TCP :8900/:8901
  app.sh             wake hook: thumbnail -> MQTT, clips -> receiver
  build.sh           clang/lld cross-compile (MIPS32r2, uClibc) — see header
host/              code that runs on your always-on host / HA
  clip_receiver.py   standalone upload sink -> writes clips to a media folder
  addon/cg721_receiver/   the same receiver packaged as a HAOS add-on
homeassistant/     HA configuration
  mqtt_camera.yaml   the last-view MQTT camera
  dashboard_card.yaml  Lovelace card (last view + Live + Clips)
  network_storage.md   surfacing clips in HA (HAOS-in-a-VM / NFS, or add-on)
firmware/          repack tooling (operates on YOUR dump; no vendor blobs)
  build_image.py     rootfs repack honoring the checkenv length-prefix gate
  sdkencode.wrap     wrapper that LD_PRELOADs the RTSP tee into sdkencode
docs/              ARCHITECTURE.md · FIRMWARE.md · HARDWARE.md · MCU_PROTOCOL.md · WIFI_BRINGUP.md · NOTES.md
```

## Quick start

### 1. Build the camera loaders
Pull your camera's `libuClibc-0.9.33.2.so`, `libdl-*.so`, `libpthread-*.so` into
`camera/camlib/`, then:
```sh
cd camera && ./build.sh      # -> mqttpub.so clipup.so h264tee.so
```

### 2. Firmware (RTSP tee)
Follow [`docs/FIRMWARE.md`](docs/FIRMWARE.md): repack the rootfs with
`firmware/build_image.py` to add `h264tee.so` + the `sdkencode` wrapper, honoring
the `checkenv` length-prefix gate, and flash. Keep a recovery dump.

### 3. Host receiver
Run `host/clip_receiver.py` (or the HAOS add-on in `host/addon/`) on an always-on
machine; see [`homeassistant/network_storage.md`](homeassistant/network_storage.md)
to surface its folder under HA's Media.

### 4. Camera runtime (no reflash needed for this part)
Place `mqttpub.so` and `clipup.so` on `/config` (NOR flash — **not** the SD, which
power-manages), install `camera/app.sh` as `/config/app.sh`, and set the
`MQTT=` / `RECV=` IPs and the `MQTT_PASS` (redacted to `CHANGE_ME` here) inside it.

### 5. Home Assistant
Add `mqtt_camera.yaml`, mount the clips folder (`network_storage.md`), and add
`dashboard_card.yaml`.

## Configuration you must set

- `camera/app.sh`: `MQTT=` (broker IP), `RECV=` (receiver IP), `MQTT_USER` /
  **`MQTT_PASS`** (the password is redacted to `CHANGE_ME` — set your own).
- IP addresses throughout are examples (`192.168.1.50` host, `.52` HA) — adjust.

## Credits & disclaimer

Independent interoperability/repair work on hardware the author owns. "VicoHome",
"addx", "Ingenic", "HiSilicon" and other names are trademarks of their respective
owners; no affiliation or endorsement is implied. Provided **as-is**, no warranty
— see [`LICENSE`](LICENSE). You are responsible for your own device and local
laws (e.g. recording consent).
