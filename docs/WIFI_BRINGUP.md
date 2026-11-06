# CG721 Wi-Fi bring-up on Thingino — Hi3861L SDIO enum timeout

How the Hi3861L Wi-Fi was brought up on an open **Thingino** build for the CG721,
and the one driver gap that blocks it on the stock `wifi-hi3881` package. Written
so the next person with a **Hi3861L + T31** board doesn't have to re-derive this.

> **Status:** the board port boots and the chip enumerates and reports firmware,
> but `wlan0` does **not** come up on stock `themactep/wifi-hi3881` — the driver is
> built interrupt-blind for this board. The fix is a driver patch (in progress,
> see [The fix](#the-fix)); this doc is the diagnosis that drove it.

## TL;DR

- The CG721 routes the Hi3861L host-wake / RX interrupt to **GPIO 43 (PB11)**, and
  pulses **GPIO 82 (PC18)** for chip reset. The stock firmware's `hichannel.ko`
  uses that GPIO interrupt.
- `themactep/wifi-hi3881` (the driver Thingino ships) is built with
  `-D_PRE_FEATURE_NO_GPIO`, which compiles the GPIO interrupt path to a **no-op**
  (in-band SDIO IRQ only). On this board DAT1 isn't wired for in-band SDIO
  interrupts, so the chip's "enum done" notification never reaches the host →
  `sdio enum timeout` → no `wlan0`.
- Fix = a GPIO-interrupt build of the driver (`wlan_irq_gpio=43`, reset on 82),
  not a DTS or module-param change — the GPIO layer was stripped from this driver
  drop and has to be put back.

## The board

Confirmed live (see [Provenance](#provenance)):

| | |
|---|---|
| SoC | Ingenic **T31ZL** (`t31zl`, efuse `0x10031003`) |
| Kernel | `3.10.14__isvp_swan_1.0__` |
| Sensor | GalaxyCore **GC2063** (`chip_id 0x2053`, i2c-0 @ `0x37`) |
| Wi-Fi SDIO host | **`jzmmc_v1.2.1` (MSC1)** — non-removable, `card insert via sysfs` |
| SD host | `jzmmc_v1.2.0` (MSC0) — `gpio-59` card-detect |
| Wi-Fi host-wake IRQ | **GPIO 43 (PB11)** |
| Wi-Fi reset | **GPIO 82 (PC18)** |
| Wi-Fi chip fw | `Hi3881V100R001C00SPC021` (2020-10-14) |

## The symptom

`hi3881` autoloads at boot; every driver init stage succeeds, then the probe
times out at a **dead-precise 10.000 s**:

```
[ 8.161097] jzmmc_v1.2 jzmmc_v1.2.1: card insert via sysfs
[ 8.471103] [VERSION]:Hi3881V100R001C00SPC021 2020-10-14 20:25:00
[ 8.476749] oam_main_init SUCCESSFULLY!
[ 8.476776] wal_customize_init SUCCESSFULLY!
[ 8.476818] oal_main_init SUCCESSFULLY
[ 8.482585] frw_main_init SUCCESSFULLY!
[ 8.482614] hi_wifi_plat_init SUCCESSFULLY
[18.482686] [ERROR]:oal_sdio_host.c:oal_sdio_func_probe:2257:sdio enum timeout, reason[probe timeout]
[18.483443] [ERROR]:1646:hcc_host_init:: sdio_probe failed!
```

Read it carefully:

- **The bus works.** The chip version prints over SDIO CMD52/53 — those are
  *polled*, they need no interrupt. So enumeration of the card itself is fine.
- **All driver-side init succeeds** (`oam`/`wal`/`oal`/`frw`/`plat` all
  `SUCCESSFULLY`). Nothing is misconfigured on the host side.
- **The 10.000 s gap is a fixed timeout, not a flaky transfer.** `oal_sdio_func_probe`
  arms a completion and waits for the chip's device→host "enum ready" notification.
  That notification is delivered by the **Wi-Fi interrupt**. The completion never
  fires → the timeout expires to the microsecond. Classic dead interrupt line.

Then it **oopses in cleanup** — a second, independent bug:

```
[18.483477] CPU 0 Unable to handle kernel paging request at virtual address 0000000c, epc == 80352d9c
[18.585188] Cause : 40808008
[18.586...] BadVA : 0000000c
```

`BadVA 0x0000000c` = NULL + 0xc. On the failed probe the teardown path runs
`oal_unregister_gpio_intr` → `oal_free_irq()` on an IRQ struct that was never
allocated (because the GPIO path is compiled out), dereferencing offset 12 of a
NULL pointer. Deterministic — identical `epc 80352d9c` / `BadVA 0000000c` across
every boot.

## Root cause

The CG721's **stock** `hichannel.ko` registers a real GPIO interrupt:

```
gpio_request_one(0x2b = 43, ...)                     ; PB11 host-wake
request_threaded_irq(__gpio_to_irq(43), oal_gpio_isr, 0, 0x4001, ...)
```

(`oal_gpio_isr` is a symbol in the stock module.) So **this board signals the host
over GPIO 43**, not over an in-band SDIO interrupt.

`themactep/wifi-hi3881` is built with **`-D_PRE_FEATURE_NO_GPIO`**
(`env_config.mk:170`). Under that define:

- `oal_sdio_interrupt_register()` compiles to a **no-op** — the module never arms
  any host-wake interrupt. It is deaf to GPIO 43 by construction. No module
  parameter changes this.
- `board_power_on()` is also compiled out — no GPIO 82 reset at probe.
- `oal_register_gpio_intr` has only a **call site, no implementation** left in this
  drop (only `oal_unregister_gpio_intr` survives — hence the teardown oops). So you
  cannot just flip the define: a GPIO-enabled build won't link until the function
  is restored. The struct fields are all still present (`ul_wlan_irq`,
  `gpio_rx_tsk`, `gpio_int_count`), so the patch slots back in cleanly.

Why not in-band SDIO interrupts instead? `jzmmc_v1.2` *does* support them
(`jzmmc_enable_sdio_irq` is present), but this board doesn't wire the DAT1 line
that in-band mode needs — so GPIO 43 is the only path. Non-`NO_GPIO` builds in
fact default `g_hisdio_intr_mode = 1` (GPIO host-wake); the SDIO-in-band flavor
Thingino ships is the special case, not the norm.

The live GPIO table confirms the pins are free for a patched driver to claim —
nothing holds 43 or 82 (GPIO-C is empty):

```
GPIOs 0-31  (A):  gpio-18 sensor_reset (out hi)
GPIOs 32-63 (B):  gpio-38/52/53 sysfs (IR/ircut),  gpio-59 mmc_detect (in)
GPIOs 64-95 (C):  (none)
```

## The fix

A **GPIO-interrupt build** of `wifi-hi3881` (not a DTS change — T31 has no per-board
DTS in Thingino, and not a module-param toggle — the code is absent). The patch, as
scoped with the driver maintainer:

1. Re-implement `oal_register_gpio_intr` — `gpio_request_one` + `request_threaded_irq`
   on `gpio_to_irq(wlan_irq_gpio)`, feeding the completion `oal_sdio_func_probe`
   waits on. New `module_param wlan_irq_gpio`, default **43**.
2. Implement `board_power_on/off` — drive **GPIO 82 (PC18)** for chip reset/enable.
3. Guard the teardown so a failed probe can't `oal_free_irq()` a never-registered
   IRQ (fixes the NULL+0xc oops independently).
4. Build with the GPIO path enabled (`g_hisdio_intr_mode = 1`), i.e. drop
   `_PRE_FEATURE_NO_GPIO` for this board.

No host-side firmware blob is involved — the Hi3861L runs its own resident
firmware; the host driver does no `request_firmware`.

### Validating the patched module

```sh
# drop the patched hi3881.ko on the camera, then:
rmmod hi3881 2>/dev/null
insmod /path/to/hi3881.ko          # or: modprobe hi3881 (if installed)
dmesg | grep -iE 'sdio|hi38|hcc|enum|wlan|gpio'
ip link                            # PASS = wlan0 present
```

Pass criteria, in order: no `enum timeout` (probe completes), `wlan0` appears in
`ip link`, then `wpa_cli -i wlan0 scan` returns results. The stock firmware's
`z_cmd` MCU features (PIR/battery/LED) are a separate layer — see
[`MCU_PROTOCOL.md`](MCU_PROTOCOL.md).

## Provenance

- On-device Thingino runtime diagnostic (booted CG721): SoC efuse, live `dmesg`,
  claimed-GPIO table, `lsmod`, sensor info, `thingino.json`, U-Boot env.
- Stock `hichannel.ko` disassembly (`llvm-objdump`) for the GPIO-43 IRQ constant
  and `oal_gpio_isr`.
- `themactep/wifi-hi3881` source dig (`env_config.mk:170` `_PRE_FEATURE_NO_GPIO`;
  `oal_sdio_host.c` probe/teardown; missing `oal_register_gpio_intr` impl) — with
  thanks to the driver maintainer for the source-level confirmation.

_Last updated: 2026-11-06._
