#!/bin/sh
# Cross-compile the camera-side shared objects for the Ingenic T31Z (MIPS32r2,
# little-endian, o32 ABI, uClibc 0.9.33.2).
#
# Requirements:
#   - clang (>= 15) with the MIPS target
#   - lld   (fuse-ld=lld)
#   - A copy of the camera's own uClibc libraries in ./camlib/, pulled from the
#     device rootfs (libc.so.0 -> libuClibc-0.9.33.2.so). The loaders below are
#     linked against the DEVICE's libc so their imported symbol set matches
#     exactly what the device exports. Do NOT substitute a generic uClibc.
#
# Put these in ./camlib/ (from /lib on the camera):
#   libuClibc-0.9.33.2.so  libdl-0.9.33.2.so  libpthread-0.9.33.2.so
#
set -e
LIB=./camlib
COMMON="--target=mipsel-linux-gnu -march=mips32r2 -mabi=32 -msoft-float \
        -fPIC -shared -Os -fno-builtin -nostdlib \
        -Wl,--hash-style=sysv -fuse-ld=lld"

# --- mqttpub.so : minimal MQTT 3.1.1 publisher (thumbnail -> broker) ---
clang $COMMON -Wl,-soname,mqttpub.so -o mqttpub.so mqttpub.c $LIB/libuClibc-0.9.33.2.so

# --- clipup.so : minimal HTTP/1.0 uploader (clip -> LAN receiver) ---
# NOTE: imports ONLY the symbol set mqttpub uses (open/read/write/close/
# socket/connect/getenv/atoi/_exit). No memcpy, no lseek -- see docs/NOTES.md.
clang $COMMON -Wl,-soname,clipup.so -o clipup.so clipup.c $LIB/libuClibc-0.9.33.2.so

# --- h264tee.so : LD_PRELOAD RTSP tee, hooks IMP_Encoder_GetStream ---
# Needs libdl (dlopen fallback) + libpthread (accept thread).
clang $COMMON -Wl,-soname,h264tee.so -o h264tee.so h264tee.c \
      $LIB/libuClibc-0.9.33.2.so $LIB/libdl-0.9.33.2.so $LIB/libpthread-0.9.33.2.so

echo "built: mqttpub.so clipup.so h264tee.so"
echo "verify imports (should resolve against the device libc):"
for so in mqttpub.so clipup.so; do
  echo "  $so:"; llvm-readelf --dyn-syms $so 2>/dev/null | awk '/UND/{print "    "$NF}' | grep -v '^    $'
done
