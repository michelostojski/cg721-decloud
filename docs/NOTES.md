# Notes & gotchas

Hard-won details from building this. Most cost hours; collected here so they
don't have to again.

## Writing shared objects for the device's uClibc

The `.so` loaders are built freestanding (`-nostdlib -fno-builtin`) and linked
against the **device's own** `libuClibc-0.9.33.2.so` (pulled from the rootfs),
so their imported symbols exactly match what the device exports.

- **`LD_PRELOAD` without a `main`.** The loaders are run as constructors
  (`__attribute__((constructor))`) via `LD_PRELOAD=x.so busybox true`, so code
  runs with no crt/`_start`. The constructor does its work and calls `_exit()`.
- **`RTLD_NEXT` returns `NULL` under this uClibc.** The RTSP hook can't resolve
  the real `IMP_Encoder_GetStream` via `dlsym(RTLD_NEXT, …)`; it falls back to
  `dlopen("/usr/lib/libimp.so", RTLD_NOW|RTLD_GLOBAL)` with a self-recursion
  guard.
- **Keep the imported symbol set minimal.** `clipup.so` originally imported
  `memcpy` (clang emitted a call for variable-length copies) and `lseek`; both
  were avoided (`-fno-builtin` + hand-rolled byte copies; file size via a
  read-to-count pass instead of `lseek`) so its import set is a strict subset of
  the known-good `mqttpub.so`. This made a real difference while debugging a
  `can't load library` error — though the root cause turned out to be the SD
  card (below), minimal imports remove a whole class of doubt.
- **MIPS o32 constants differ from x86.** `SOCK_STREAM = 2` (not 1),
  `SOL_SOCKET = 0xffff`, `SO_REUSEADDR = 0x0004`, `SO_SNDTIMEO = 0x1005`,
  `MSG_NOSIGNAL = 0x4000`; `O_WRONLY = 1`, `O_CREAT = 0x100`.

## The SD card power-management trap (battery camera)

This one masqueraded as everything else. On a battery camera the SD card is
**power-managed** — unmounted/spun down when idle — so `/tmp/sdcard` blinks in
and out:

- A library `LD_PRELOAD`-ed from the SD intermittently fails with
  `busybox: can't load library '…'` — not because the ELF is bad (it was
  byte-perfect, md5-verified) but because the card was down at load time.
- `md5sum` on a file can report `No such file or directory` seconds after that
  same file loaded successfully.

**Fixes:**
1. Keep the loaders (`mqttpub.so`, `clipup.so`) on `/config` (NOR overlay,
   always mounted), not on the SD.
2. In `app.sh`, **wait for the SD to mount** (poll for `DCIM`, ~15 s) before
   touching clips — it typically comes up ~1 s into a wake.
3. The wake hook can fire *before* the SD is mounted; without the wait it bailed
   out immediately with an empty `DCIM`.

## HEVC tee: deliver complete NALs + cache parameter sets

Playing the teed H.265 reliably required:

- **Complete-NAL blocking sends** with `SO_SNDTIMEO` (0.5 s). Non-blocking
  single `send()`s dropped bytes mid-NAL → decoder RPS errors
  ("Could not find ref…").
- **Per-type VPS/SPS/PPS caching.** The sub stream emits one NAL per access unit
  (`pc=1`), so VPS (type 32) / SPS (33) / PPS (34) arrive in separate AUs. Cache
  each type in its own slot and prepend all three at every IDR (19/20/21), or a
  late joiner never gets dimensions ("unspecified size").

## Other

- `pthread_create` needs a real `pthread_t*` output pointer — passing `NULL`
  NULL-derefs and crashes `sdkencode` after the first `GetStream`.
- busybox on the device lacks `head`, `sed`, `wget`, `httpd`, `curl`, `pidof`,
  `grep -E`, `df`. Shell loops (`for … break` for "newest file") and tiny
  `LD_PRELOAD` constructors replace them.
- A single-quote the MQTT password: `MQTT_PASS='...#...'` — an unquoted `#`
  starts a comment.
- The OpenHarmony `hi3861v100` repo is **chip-side** firmware, not the Linux
  host SDIO driver — it can't build `hichannel.ko`. Keep the stock Wi-Fi blob.
