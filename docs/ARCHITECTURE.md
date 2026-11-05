# Architecture

## The camera

- **SoC:** Ingenic T31Z ("Zeratul"), MIPS32r2 little-endian, o32 ABI, uClibc 0.9.33.2
- **Sensor:** GalaxyCore GC2063 (1080p)
- **Wi-Fi/MCU:** HiSilicon Hi3861L over SDIO (closed host driver `hichannel.ko`)
- **Encoder:** H.265/HEVC (Ingenic IMP SDK, `libimp.so`)
- **Flash:** 16 MB NOR
- **Stock cloud:** "addx" (VicoHome app)
- **Firmware:** `CAMERA_SOC_T31Z_gc2063_4.7.2_V1`, OTA id `T31ZL-Hi3861L`

The de-cloud **augments** the stock firmware rather than replacing it
(OpenIPC/Thingino have no host driver for the Hi3861L SDIO Wi-Fi), so the closed
Wi-Fi blob keeps working.

## Data flow

```
                      ┌──────────────────────── CAMERA (T31Z) ───────────────────────┐
                      │                                                               │
  sensor ─► IMP ─► H.265 encoder ─► sdkencode ─┬─► (stock) SD event clips + thumbs    │
                                               │                                      │
                      h264tee.so (LD_PRELOAD)  └─► tees encoder output to TCP          │
                      hooks IMP_Encoder_GetStream    :8900 main (1080p) / :8901 sub    │
                                               │                                      │
                      on each motion wake, /config/app.sh:                            │
                        • mqttpub.so  ─ thumbnail ─► MQTT  cg721/snapshot (retained)   │
                        • clipup.so   ─ clip+thumb ─► HTTP  receiver:8099              │
                      └───────────────────────────────────────────────────────────────┘
                                   │ Wi-Fi (local LAN only, cloud blocked)
          ┌────────────────────────┼─────────────────────────────────────────┐
          ▼                        ▼                                          ▼
     Mosquitto                clip_receiver.py                          go2rtc / Frigate
   cg721/snapshot            writes /media/cg721                     RTSP :8900 ─► H.264
          │                        │                                          │
          ▼                        ▼                                          ▼
   camera.cg721            Media → cg721                             camera.cg721 live
  (last-view tile)        (browsable clips)                          (when awake)
```

## Three independent local paths

1. **Live RTSP** — `h264tee.so` is `LD_PRELOAD`-ed into `sdkencode` and tees the
   H.265 stream the hardware encoder produces to TCP `:8900` (main) and `:8901`
   (sub). go2rtc/Frigate read it as RTSP and transcode to H.264 for browser
   live view. Only available while the camera is awake.

2. **Last-view thumbnail** — on each wake `app.sh` runs `mqttpub.so`, a tiny
   MQTT 3.1.1 publisher that posts the newest event thumbnail (retained) to
   `cg721/snapshot`. HA's `mqtt: camera:` shows it as an always-available tile.

3. **Event clips** — `app.sh` runs `clipup.so`, a tiny HTTP/1.0 uploader, to push
   every not-yet-sent SD clip (+ thumbnail) to `clip_receiver.py`, which drops
   them into HA's media folder. Harvests the clips the **stock** firmware already
   records, instead of fighting the live stream for a battery camera.

Everything is local; the addx cloud can be firewalled off entirely.

## Why "push" for a battery camera

The camera deep-sleeps and only wakes on PIR. There is no always-on server to
poll, and the wake window is short. So the camera is the active party: during
its wake it pushes the thumbnail and clips out. The receiver is a trivial sink.
`app.sh` marks a clip done only on a confirmed `rc=0`, so uploads retry across
wakes and nothing is lost.
