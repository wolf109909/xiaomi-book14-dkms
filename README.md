# xiaomi-book14 — platform driver for Xiaomi Book Pro 14 2026

Out-of-tree kernel module fixing this laptop's broken WMI/ACPI platform
support on Linux: **suspend, fan readings, power profiles, and the dedicated
settings key**.

Supported hardware: **Xiaomi Book Pro 14 2026** (Intel Core Ultra 5 358H,
Panther Lake, Bitland MIFS v2 firmware, DMI `product_name: Xiaomi Book Pro 14`).
Other MIFS-based Xiaomi laptops may partially work but are untested.

## What works with this module

| Feature | Without module | With module |
|---|---|---|
| Suspend | **aborts** (`bitland_mifs_wmi_suspend` returns `-EINVAL` → "Some devices failed to suspend") | works |
| Lid close/open | no events (screen stays on, lid-pressed keys type into lock screen) | full `SW_LID` events → logind blanks/suspends |
| Fan RPM | not exposed anywhere | `sensors` → hwmon `xiaomi_book14`, left+right fan |
| Power profiles | broken (ppd errors, bogus AC probe) | 4 profiles via `platform_profile`, each also sets RAPL PL1/PL2 |
| Settings key (top-right of keyboard) | dead | input device `Xiaomi Book 14 Settings Key` (`KEY_CONFIG`) |
| CapsLock LED | never lights | follows CapsLock state, restored after suspend |

## Why mainline fails

1. **Firmware defect**: `ECON` is referenced by `\_SB.PC00.LPCB.Q_EC._STA`
   (and `H_EC` everywhere) but is **defined in none of the 34 loaded ACPI
   tables**. Boot log:
   `ACPI BIOS Error (bug): Could not resolve symbol [\_SB.PC00.LPCB.Q_EC._STA.ECON], AE_NOT_FOUND`.
   → `acpi_ec` never binds → every `_Qxx` handler (lid `_Q0C/_Q0D`, Fn+K
   `_Q24`, …) is dead.
2. **Mainline `bitland_mifs_wmi` targets different firmware.** This machine's
   `WMAA` (SSDT24, `WMID`/"MIFS") only implements `FUN2` ∈ {0x0800, 0x0A00,
   0x0C00, 0x1000}; perf-mode values are QFAN codes {2,3,4,9,10}, not 0..3.
   Mainline `platform_profile` GET returned `-EINVAL` and its **suspend hook
   aborted every suspend** (`dpm_run_callback(): bitland_mifs_wmi_suspend
   returns -22`). This package therefore **blacklists `bitland_mifs_wmi`**
   (and `redmi_wmi`).
3. **Intel DTT (`\_SB.IETM` = INTC10D4) has no Linux driver**, so the
   firmware's `Notify(IETM,0x88)` TDP path does nothing on Linux. TDP is set
   via RAPL MSRs instead.
4. **No fan RPM anywhere in this firmware's WMI** — fan tach lives in the EC
   shared-memory window (0xFE0B0300).

## What the module does

- Binds WMI method GUID `B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B` (WMAA), with
  correct status checking (`SGER==0x8000`).
- **platform_profile** (4 profiles): low_power→QFAN 0x02, balanced→0x03,
  balanced-performance→0x04, performance→0x09; each also programs **RAPL
  PL1/PL2 via MSR 0x610** (module params `pl1_*` in mW) so TDP actually
  changes — verified: 15 W caps CPU at ~1.3 GHz under load.
- **Lid**: polls EC window `LSTE` (0xFE0B0314 bit0, `poll_ms`, default
  250 ms) and evaluates the firmware's own `_Q0C`/`_Q0D` → `Notify(LID0,0x80)`
  → button driver emits `SW_LID` → logind blanks screen / suspends.
- **Fan tach**: hwmon `xiaomi_book14`, `fan1_input`@0x69, `fan2_input`@0x6B
  (LE16 RPM, 0 = stopped).
- **Perf-mode watch**: polls `QFAN` (0xFE0B0368); on change applies RAPL and
  calls `platform_profile_notify()`.
- **Settings key**: polls the XGNS mailbox (0x6FDB8000, `mbox_poll_ms`,
  default 50 ms) for EC hotkey events `EVFN=0x25` (press) / `0x26` (release)
  and reports `KEY_CONFIG` on a dedicated input device. Pair it with the
  sibling **xiaomi-settings-key** project (userspace daemon) to turn the key
  into a customizable launcher.
- **CapsLock LED**: this keyboard requires `i8042.nopnp=1 i8042.dumbkbd=1`;
  dumbkbd makes i8042 reject all writes to the keyboard, so atkbd's LED path
  is dead (no `input*::capslock` LED, no EV_LED capability). The LED is
  driven by the EC in response to the standard PS/2 Set LEDs command `0xED`
  (verified by poking `/dev/port`: `0xED 0x07` lights it; the EC also
  mirrors state in ERAM `0x16` bit6 "CPLK"). The module re-enables EV_LED on
  the keyboard device, listens for `LED_CAPSL` input events (vt/KWin
  inject them), and sends the `0xED` sequence itself. State is restored on
  resume. Side effect: each toggle logs two harmless `atkbd serio0:
  Spurious ACK` lines (the EC's ACKs have no waiter since dumbkbd disabled
  atkbd's command tracking).
- **Suspend/resume hooks never fail** (root cause of broken suspend removed).
- debugfs `/sys/kernel/debug/xiaomi-book14/qfan_mode`: raw QFAN get/set;
  `settings_key_test` (write-only): synthesizes a settings-key tap, for
  testing the key→daemon→command pipeline without the physical key;
  `ec_dump`: hex dump of the EC window; `ec_write`: write a byte into the
  EC window (`"0xOFF 0xVAL"`).

## Install (Arch / CachyOS)

```sh
git clone <this repo>
cd xiaomi-book14-dkms
makepkg -si
```

The package:
- installs the source to `/usr/src/xiaomi-book14-1.2.0` and registers it with
  **DKMS** (auto-rebuilds on every kernel update),
- **blacklists** the broken mainline drivers
  (`/usr/lib/modprobe.d/xiaomi-book14.conf`),
- **autoloads** the module at boot
  (`/usr/lib/modules-load.d/xiaomi-book14.conf`).

After install: `sudo modprobe xiaomi-book14` (or reboot).

## Install (other distros, manual DKMS)

```sh
sudo cp xiaomi-book14.c Makefile dkms.conf /usr/src/xiaomi-book14-1.2.0/
sudo dkms add xiaomi-book14/1.2.0
sudo dkms install xiaomi-book14/1.2.0
echo xiaomi-book14 | sudo tee /etc/modules-load.d/xiaomi-book14.conf
printf 'blacklist bitland_mifs_wmi\nblacklist redmi_wmi\n' | sudo tee /etc/modprobe.d/xiaomi-book14.conf
sudo modprobe xiaomi-book14
```

## Usage

```sh
sensors xiaomi_book14-*            # Left Fan / Right Fan RPM
cat /sys/class/platform-profile/platform-profile-*/profile
powerprofilesctl set power-saver|balanced|performance   # via power-profiles-daemon
```

Module parameters (defaults): `pl1_low_power=15000`, `pl1_balanced=35000`,
`pl1_balanced_perf=45000`, `pl1_performance=61000` (mW), `poll_ms=250`,
`mbox_poll_ms=50`.

## Uninstall

```sh
sudo pacman -R xiaomi-book14-dkms   # or: sudo dkms remove xiaomi-book14/1.2.0 --all
```


## License

GPL-2.0-or-later
