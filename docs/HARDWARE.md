# CG721 hardware reference (reverse-engineered)

Board-level facts for the **CG721** (VicoHome) camera, extracted from its stock
firmware dump, kernel module disassembly, and a live `dmesg`/`cpuinfo`. Collected
to support an open WiFi driver + Thingino board port. Every value below is
confirmed from the device, not assumed.

## SoC

| | |
|---|---|
| Family | Ingenic **T31** (Xburst), QFN package |
| SKU | **T31ZL** (see naming note) |
| Kernel `system type` | `Indus_T31_QFN` |
| Ingenic codename | Zeratul |
| Kernel | Linux `3.10.14-Archon` |
| Flash | 16 MB NOR (SPI, `jz_sfc`) |
| Sensor | GalaxyCore **GC2063** (1080p) |

**Naming note.** The firmware carries two threads: the Ingenic SDK/toolchain says
`t31z` (`/opt/t31z/gcc_472`, `sdk/t31z/Zeratul_Release_20220505`,
`CAMERA_SOC_T31Z_gc2063`), which is Ingenic's T31Z-**series** shorthand (covers
ZL/ZC/ZX); the ODM product/OTA line says **`T31ZL-Hi3861L`** (build path, SDK
path, and OTA bucket `…cloudfront.net/T31ZL-Hi3861L/`). Combined with the OTA id
and the fact OpenIPC shipped "T31ZL Lite" for this unit, the silicon is a
**T31ZL**. The U-Boot is generic (`Board: ISVP (Ingenic XBurst T31 SoC)`, prompt
`isvp_t31#`) and exposes no SKU string; the only more-authoritative source would
be a live efuse read, but every firmware artifact agrees.

## WiFi (HiSilicon Hi3861L over SDIO)

| Item | Value | Source |
|---|---|---|
| WiFi chip | HiSilicon **Hi3861L** (WiFi + MCU) | firmware / OTA id |
| Stock driver | `hichannel.ko` (HiSilicon SDIO stack) | rootfs `/lib/modules/3.10.14-Archon/` |
| Driver family | same as `themactep/wifi-hi3881` | matching symbols: `oal_sdio_*`, `hcc_task_rx_gpio_thread`, `oal_gpio_isr` |
| **WLAN host-wake / RX interrupt GPIO** | **43** (`PB11`) | `hichannel.ko` disasm (below) |
| **WiFi reset / enable GPIO** | **82** (`PC18`) | `reset_wifi_i2c` / `reset_wifi_uart` (sysfs `gpio82`) |
| **WiFi SDIO host** | **`jzmmc_v1.2.1` (MSC1)** — non-removable, keep-power, no card-detect | `dmesg` |
| SD card host | `jzmmc_v1.2.0` (MSC0) — removable, card-detect | `dmesg` ("card inserted") |
| MCU side-channel | I2C-2 (`/dev/i2c-2`) and UART0 (`/dev/ttyS0`), `z_cmd` protocol; reset pulses GPIO 82 | `reset_wifi_*` |
| Module load | `insmod hichannel.ko` with **no args** → GPIO compiled in | `/usr/bin/insmod_wifi` |
| Boot order | `insmod_mmc` (mmc_core → jzmmc → mmc_block) → `insmod_wifi` | `/etc/init.d/rcS` |

### WLAN IRQ GPIO — disassembly evidence

`insmod_wifi` passes no parameters, so the interrupt GPIO is baked into the
module. The init routine in `hichannel.ko` (MIPS) does:

```
gpio_request_one($a0 = 0x2b, 1, "<label>")   ; 0x2b = 43
__gpio_to_irq(0x2b)                           ; 43 -> irq
request_threaded_irq(irq, oal_gpio_isr, 0, 0x4001, ...)
```

So the host-wake/RX line is **GPIO 43 (PB11)** — which also matches the upstream
driver's default (`hi3861l_irq_gpio = 43`).

### GPIO numbering

Ingenic T31 ports are 32 pins each: PA 0–31, PB 32–63, PC 64–95, PD 96–127.
- GPIO 43 → PB11 (WLAN IRQ)
- GPIO 82 → PC18 (WiFi reset)

## Thingino port implications

- **Board folder:** `configs/cameras/vicohome_cg721_t31zl_hi3881/` (defconfig + DTS).
- **DTS:** configure **msc1** as the SDIO WiFi host (`non-removable`,
  `keep-power-in-suspend`, no `cd-gpios`), msc0 as the SD slot; WLAN IRQ on PB11,
  WiFi reset on PC18; sensor GC2063.
- **Driver:** use `themactep/wifi-hi3881` (active, GPL-2.0). `jake5253`'s
  `thingino_wifi_hi3861l` is the same HiSilicon stack with `CFG_T31` adaptation
  but never built clean against Thingino kernels — reference only.
- **No host-side firmware blob:** the Hi3861L runs its own resident firmware; the
  host driver has no `request_firmware`/`.bin` load.

## Boot / validation gate

On-device boot test: SDIO enumerate on MSC1 → chip firmware up → `wlan0` appears
→ `scan` → `associate`. The CG721 unit is available for this testing.

## Provenance

- Stock rootfs (`cpio` ramdisk): `/usr/bin/insmod_wifi`, `insmod_mmc`,
  `reset_wifi_i2c`, `reset_wifi_uart`, `/etc/init.d/rcS`,
  `/lib/modules/3.10.14-Archon/{hichannel,jzmmc}.ko`.
- `hichannel.ko` disassembly (`llvm-objdump`) for the IRQ GPIO constant.
- Live device: `cat /proc/cpuinfo`, `dmesg | grep jzmmc`.
- Bootloader (`boot_real.bin`) + full flash for the SoC/SKU strings.
