# xiaomi-book14-dkms

Out-of-tree DKMS platform driver for the **Xiaomi Book Pro 14 2026**
(Panther Lake, Bitland MIFS v2 firmware). Single C file, Arch/CachyOS
packaging, plus udev/systemd workarounds. README.md is the user-facing doc
and the record of reverse-engineering findings; keep it in sync with code.

The author also upstreams a `bitland-mifs-wmi` driver to
platform-driver-x86 derived from this module, so code here should stay
close to mainline style (checkpatch --strict clean, devm everywhere,
guard()/__free cleanup, no version #ifdefs beyond the 6.16 floor).

## Layout

| File | Role |
|---|---|
| `xiaomi-book14.c` | the whole driver (WMI driver bound to the MIFS WMAA GUID) |
| `Makefile` | kbuild wrapper; auto-selects `LLVM=1` for Clang-built kernels |
| `dkms.conf` | DKMS metadata; min kernel 6.16, x86_64 only |
| `PKGBUILD`, `xiaomi-book14-dkms.install` | Arch package |
| `xiaomi-book14-modprobe.conf` | blacklists `bitland_mifs_wmi`, `redmi_wmi` |
| `xiaomi-book14-modules-load.conf` | autoload |
| `99-xiaomi-book14-adp1-wakeup.rules`, `*.service` | stop AC/USB-C events waking a closed-lid s2idle |
| `scripts/check.sh` | compile test: W=1 + sparse + checkpatch |

## Driver map (xiaomi-book14.c)

- **WMAA calls** `book14_mifs_call()`: 32-byte LE in/out buffers, success iff
  `SGER == 0x8000`; `0xE000` = unsupported. Only FUN2 0x0800 (perf mode)
  is used. `book14_wmaa()` uses `wmidev_invoke_method()` on 7.0+ (the old
  `wmidev_evaluate_method()` is deprecated there) and the acpi_buffer API
  on 6.16–6.19 — the only version `#if` besides the 6.16 floor.
- Mainline has its own `bitland-mifs-wmi.c` on the same GUID since 7.1
  (by Mingyou Chen, targets other MIFS firmware); the package blacklists it.
- **EC window** MMIO `0xFE0B0300`+0x100 (`devm_ioremap`): `0x14` bit0 lid,
  `0x68` QFAN perf mode, `0x69`/`0x6B` LE16 fan RPM. ACPI EC never binds
  (firmware never defines `ECON`), so `_Qxx` handlers are dead and
  everything is polled.
- **Pollers**: `ec_work` (normal timer, `poll_ms`) does lid + QFAN + RAPL +
  mailbox; `mbox_work` (deferrable, `mbox_poll_ms`) only mailbox. Both on
  `system_power_efficient_wq`, stopped via devm action (LIFO before the
  resources they touch) and on suspend.
- **Lid**: evaluates firmware `_Q0C`/`_Q0D` (→ `Notify(LID0)`); falls back
  to own `SW_LID` input device if that AML is missing.
- **platform_profile**: low_power/balanced/balanced-performance/performance
  ↔ QFAN 0x02/0x03/0x04/0x09 (0x0A also reads as performance).
- **RAPL**: PL1 per profile from `pl1_*` params, PL2 = 1.25×PL1, written to
  `MSR_PKG_POWER_LIMIT`; idempotent, re-applied after resume; disables
  itself permanently if MSR locked or access faults.
- **Settings key**: XGNS mailbox at phys `0x6FDB8000` (`memremap` WB,
  refuses System RAM); EVTP 0x01 + EVFN 0x25/0x26 → `KEY_CONFIG`.
- **CapsLock LED**: input handler on the i8042 AT keyboard (only when it
  lacks EV_LED, i.e. `i8042.dumbkbd=1`); sends PS/2 `0xED` directly under
  `i8042_lock_chip()` from a work item.
- **debugfs** `xiaomi-book14/`: `qfan_mode`, `ec_dump`,
  `settings_key_test`, `ec_write` (only with `debug_ec_write=1`).
- Suspend/resume must **never return an error** (that is the bug in the
  mainline driver this replaces).

## Checking changes

There is no hardware in a cloud workspace: changes can only be
compile-tested here; runtime behaviour must be verified by the user on the
laptop (`dmesg | grep xiaomi-book14`, `sensors`, debugfs files).

```sh
KDIR=/path/to/configured/linux scripts/check.sh   # must print ok twice
```

Target kernels: 6.16 minimum, 6.18 LTS, and current mainline rc (the user
runs rc kernels, CachyOS-style Clang builds). Before a release, check at
least 6.18 (GCC) and the newest rc with both GCC and Clang. Platform_profile
API used is the post-6.14 `devm_platform_profile_register` +
`platform_profile_ops` form.

Without a full kernel build (no Module.symvers) modpost cannot check
imports; instead verify each `nm -u xiaomi-book14.o` symbol is exported in
the target tree and not namespaced (`EXPORT_SYMBOL_NS*` needs
`MODULE_IMPORT_NS`).

### Recreating the build env in a fresh cloud workspace

Ubuntu apt mirrors are blocked by the egress proxy; GitHub works.

```sh
git clone --depth 1 --branch v6.18 https://github.com/torvalds/linux ~/kernel/linux-6.18
# flex: build westes/flex v2.6.4 release tarball from GitHub (./configure && make install)
# libelf headers: copy libelf/{libelf,gelf,nlist}.h from github.com/sourceware-org/elfutils
#   to /usr/local/include, and symlink libelf.so -> libelf.so.1 (runtime lib is installed)
# sparse: github.com/lucvoo/sparse, make HAVE_LLVM=no HAVE_GTK=no HAVE_SQLITE=no install
cd ~/kernel/linux-6.18 && make defconfig
./scripts/config -e ACPI_WMI -e HWMON -e SERIO_I8042 -e DEBUG_FS -e IDEAPAD_LAPTOP \
    -d DEBUG_INFO_BTF -d MODULE_SIG -d WERROR   # IDEAPAD selects ACPI_PLATFORM_PROFILE
make olddefconfig && make modules_prepare        # ~15 s; enough for compile tests
make -j"$(nproc)" vmlinux modules                # optional, slow: gives Module.symvers
```

Newest rc: `git ls-remote --tags https://github.com/torvalds/linux 'v7.*'`.
A Clang tree needs a separate clean checkout (O= refuses a used source
tree): `git worktree add ../linux-X-clang <tag>`, then the same steps with
`LLVM=1` on every make. To read a file from another release cheaply:
`git fetch --depth 1 --filter=blob:none origin tag vX.Y` then
`git show vX.Y:path` (raw.githubusercontent.com is blocked).

## Conventions

- Commit subjects: `fix: …`, `docs: …`, or plain imperative; releases as
  `xiaomi-book14 X.Y.Z: …`.
- A version bump touches `dkms.conf`, `PKGBUILD` (`pkgver`, reset
  `pkgrel=1`) and every `1.x.y` path in README.md. Packaging-only fixes
  bump `pkgrel` only.
- Every new hardware fact (register, address, AML method) gets a comment
  saying where it came from (DSDT/SSDT name, live measurement).
- License GPL-2.0-or-later; SPDX headers on new files.
