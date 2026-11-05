# Firmware modification

> ⚠️ Modifying flash can brick the device. Always keep a full known-good dump and
> a way to reflash (SPI programmer). Nothing here ships vendor binaries — you
> work from your own device's dump.

## Flash layout (16 MB NOR)

| Offset     | Content                                             |
|------------|-----------------------------------------------------|
| 0x00000    | U-Boot SPL + U-Boot 2013.07 (`checkenv`)            |
| 0x98000    | kernel `uImage` (lzo, `Linux-3.10.14-Archon`)      |
| 0x358000   | rootfs (lzop cpio ramdisk, `root=/dev/ram0`)       |
| 0x658000   | recovery (lzma, "Immortal")                        |
| ...        | config / system partitions                          |

## The `checkenv` rootfs gate (the key to booting a modified rootfs)

The SPL's `checkenv` stage validates the rootfs before boot. The critical,
non-obvious detail:

- The **first 4 bytes of the rootfs partition are the little-endian length of the
  compressed lzo stream** (e.g. `f4 39 2c 00` = `0x002c39f4`).
- `checkenv` reads that length, restores the lzop magic (`89 4c 5a 4f`), and
  decompresses exactly that many bytes.
- If the length does not match the actual stream, boot fails with
  `rootfs error:-1` and the device falls back to recovery.

So a repacked rootfs **must** rewrite those first 4 bytes to the LE length of the
*new* compressed stream. (Early attempts failed because they kept the original
length prefix on a longer stream.)

### lzop packing details

- Header 56 bytes, 256 KB (`0x40000`) blocks, `F_ADLER32_D` framing (big-endian
  adler32 of each decompressed block).
- Compress with liblzo2 `lzo1x_999` — `lzo1x_1` overflows the ~3 MB rootfs
  partition.

`build_image.py` in `../firmware/` does the repack: unpack the rootfs cpio, add
the wrapper + `.so` files, recompress with the correct block framing, write the
LE length prefix, pad, and re-embed at `0x358000`. It simulates `checkenv`'s
logic to self-verify before you flash.

## Injecting the RTSP tee

`sdkencode` is the single hardware/encoder owner. Rather than patch it, the
rootfs is modified to:

1. rename `/usr/bin/sdkencode` → `sdkencode.real`
2. install a wrapper (`sdkencode.wrap`) as `/usr/bin/sdkencode` that launches
   `sdkencode.real` with `LD_PRELOAD=/usr/lib/h264tee.so` (with a safety net that
   falls back to a clean exec if the preloaded instance dies within a few seconds)

`h264tee.so` interposes `IMP_Encoder_GetStream` and tees complete NAL units to
TCP. See `docs/NOTES.md` for the HEVC parameter-set caching and the uClibc
symbol-resolution quirks that make the hook work.
