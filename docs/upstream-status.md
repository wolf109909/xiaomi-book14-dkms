# Upstream status: bitland-mifs-wmi on the Xiaomi Book Pro 14 (TM2424)

Snapshot of 2026-10-19, written for the next working session (meant to run
on the laptop itself, so it can collect hardware data and send mail).
Re-check the threads before acting: they move quickly.

## Goal

The author no longer wants to push their own MIFS v2 series. The plan is
to **support the other contributors' work**: answer the open questions,
test their patches on this machine, and contribute only what nobody else
covers (fan tachometers).

## The author's series (July 2026, not merged)

"[RESEND PATCH 0/5] platform/x86: bitland-mifs-wmi: MIFS v2 firmware support
(Xiaomi Book Pro 14 2026)", sent 2026-07-28 from wolf109909@outlook.com
(name 孙 誉铭 / Yuming Sun).
Thread: https://ratatoskr.run/platform-driver-x86/2026/07/17331400/t
Cover on lkml.iu.edu: https://lkml.iu.edu/2607.3/08825.html
(lore.kernel.org is the canonical archive; get the Message-ID there for
In-Reply-To.)

| Patch | Status | Plan |
|---|---|---|
| 0/5 cover | **Armin Wolf asked twice for an acpidump** (2026-07-30, 2026-08-26), calls missing ECON a "severe ACPI bug". Unanswered. | Reply first, with acpidump + BIOS version + whether `acpi_ec` binds |
| 1/5 never abort suspend | Ilpo questioned `profile_valid` logic | Withdraw; support Chris Taraszka's suspend fix (below), offer Tested-by |
| 2/5 QFAN mapping | Mapping disputed; Mingyou wants ops table not `is_v2` | Withdraw; support Chris's profile-values patch, offer Tested-by |
| 3/5 fan tach (EC 0x69/0x6B) | Ilpo review open (SZ_ constant, comment alignment, -EINVAL wrong, drop is_v2) | Only unique part: resend as v2 on top of Mingyou's ops table |
| 4/5 lid EC poll | KentoNion: on their BIOS ACPI EC *does* start and the lid switch binds; WMI driver is the wrong layer | Drop until the ECON/EC question is answered with real data |
| 5/5 docs | — | Fold relevant parts into the fan v2 |

## Other people's work to support

- **Chris Taraszka, "Don't abort suspend when platform profile access fails"**
  (2026-09-11, Fixes: dc1ec4fa86b2, Cc: stable; Armin replied).
  https://ratatoskr.run/stable/2026/09/17554736
- **Chris Taraszka, "Add Xiaomi Book Pro 14 platform profile values"**
  (2026-09-28; Ilpo reviewing, latest reply 2026-10-19). DMI: sys_vendor
  XIAOMI + product "Xiaomi Book Pro 14". BIOS XMAPT4B0P0909. Mapping:
  low-power 0x02 (0x0a also read as low-power), balanced 0x03,
  balanced-performance 0x09, performance 0x04. Notes -ENOMSG on for-next
  from commit 23cc56f6dea6 ("Detect failed function calls").
  https://ratatoskr.run/platform-driver-x86/2026/09/17654302
- **Mingyou Chen, "Merge redmi-wmi into bitland-mifs-wmi"**, with a
  per-machine ops/quirks table; at v12 on 2026-10-12. This is the base new
  patches should go on.
  https://ratatoskr.run/platform-driver-x86/2026/08/17418132/t
- **KentoNion** (also TM2424) "[PATCH] bitland-mifs-wmi: add TM2424 ops and
  hotkeys" on top of Mingyou v5. Names codes 0x02 Quiet, 0x03 Turbo,
  0x04 Full-speed, 0x09 Auto, 0x0A Eco.

7.3-rc7 contains none of the above (redmi-wmi still separate, no ops table).

## Finding that affects this repo

Chris measured PL1 per mode: 0x02 ≈ 18 W, 0x03 ≈ 37 W, 0x09 ≈ 45 W,
**0x04 ≈ 58 W**. With KentoNion's naming, 0x04 is the top mode, not 0x09.
`xiaomi-book14.c` maps balanced-performance → 0x04 and performance → 0x09
(the inverse). The module hides this by programming PL1 itself.
To do: measure on the laptop with `rapl=N` (firmware limits) and swap
`pp_to_qfan()` / `qfan_to_pp()` if confirmed.

## Data to collect on the laptop first

```sh
cat /sys/class/dmi/id/{sys_vendor,board_name,product_name,bios_version}
journalctl -k -b | grep -i -E 'ACPI: EC|ECON|LID|Q_EC'   # does acpi_ec bind on this BIOS?
sudo acpidump > tm2424-acpidump.txt                       # for Armin (link or off-list)
# per-mode firmware PL1, with this module loaded rapl=N (or unloaded):
cat /sys/class/powercap/intel-rapl-mmio:0/constraint_0_power_limit_uw
```

Then: test Chris's two patches (and Mingyou's latest) on the running rc
kernel, and send Tested-by replies.

## Mail etiquette for these replies

Plain text, bottom-posted / inline replies, wrapped at ~72 columns, reply
to all with the existing Cc list and correct In-Reply-To (use `b4` or
`git send-email --in-reply-to=<msgid>`). Tested-by lines go on their own
line: `Tested-by: Yuming Sun <wolf109909@outlook.com>`. The author sends
mail themselves after reviewing every draft — never send unreviewed.
SMTP credentials are not stored in this repo.

## Branches in this repo (from the 2026-10-18/19 cloud session)

- `dev-setup`: scripts/check.sh, CLAUDE.md, wmidev_invoke_method() on 7.0+.
- `review-fixes` (on top of dev-setup): CapsLock ACK race via i8042 filter,
  torn fan RPM reads, sysfs profile changes logged as Fn+K, restore
  firmware RAPL on unload, drop dkms CLEAN. Compile-tested only on 6.18 and
  7.3-rc7 (GCC + Clang); needs hardware testing — see the PR/commit
  messages for the test steps.
