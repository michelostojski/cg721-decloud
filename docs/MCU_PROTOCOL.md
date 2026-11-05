# CG721 MCU (Hi3861L) control protocol — `z_cmd`

Reverse-engineered map of how the stock firmware controls the Hi3861L's **MCU
side** — PIR, status LED, battery/charge, wake. Relevant because on the CG721
these are **not** T31 GPIOs; they live behind the MCU. Implementing motion-wake
or battery support on an open firmware (Thingino/OpenIPC) requires this.

> Status: the **`z_cmd` frame is fully decoded** (from the `reset_wifi_*` tools).
> The **routine command transport is the SDIO/HCC channel** (inferred, see below);
> the per-command opcodes beyond `reset` are best captured with live tooling.

## Two transports

The Hi3861L is a combined **WiFi + MCU** on the SDIO bus. The host reaches the MCU
two ways:

1. **SDIO / HCC channel (primary, routine).** `addx_mcu_daemon` — the battery/
   PIR/LED/wake manager — holds **only sockets** (D-Bus + netlink), no `/dev/i2c-2`
   or `/dev/ttyS0` fd. So its MCU traffic is multiplexed over the **hichannel.ko
   HCC layer** on SDIO, alongside WiFi data. (Confirmed: the daemon's `/proc/PID/fd`
   is sockets + console only; it uses `sendmsg`/`recvmsg`.)
2. **I2C-2 / UART side-channel (reset/recovery).** The `reset_wifi_i2c` /
   `reset_wifi_uart` tools talk to the MCU directly when SDIO is down:
   - **I2C-2** (`/dev/i2c-2`), MCU slave **`0x11`**, via `I2C_RDWR`; timeout 500, retries 10.
   - **UART0** (`/dev/ttyS0`) after a `wakeup_slave`.
   - Hard reset: pulse **GPIO 82 (PC18)**.

## The `z_cmd` frame (decoded from `reset_wifi_i2c`)

8-byte header, **big-endian on the wire** (each u16 sent high-byte first), optional
data payload:

```
┌─────────┬─────────┬───────────────┬─────────┬──────────┐
│ head    │ index   │ index_n       │ end     │ data…    │
│ 0xC309  │ <cmd>   │ ~index & FFFF │ 0x55AA  │ payload  │
│ u16 BE  │ u16 BE  │ u16 BE        │ u16 BE  │          │
└─────────┴─────────┴───────────────┴─────────┴──────────┘
```

- **`head` = `0xC309`**, **`end` = `0x55AA`** — framing magics, validated on the
  response (`invalid head` / `invalid index_n` errors otherwise).
- **`index`** = command id (`0x40xx` range).
- **`index_n` = one's-complement of `index`** (`~index & 0xFFFF`) — integrity check;
  the validator recomputes and compares it.
- Response echoes the same 8-byte framing; frame length field = `0x8`.

Send: build `[C309][index][~index][55AA][data]` big-endian → write to I2C `0x11` →
read 8-byte response → verify `head==C309 && end==55AA && resp_index_n==~resp_index`.

## Known / needed commands

| Command | Opcode (`index`) |
|---|---|
| `reset_wifi` | **`0x4005`** (confirmed) |
| `get_battery_status`, `get_new_battery_vol`, `set_new_battery_vol`, `set_battery_vol_cali`, `get_battery_pack_{sn,model,label,testpoint_adc}` | `0x40xx` (TBD) |
| `get_charge_info`, `get_charge_pin_level`, `set_charge_auto_poweron` | `0x40xx` (TBD) |
| `get_pir_info`, `set_pir_info`, `set_pir_switch`, `set_pir_sensivity` | `0x40xx` (TBD) |
| `set_led_ctrl`, `set_led_state` | `0x40xx` (TBD) |
| `get_wakeup_info`, `set_wakeup_info` | `0x40xx` (TBD) |
| `get_wifi_scanf_info` | `0x40xx` (TBD) |

(Method names from `addx_mcu_daemon`; each calls `hi3861l_api_set_cmd` → `hi3861l_get_ops_handle`.)

## Getting the remaining opcodes

Static extraction from `addx_mcu_daemon` is impractical: it's linked at `0x40xxxx`
(so `0x40xx` constants are mostly address low-halves), and the opcode reaches
`set_cmd` through an **indirect** `ops` pointer, not a clean immediate. Capture
them live instead:

- **The daemon can't be restarted to attach a sniffer** — killing it triggers the
  MCU power watchdog and the camera reboots. To preload a hook it must be added to
  the `rcS` launch line (`addx_mcu_daemon &`) in a **firmware rebuild**, so the
  daemon comes up hooked from boot.
- Because routine traffic is **SDIO/HCC, not I2C**, an `ioctl(I2C_RDWR)` hook (see
  `tools/i2clog` approach) captures nothing from the daemon. Hook **`sendmsg`/
  `recvmsg`** on the netlink socket instead, and decode the HCC wrapping around the
  `z_cmd` frame — or put a logic analyzer on the SDIO bus.

## Implication for an open firmware

Adding PIR-wake / battery / status-LED to Thingino on the CG721 means extending
**`wifi-hi3881`'s HCC layer** to carry these MCU commands (the `z_cmd` frame above)
over the SDIO channel — not wiring GPIOs. The I2C-2/UART path (slave `0x11`, same
frame) is a simpler alternative entry point if the driver exposes it, and is the
one fully decoded here.

## Provenance

- Frame/magics/I2C address: disassembly of `reset_wifi_i2c` (`llvm-objdump`) — the
  response validator at the frame-builder reveals `head=0xC309`, `end=0x55AA`,
  `index_n=~index`; `main` passes `0x4005` for reset; `i2c_msg.addr=0x11`.
- Transport split: `addx_mcu_daemon` `/proc/PID/fd` (sockets only) + its method
  symbols; `reset_wifi_{i2c,uart}` device/ioctl use; `ircut_ctrl.sh` for the
  (separate) direct IR GPIOs.
