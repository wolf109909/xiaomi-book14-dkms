// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Xiaomi Book Pro 14 2026 (Panther Lake, Bitland MIFS v2) platform driver.
 *
 * Copyright (C) 2026 Yuming Sun <wolf109909@outlook.com>
 *
 * Reverse-engineered from the laptop's ACPI tables (DSDT/SSDT24 "WMID"
 * device, UID "MIFS") and the firmware's EC shared-memory window.
 *
 * Why this driver exists:
 *  - The mainline bitland-mifs-wmi driver targets different MIFS firmware:
 *    this machine's WMAA method only implements FUN2 0x0800/0x0A00/0x0C00/
 *    0x1000, perf-mode values are QFAN codes {2,3,4,9,10} (not 0..3), and it
 *    has no WMI fan-speed/CPU-temp functions at all.
 *  - The firmware never defines ECON, so \_SB.PC00.LPCB.Q_EC._STA fails and
 *    the ACPI EC driver never binds: all _Qxx handlers (lid, Fn+K, ...) are
 *    dead. We poll the EC window directly instead.
 *  - INTC10D4 (Intel DTT) has no Linux driver, so changing TDP must go
 *    through RAPL MSR writes.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
#error "xiaomi-book14 requires Linux 6.16 or newer"
#endif

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/i8042.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>
#include <asm/msr.h>

#define DRV_NAME		"xiaomi-book14"
#define XIAOMI_MIFS_METHOD_GUID	"B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B"

/* WMAA protocol (SSDT24): 32-byte in/out, little-endian */
#define MIFS_OP_GET		0xFA00
#define MIFS_OP_SET		0xFB00
#define MIFS_FN_PERF_MODE	0x0800
#define MIFS_STATUS_OK		0x8000
#define MIFS_STATUS_UNSUPPORTED	0xE000

/* EC shared-memory window (DSDT \_SB.PC00.LPCB.Q_EC, region ERAM) */
#define QEC_PATH		"\\_SB.PC00.LPCB.Q_EC"
#define EC_MEM_BASE		0xFE0B0300
#define EC_MEM_SIZE		0x100
#define EC_REG_LID		0x14	/* bit0: LSTE, 1 = lid open */
#define EC_LID_OPEN		BIT(0)
#define EC_REG_QFAN		0x68	/* perf mode set by FW/EC */
#define EC_REG_FAN1		0x69	/* u16 LE, RPM, 0 when stopped */
#define EC_REG_FAN2		0x6B	/* u16 LE, RPM */

/* WMID XGNS mailbox (SSDT24): shared BIOS<->OS event block */
#define MBOX_PHYS_DEFAULT	0x6FDB8000
#define MBOX_SIZE		0x2C
#define MBOX_OFF_EVTP		0x24
#define MBOX_OFF_EVFN		0x25
#define MBOX_EVTP_HOTKEY	0x01
#define MBOX_EVFN_SETTINGS_PRESS	0x25	/* dedicated settings key down (_Q15) */
#define MBOX_EVFN_SETTINGS_RELEASE	0x26	/* dedicated settings key up (_Q16) */

/* Perf modes (QFAN values, from SSDT24 WMAA/EV20) */
#define QFAN_QUIET		0x02
#define QFAN_BALANCED		0x03
#define QFAN_SPEED		0x04
#define QFAN_EXTREME		0x09
#define QFAN_EXTREME2		0x0A

/* RAPL: PL1 in bits 14:0, PL2 in bits 46:32 of MSR_PKG_POWER_LIMIT */
#define RAPL_LIMIT_MASK		0x7fffULL
#define RAPL_PL2_SHIFT		32
#define RAPL_LOCK		BIT_ULL(63)

/* i8042 ports (CapsLock LED path; see the kbd_* helpers below) */
#define I8042_DATA_REG		0x60
#define I8042_STATUS_REG	0x64
#define I8042_STR_IBF		0x02
#define I8042_STR_AUXDATA	0x20
#define KBD_CMD_SET_LEDS	0xED
#define KBD_REPLY_ACK		0xFA
#define KBD_REPLY_RESEND	0xFE
#define KBD_LED_CAPSL		0x04
#define KBD_IBF_TIMEOUT_US	10000
#define KBD_ACK_TIMEOUT_MS	100

#define STATE_UNKNOWN		0xff
#define POLL_MIN_MS		10

struct mifs_in {
	__le16 fun1;	/* MIFS_OP_* */
	__le16 fun2;	/* function selector */
	__le16 fun3;	/* sub-function / value on set */
	__le32 fun4;	/* value on set */
	u8 pad[20];
} __packed;

struct mifs_out {
	__le16 sger;	/* MIFS_STATUS_OK on success */
	__le16 futr;
	__le16 frd0;
	__le32 frd1;
	__le32 frd2;
	__le32 frd3;
	u8 pad[14];
} __packed;

/* ---------- module parameters ---------- */

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Bind even if DMI does not identify a Xiaomi Book Pro 14");

static bool rapl = true;
module_param(rapl, bool, 0644);
MODULE_PARM_DESC(rapl, "Program RAPL PL1/PL2 on profile changes (default: on)");

/* per-profile PL1 in mW; changes take effect within one poll interval */
static int pl1_low_power = 15000;
static int pl1_balanced = 35000;
static int pl1_balanced_perf = 45000;
static int pl1_performance = 61000;
module_param(pl1_low_power, int, 0644);
module_param(pl1_balanced, int, 0644);
module_param(pl1_balanced_perf, int, 0644);
module_param(pl1_performance, int, 0644);
MODULE_PARM_DESC(pl1_low_power, "RAPL PL1 (mW) for low-power profile");
MODULE_PARM_DESC(pl1_balanced, "RAPL PL1 (mW) for balanced profile");
MODULE_PARM_DESC(pl1_balanced_perf, "RAPL PL1 (mW) for balanced-performance profile");
MODULE_PARM_DESC(pl1_performance, "RAPL PL1 (mW) for performance profile");

static uint poll_ms = 250;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "EC poll interval in ms (lid switch, perf-mode watch; min 10)");

static uint mbox_poll_ms = 50;
module_param(mbox_poll_ms, uint, 0644);
MODULE_PARM_DESC(mbox_poll_ms, "Hotkey mailbox poll interval in ms while the CPU is awake (min 10)");

static ulong mbox_addr = MBOX_PHYS_DEFAULT;
module_param(mbox_addr, ulong, 0444);
MODULE_PARM_DESC(mbox_addr, "Physical address of the XGNS hotkey mailbox (0 disables the settings key)");

static bool debug_ec_write;
module_param(debug_ec_write, bool, 0444);
MODULE_PARM_DESC(debug_ec_write, "Expose debugfs ec_write (raw EC window writes; default: off)");

/* ---------- driver state ---------- */

struct book14 {
	struct wmi_device *wdev;
	struct mutex call_lock;		/* serializes WMAA calls */
	struct mutex rapl_lock;		/* serializes RAPL MSR updates */
	struct mutex mbox_lock;		/* serializes mailbox scans */

	u8 __iomem *ec;
	u8 *mbox;
	acpi_handle qec, q0c, q0d;
	bool qec_notify;

	struct device *pp_dev;
	struct input_dev *settings_input;
	struct input_dev *lid_input;	/* only when the _Q0C/_Q0D AML is missing */

	struct delayed_work ec_work;	/* guaranteed-latency poll */
	struct delayed_work mbox_work;	/* deferrable fast poll */

	u8 last_lid;
	u8 last_qfan;
	u8 last_evfn;
	bool settings_down;

	int cur_pl1;
	bool rapl_failed;

	struct input_handler kbd_handler;
	struct input_handle *kbd_handle;
	struct work_struct kbd_work;
	u8 kbd_leds;
	bool kbd_filter;		/* our i8042 filter is installed */
	bool kbd_wait_ack;		/* filter: next kbd ACK/RESEND is ours */
	u8 kbd_reply;
	struct completion kbd_ack;

	struct dentry *dbg;
};

/* poll intervals are runtime-writable parameters: clamp to a sane minimum */
static unsigned long book14_poll_delay(uint ms)
{
	return msecs_to_jiffies(max_t(uint, ms, POLL_MIN_MS));
}

/* ---------- WMAA ---------- */

/*
 * Run WMAA (instance 0, method 1) and copy its 32-byte result into @res.
 * Linux 7.0 added the buffer-based wmidev_invoke_method() and deprecated
 * the ACPI-object based wmidev_evaluate_method(); use the new API where it
 * exists and keep the old one for 6.16 - 6.19.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static int book14_wmaa(struct book14 *b, const struct mifs_in *in,
		       struct mifs_out *res)
{
	const struct wmi_buffer in_buf = {
		.length = sizeof(*in),
		.data = (void *)in,
	};
	struct wmi_buffer out_buf;
	int ret;

	ret = wmidev_invoke_method(b->wdev, 0, 1, &in_buf, &out_buf,
				   sizeof(*res));
	if (ret)
		return ret;

	memcpy(res, out_buf.data, sizeof(*res));
	kfree(out_buf.data);
	return 0;
}
#else
static int book14_wmaa(struct book14 *b, const struct mifs_in *in,
		       struct mifs_out *res)
{
	const struct acpi_buffer in_buf = { sizeof(*in), (void *)in };
	struct acpi_buffer out_buf = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	status = wmidev_evaluate_method(b->wdev, 0, 1, &in_buf, &out_buf);
	if (ACPI_FAILURE(status))
		return -EIO;

	union acpi_object *obj __free(kfree) = out_buf.pointer;

	if (!obj || obj->type != ACPI_TYPE_BUFFER ||
	    obj->buffer.length < sizeof(*res))
		return -EIO;
	memcpy(res, obj->buffer.pointer, sizeof(*res));
	return 0;
}
#endif

static int book14_mifs_call(struct book14 *b, u16 op, u16 fun2, u16 fun3,
			    struct mifs_out *out)
{
	const struct mifs_in in = {
		.fun1 = cpu_to_le16(op),
		.fun2 = cpu_to_le16(fun2),
		.fun3 = cpu_to_le16(fun3),
	};
	struct mifs_out res;
	u16 sger;
	int ret;

	guard(mutex)(&b->call_lock);

	ret = book14_wmaa(b, &in, &res);
	if (ret)
		return ret;

	sger = le16_to_cpu(res.sger);
	if (sger != MIFS_STATUS_OK) {
		dev_dbg(&b->wdev->dev, "WMAA %04x/%04x failed, sger=%04x\n",
			op, fun2, sger);
		return sger == MIFS_STATUS_UNSUPPORTED ? -EOPNOTSUPP : -EIO;
	}
	if (out)
		*out = res;
	return 0;
}

static int book14_get_qfan(struct book14 *b, u8 *qfan)
{
	struct mifs_out out;
	int ret;

	ret = book14_mifs_call(b, MIFS_OP_GET, MIFS_FN_PERF_MODE, 0, &out);
	if (ret)
		return ret;
	*qfan = le16_to_cpu(out.frd0) & 0xff;
	return 0;
}

/* ---------- profile <-> QFAN <-> PL1 mapping ---------- */

static int pp_to_qfan(enum platform_profile_option p, u8 *v)
{
	switch (p) {
	case PLATFORM_PROFILE_LOW_POWER:
		*v = QFAN_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		*v = QFAN_BALANCED;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		*v = QFAN_SPEED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		*v = QFAN_EXTREME;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static enum platform_profile_option qfan_to_pp(u8 q)
{
	switch (q) {
	case QFAN_QUIET:
		return PLATFORM_PROFILE_LOW_POWER;
	case QFAN_SPEED:
		return PLATFORM_PROFILE_BALANCED_PERFORMANCE;
	case QFAN_EXTREME:
	case QFAN_EXTREME2:
		return PLATFORM_PROFILE_PERFORMANCE;
	case QFAN_BALANCED:
	default:
		return PLATFORM_PROFILE_BALANCED;
	}
}

static int qfan_to_pl1(u8 q)
{
	switch (qfan_to_pp(q)) {
	case PLATFORM_PROFILE_LOW_POWER:
		return READ_ONCE(pl1_low_power);
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		return READ_ONCE(pl1_balanced_perf);
	case PLATFORM_PROFILE_PERFORMANCE:
		return READ_ONCE(pl1_performance);
	default:
		return READ_ONCE(pl1_balanced);
	}
}

/* ---------- RAPL ---------- */

/*
 * Program PL1 (and PL2 = 1.25 x PL1) for the given perf mode. Cheap no-op
 * when the target is already applied, so it is safe to call every poll:
 * this also picks up pl1_* parameter changes and re-applies the limits after
 * resume (firmware may restore its own values).
 */
static void book14_rapl_apply(struct book14 *b, u8 qfan)
{
	struct device *dev = &b->wdev->dev;
	u64 unit_msr, limit_msr, pl1, pl2;
	u32 uw_per_unit;
	int mw = qfan_to_pl1(qfan);

	if (!READ_ONCE(rapl) || mw <= 0)
		return;

	guard(mutex)(&b->rapl_lock);

	if (b->rapl_failed || mw == b->cur_pl1)
		return;

	if (rdmsrq_safe(MSR_RAPL_POWER_UNIT, &unit_msr) ||
	    rdmsrq_safe(MSR_PKG_POWER_LIMIT, &limit_msr))
		goto fail;

	if (limit_msr & RAPL_LOCK) {
		dev_warn(dev, "RAPL power limits are locked by firmware, not managing PL1/PL2\n");
		b->rapl_failed = true;
		return;
	}

	/* power unit = 1/2^(bits 3:0) W */
	uw_per_unit = 1000000U >> (unit_msr & 0xf);
	if (!uw_per_unit)
		goto fail;

	pl1 = div_u64((u64)mw * 1000, uw_per_unit);
	pl2 = div_u64((u64)(mw + mw / 4) * 1000, uw_per_unit);
	pl1 = min(pl1, RAPL_LIMIT_MASK);
	pl2 = min(pl2, RAPL_LIMIT_MASK);

	limit_msr &= ~(RAPL_LIMIT_MASK | (RAPL_LIMIT_MASK << RAPL_PL2_SHIFT));
	limit_msr |= pl1 | (pl2 << RAPL_PL2_SHIFT);

	if (wrmsrq_safe(MSR_PKG_POWER_LIMIT, limit_msr))
		goto fail;

	b->cur_pl1 = mw;
	dev_dbg(dev, "RAPL PL1=%d mW PL2=%d mW\n", mw, mw + mw / 4);
	return;

fail:
	dev_warn(dev, "RAPL MSR access failed, not managing PL1/PL2\n");
	b->rapl_failed = true;
}

/* ---------- platform profile ---------- */

static int book14_profile_get(struct device *dev,
			      enum platform_profile_option *profile)
{
	struct book14 *b = dev_get_drvdata(dev);
	u8 qfan;
	int ret;

	ret = book14_get_qfan(b, &qfan);
	if (ret)
		return ret;
	*profile = qfan_to_pp(qfan);
	return 0;
}

static int book14_profile_set(struct device *dev,
			      enum platform_profile_option profile)
{
	struct book14 *b = dev_get_drvdata(dev);
	int ret;
	u8 v;

	ret = pp_to_qfan(profile, &v);
	if (ret)
		return ret;
	ret = book14_mifs_call(b, MIFS_OP_SET, MIFS_FN_PERF_MODE, v, NULL);
	if (ret)
		return ret;

	book14_rapl_apply(b, v);
	return 0;
}

static int book14_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static const struct platform_profile_ops book14_profile_ops = {
	.probe = book14_profile_probe,
	.profile_get = book14_profile_get,
	.profile_set = book14_profile_set,
};

/* ---------- hwmon: dual fan tach ---------- */

static umode_t book14_hwmon_visible(const void *data,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	return 0444;
}

static int book14_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct book14 *b = dev_get_drvdata(dev);
	unsigned int off = channel ? EC_REG_FAN2 : EC_REG_FAN1;

	if (type != hwmon_fan || attr != hwmon_fan_input)
		return -EOPNOTSUPP;
	/* u16 LE at an odd offset: two byte reads, no unaligned MMIO access */
	*val = readb(b->ec + off) | (readb(b->ec + off + 1) << 8);
	return 0;
}

static int book14_hwmon_read_string(struct device *dev,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel, const char **str)
{
	if (type != hwmon_fan || attr != hwmon_fan_label)
		return -EOPNOTSUPP;
	*str = channel ? "Right Fan" : "Left Fan";
	return 0;
}

static const struct hwmon_channel_info * const book14_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_ops book14_hwmon_ops = {
	.is_visible = book14_hwmon_visible,
	.read = book14_hwmon_read,
	.read_string = book14_hwmon_read_string,
};

static const struct hwmon_chip_info book14_chip_info = {
	.ops = &book14_hwmon_ops,
	.info = book14_hwmon_info,
};

/* ---------- EC poll: lid switch + perf-mode watch ---------- */

/*
 * The ACPI EC driver never binds on this machine (ECON undefined in the
 * firmware tables, so Q_EC._STA fails). All _Qxx handlers are therefore
 * dead, including _Q0C/_Q0D (lid) and _Q24 (Fn+K). Poll the EC window and
 * drive the same AML the query handlers would have run.
 */
static void book14_check_lid(struct book14 *b)
{
	u8 lid = readb(b->ec + EC_REG_LID) & EC_LID_OPEN;
	acpi_status status;

	if (lid == b->last_lid)
		return;

	if (b->lid_input) {
		/* fallback switch: always report, the input core drops repeats */
		input_report_switch(b->lid_input, SW_LID, !lid);
		input_sync(b->lid_input);
	} else if (b->last_lid != STATE_UNKNOWN) {
		status = acpi_evaluate_object(lid ? b->q0d : b->q0c, NULL,
					      NULL, NULL);
		if (ACPI_FAILURE(status))
			dev_warn_ratelimited(&b->wdev->dev, "%s failed: %s\n",
					     lid ? "_Q0D" : "_Q0C",
					     acpi_format_exception(status));
	}

	if (b->last_lid != STATE_UNKNOWN)
		dev_info(&b->wdev->dev, "lid %s\n", lid ? "opened" : "closed");
	b->last_lid = lid;
}

static void book14_check_qfan(struct book14 *b)
{
	u8 qfan = readb(b->ec + EC_REG_QFAN);

	if (qfan != b->last_qfan) {
		if (b->last_qfan != STATE_UNKNOWN) {
			dev_info(&b->wdev->dev,
				 "perf mode changed to 0x%02x (Fn+K or EC)\n",
				 qfan);
			platform_profile_notify(b->pp_dev);
		}
		b->last_qfan = qfan;
	}

	book14_rapl_apply(b, qfan);
}

/*
 * Settings key: the EC drops EVFN 0x25/0x26 into the XGNS mailbox and only
 * holds the press code while the key is down, so this needs fast polling.
 */
static void book14_check_mbox(struct book14 *b)
{
	struct input_dev *in = b->settings_input;
	u8 evtp, evfn;

	guard(mutex)(&b->mbox_lock);

	evtp = READ_ONCE(b->mbox[MBOX_OFF_EVTP]);
	evfn = READ_ONCE(b->mbox[MBOX_OFF_EVFN]);
	if (evfn == b->last_evfn)
		return;
	b->last_evfn = evfn;

	dev_dbg(&b->wdev->dev, "mbox event: EVTP=%02x EVFN=%02x\n", evtp, evfn);
	if (evtp != MBOX_EVTP_HOTKEY)
		return;

	switch (evfn) {
	case MBOX_EVFN_SETTINGS_PRESS:
		input_report_key(in, KEY_CONFIG, 1);
		input_sync(in);
		b->settings_down = true;
		break;
	case MBOX_EVFN_SETTINGS_RELEASE:
		if (!b->settings_down) {
			/* tap fully inside one poll window: synthesize press */
			input_report_key(in, KEY_CONFIG, 1);
			input_sync(in);
		}
		input_report_key(in, KEY_CONFIG, 0);
		input_sync(in);
		b->settings_down = false;
		break;
	}
}

/*
 * Two pollers keep idle wakeups low without losing key presses:
 *  - ec_work runs on a normal timer every poll_ms (lid, perf mode, RAPL) and
 *    also scans the mailbox, which bounds hotkey latency to poll_ms.
 *  - mbox_work runs every mbox_poll_ms on a *deferrable* timer: it never
 *    wakes an idle CPU on its own, but catches quick taps whenever the
 *    system is awake anyway (i.e. while someone is using it).
 */
static void book14_ec_work(struct work_struct *work)
{
	struct book14 *b = container_of(to_delayed_work(work), struct book14,
					ec_work);

	book14_check_lid(b);
	book14_check_qfan(b);
	if (b->mbox)
		book14_check_mbox(b);

	queue_delayed_work(system_power_efficient_wq, &b->ec_work,
			   book14_poll_delay(READ_ONCE(poll_ms)));
}

static void book14_mbox_work(struct work_struct *work)
{
	struct book14 *b = container_of(to_delayed_work(work), struct book14,
					mbox_work);

	book14_check_mbox(b);
	queue_delayed_work(system_power_efficient_wq, &b->mbox_work,
			   book14_poll_delay(READ_ONCE(mbox_poll_ms)));
}

static void book14_start_polling(struct book14 *b)
{
	queue_delayed_work(system_power_efficient_wq, &b->ec_work, 0);
	if (b->mbox)
		queue_delayed_work(system_power_efficient_wq, &b->mbox_work, 0);
}

static void book14_stop_polling(void *data)
{
	struct book14 *b = data;

	cancel_delayed_work_sync(&b->mbox_work);
	cancel_delayed_work_sync(&b->ec_work);
}

/* ---------- CapsLock LED ----------
 *
 * This keyboard requires i8042.nopnp=1 + i8042.dumbkbd=1 to function.
 * dumbkbd makes i8042 reject all writes to the keyboard, so atkbd's LED
 * path is dead and the CapsLock LED never lights (the LED is driven by
 * the EC in response to the standard PS/2 Set LEDs command 0xED; the EC
 * also mirrors the state in ERAM 0x16 bit6 "CPLK"). The input core still
 * emits EV_LED events (vt/KWin/libinput set them), so we listen for
 * LED_CAPSL on the built-in keyboard and send the 0xED sequence ourselves.
 * atkbd also drops the device's EV_LED capability under dumbkbd, so we
 * re-enable it on connect: that unblocks vt/KWin LED state injection.
 *
 * The EC's ACK (0xFA) arrives as an ordinary keyboard byte: the i8042 IRQ
 * handler reads it under its own spinlock (i8042_lock_chip() does not keep
 * it out), so polling the status register for it races with the IRQ. An
 * i8042 filter sees every byte instead: it completes kbd_ack and swallows
 * the ACKs we asked for, which also stops atkbd logging them as spurious.
 */
static bool book14_i8042_filter(unsigned char data, unsigned char str,
				struct serio *serio, void *context)
{
	struct book14 *b = context;

	/* runs in IRQ context under the i8042 spinlock */
	if (!READ_ONCE(b->kbd_wait_ack) || (str & I8042_STR_AUXDATA) ||
	    (data != KBD_REPLY_ACK && data != KBD_REPLY_RESEND))
		return false;

	WRITE_ONCE(b->kbd_wait_ack, false);
	b->kbd_reply = data;
	complete(&b->kbd_ack);
	return true;
}

/* send one byte to the keyboard (EC) and wait for its ACK if we can see it */
static int book14_kbd_write(struct book14 *b, u8 byte)
{
	u8 st;
	int ret;

	ret = read_poll_timeout(inb, st, !(st & I8042_STR_IBF), 50,
				KBD_IBF_TIMEOUT_US, false, I8042_STATUS_REG);
	if (ret)
		return ret;

	if (!b->kbd_filter) {
		/* no filter (another driver owns it): give the EC time instead */
		outb(byte, I8042_DATA_REG);
		usleep_range(2000, 3000);
		return 0;
	}

	reinit_completion(&b->kbd_ack);
	WRITE_ONCE(b->kbd_wait_ack, true);
	outb(byte, I8042_DATA_REG);

	if (!wait_for_completion_timeout(&b->kbd_ack,
					 msecs_to_jiffies(KBD_ACK_TIMEOUT_MS))) {
		WRITE_ONCE(b->kbd_wait_ack, false);
		return -ETIMEDOUT;
	}
	return b->kbd_reply == KBD_REPLY_ACK ? 0 : -EIO;
}

/* process context only (sleeps) */
static int book14_kbd_send(struct book14 *b, u8 leds)
{
	int ret;

	i8042_lock_chip();
	ret = book14_kbd_write(b, KBD_CMD_SET_LEDS);
	if (!ret) {
		usleep_range(1000, 2000);
		ret = book14_kbd_write(b, leds);
	}
	i8042_unlock_chip();
	return ret;
}

static void book14_kbd_work(struct work_struct *work)
{
	struct book14 *b = container_of(work, struct book14, kbd_work);
	int ret;

	ret = book14_kbd_send(b, READ_ONCE(b->kbd_leds));
	if (ret)
		dev_dbg(&b->wdev->dev, "CapsLock LED update failed: %d\n", ret);
}

static void book14_kbd_event(struct input_handle *handle, unsigned int type,
			     unsigned int code, int value)
{
	struct book14 *b = handle->private;

	if (type == EV_LED && code == LED_CAPSL) {
		WRITE_ONCE(b->kbd_leds, value ? KBD_LED_CAPSL : 0);
		schedule_work(&b->kbd_work);
	}
}

/* called with the input core's mutex held */
static int book14_kbd_connect(struct input_handler *handler,
			      struct input_dev *dev,
			      const struct input_device_id *id)
{
	struct book14 *b = handler->private;
	struct input_handle *handle;
	int ret;

	/*
	 * Only one keyboard, and only when atkbd cannot drive the LEDs itself
	 * (it drops EV_LED when i8042.dumbkbd removes the serio write path).
	 * Without dumbkbd, atkbd handles the LEDs and we must stay out.
	 */
	if (b->kbd_handle || test_bit(EV_LED, dev->evbit))
		return -ENODEV;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = DRV_NAME "-kbdled";
	handle->private = b;

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;

	/* the i8042 filter slot is global: fine if another driver holds it */
	b->kbd_filter = !i8042_install_filter(book14_i8042_filter, b);
	if (!b->kbd_filter)
		dev_info(&b->wdev->dev, "i8042 filter in use, CapsLock LED without ACK tracking\n");

	/* re-enable LED events on the device (dropped under dumbkbd) */
	__set_bit(EV_LED, dev->evbit);
	__set_bit(LED_CAPSL, dev->ledbit);
	b->kbd_handle = handle;

	/* sync LED with the current OS capslock state */
	WRITE_ONCE(b->kbd_leds, test_bit(LED_CAPSL, dev->led) ? KBD_LED_CAPSL : 0);
	schedule_work(&b->kbd_work);
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void book14_kbd_disconnect(struct input_handle *handle)
{
	struct book14 *b = handle->private;

	input_close_device(handle);
	input_unregister_handle(handle);
	cancel_work_sync(&b->kbd_work);

	if (b->kbd_filter) {
		i8042_remove_filter(book14_i8042_filter);
		b->kbd_filter = false;
	}

	/* undo our EV_LED so a reload (or atkbd) sees the device as it was */
	__clear_bit(EV_LED, handle->dev->evbit);
	b->kbd_handle = NULL;
	kfree(handle);
}

/* AT keyboard on the i8042 with a CapsLock key: never USB keyboards/mice */
static const struct input_device_id book14_kbd_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_BUS |
			 INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT,
		.bustype = BUS_I8042,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_CAPSLOCK)] = BIT_MASK(KEY_CAPSLOCK) },
	},
	{ }
};

static void book14_kbd_unregister(void *data)
{
	struct book14 *b = data;

	input_unregister_handler(&b->kbd_handler);
	cancel_work_sync(&b->kbd_work);
}

static void book14_kbd_init(struct book14 *b)
{
	struct device *dev = &b->wdev->dev;
	int ret;

	INIT_WORK(&b->kbd_work, book14_kbd_work);
	init_completion(&b->kbd_ack);
	b->kbd_handler = (struct input_handler) {
		.event = book14_kbd_event,
		.connect = book14_kbd_connect,
		.disconnect = book14_kbd_disconnect,
		.name = DRV_NAME "_kbdled",
		.id_table = book14_kbd_ids,
		.private = b,
	};

	ret = input_register_handler(&b->kbd_handler);
	if (ret) {
		dev_warn(dev, "CapsLock LED handler failed: %d\n", ret);
		return;
	}
	if (devm_add_action_or_reset(dev, book14_kbd_unregister, b))
		dev_warn(dev, "CapsLock LED handler disabled\n");
}

/* ---------- Q_EC notify: EC query events (debug only) ---------- */

static void book14_ec_notify(acpi_handle handle, u32 event, void *context)
{
	pr_debug("Q_EC notify 0x%02x\n", event);
}

static void book14_remove_notify(void *data)
{
	struct book14 *b = data;

	acpi_remove_notify_handler(b->qec, ACPI_DEVICE_NOTIFY,
				   book14_ec_notify);
}

/* ---------- debugfs: raw mode control (verification/tuning) ---------- */

static ssize_t qfan_mode_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct book14 *b = s->private;
	u8 v;
	int ret;

	ret = kstrtou8_from_user(ubuf, count, 0, &v);
	if (ret)
		return ret;
	ret = book14_mifs_call(b, MIFS_OP_SET, MIFS_FN_PERF_MODE, v, NULL);
	if (ret)
		return ret;
	/* the EC poll picks up the change, applies RAPL and notifies */
	return count;
}

static int qfan_mode_show(struct seq_file *s, void *unused)
{
	struct book14 *b = s->private;
	u8 qfan;
	int ret;

	ret = book14_get_qfan(b, &qfan);
	if (ret)
		return ret;
	seq_printf(s, "0x%02x\n", qfan);
	return 0;
}
DEFINE_SHOW_STORE_ATTRIBUTE(qfan_mode);

/*
 * settings_key_test: writing anything synthesizes a settings-key tap on the
 * input device (press+release of KEY_CONFIG). Lets users/CI verify the whole
 * key->daemon->command pipeline without touching the physical key.
 */
static ssize_t settings_key_test_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct book14 *b = file->private_data;

	guard(mutex)(&b->mbox_lock);
	input_report_key(b->settings_input, KEY_CONFIG, 1);
	input_sync(b->settings_input);
	input_report_key(b->settings_input, KEY_CONFIG, 0);
	input_sync(b->settings_input);
	return count;
}

static const struct file_operations settings_key_test_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = settings_key_test_write,
};

/* ec_write: "0xOFF 0xVAL" writes one byte into the EC shared-memory window */
static ssize_t ec_write_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct book14 *b = file->private_data;
	unsigned int off, val;
	char buf[32];

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (sscanf(buf, "%i %i", &off, &val) != 2 ||
	    off >= EC_MEM_SIZE || val > 0xff)
		return -EINVAL;
	writeb(val, b->ec + off);
	return count;
}

static const struct file_operations ec_write_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ec_write_write,
};

/* ec_dump: hex dump of the EC shared-memory window (0x100 bytes) */
static int ec_dump_show(struct seq_file *s, void *unused)
{
	struct book14 *b = s->private;
	u8 line[16];
	int i;

	for (i = 0; i < EC_MEM_SIZE; i += sizeof(line)) {
		memcpy_fromio(line, b->ec + i, sizeof(line));
		seq_printf(s, "%02x: %*phN\n", i, (int)sizeof(line), line);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ec_dump);

static void book14_debugfs_remove(void *data)
{
	struct book14 *b = data;

	debugfs_remove_recursive(b->dbg);
}

static void book14_debugfs_init(struct book14 *b)
{
	struct device *dev = &b->wdev->dev;

	b->dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("qfan_mode", 0600, b->dbg, b, &qfan_mode_fops);
	debugfs_create_file("ec_dump", 0400, b->dbg, b, &ec_dump_fops);
	if (b->settings_input)
		debugfs_create_file("settings_key_test", 0200, b->dbg, b,
				    &settings_key_test_fops);
	if (debug_ec_write)
		debugfs_create_file("ec_write", 0200, b->dbg, b,
				    &ec_write_fops);

	if (devm_add_action_or_reset(dev, book14_debugfs_remove, b))
		dev_warn(dev, "debugfs disabled\n");
}

/* ---------- probe helpers ---------- */

static const struct dmi_system_id book14_dmi_table[] = {
	{
		.ident = "Xiaomi Book Pro 14 2026",
		.matches = {
			DMI_MATCH(DMI_PRODUCT_NAME, "Xiaomi Book Pro 14"),
		},
	},
	{ }
};

/*
 * The mailbox address comes from SSDT24 and may move with a BIOS update.
 * Refuse to treat ordinary kernel-managed RAM as the mailbox: that would
 * mean reading random memory.
 */
static void book14_mbox_init(struct book14 *b)
{
	struct device *dev = &b->wdev->dev;
	void *mbox;

	if (!mbox_addr)
		return;

	if (region_intersects(mbox_addr, MBOX_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		dev_warn(dev, "mailbox %#lx is in System RAM, settings key disabled (check XGNS in SSDT, set mbox_addr=)\n",
			 mbox_addr);
		return;
	}

	mbox = devm_memremap(dev, mbox_addr, MBOX_SIZE, MEMREMAP_WB);
	if (IS_ERR(mbox)) {
		dev_warn(dev, "cannot map mailbox %#lx, settings key disabled: %pe\n",
			 mbox_addr, mbox);
		return;
	}
	b->mbox = mbox;
	b->last_evfn = READ_ONCE(b->mbox[MBOX_OFF_EVFN]);
}

static int book14_input_init(struct book14 *b)
{
	struct device *dev = &b->wdev->dev;
	int ret;

	if (b->mbox) {
		b->settings_input = devm_input_allocate_device(dev);
		if (!b->settings_input)
			return -ENOMEM;
		b->settings_input->name = "Xiaomi Book 14 Settings Key";
		b->settings_input->phys = DRV_NAME "/input0";
		b->settings_input->id.bustype = BUS_HOST;
		input_set_capability(b->settings_input, EV_KEY, KEY_CONFIG);
		ret = input_register_device(b->settings_input);
		if (ret)
			return dev_err_probe(dev, ret, "settings key input register failed\n");
	}

	/* Normally the firmware's own _Q0C/_Q0D notify the ACPI lid device. */
	if (ACPI_SUCCESS(acpi_get_handle(b->qec, "_Q0C", &b->q0c)) &&
	    ACPI_SUCCESS(acpi_get_handle(b->qec, "_Q0D", &b->q0d)))
		return 0;

	dev_info(dev, "lid AML methods missing, providing own lid switch\n");
	b->lid_input = devm_input_allocate_device(dev);
	if (!b->lid_input)
		return -ENOMEM;
	b->lid_input->name = "Xiaomi Book 14 Lid Switch";
	b->lid_input->phys = DRV_NAME "/input1";
	b->lid_input->id.bustype = BUS_HOST;
	input_set_capability(b->lid_input, EV_SW, SW_LID);
	ret = input_register_device(b->lid_input);
	if (ret)
		return dev_err_probe(dev, ret, "lid input register failed\n");
	return 0;
}

/* ---------- WMI driver ---------- */

static int book14_probe(struct wmi_device *wdev, const void *context)
{
	struct device *dev = &wdev->dev;
	struct device *hwmon;
	struct book14 *b;
	acpi_status status;
	u8 qfan;
	int ret;

	if (!dmi_check_system(book14_dmi_table)) {
		if (!force) {
			dev_info(dev, "not a Xiaomi Book Pro 14, not binding (force=1 overrides)\n");
			return -ENODEV;
		}
		dev_warn(dev, "unknown machine, binding because force=1\n");
	}

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->wdev = wdev;
	b->last_lid = STATE_UNKNOWN;
	b->last_qfan = STATE_UNKNOWN;
	b->cur_pl1 = -1;

	ret = devm_mutex_init(dev, &b->call_lock);
	if (!ret)
		ret = devm_mutex_init(dev, &b->rapl_lock);
	if (!ret)
		ret = devm_mutex_init(dev, &b->mbox_lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&b->ec_work, book14_ec_work);
	INIT_DEFERRABLE_WORK(&b->mbox_work, book14_mbox_work);
	dev_set_drvdata(dev, b);

	/* hardware fingerprint: the EC device the window/lid code relies on */
	status = acpi_get_handle(NULL, QEC_PATH, &b->qec);
	if (ACPI_FAILURE(status))
		return dev_err_probe(dev, -ENODEV, "%s not found, unsupported firmware\n",
				     QEC_PATH);

	/* firmware must understand the MIFS v2 perf-mode query */
	ret = book14_get_qfan(b, &qfan);
	if (ret)
		return dev_err_probe(dev, ret, "MIFS perf-mode query failed\n");
	dev_info(dev, "MIFS v2 online, QFAN=0x%02x\n", qfan);

	b->ec = devm_ioremap(dev, EC_MEM_BASE, EC_MEM_SIZE);
	if (!b->ec)
		return dev_err_probe(dev, -ENOMEM, "cannot map EC window %#x\n",
				     EC_MEM_BASE);

	book14_mbox_init(b);

	ret = book14_input_init(b);
	if (ret)
		return ret;

	hwmon = devm_hwmon_device_register_with_info(dev, "xiaomi_book14", b,
						     &book14_chip_info, NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(dev, PTR_ERR(hwmon), "hwmon register failed\n");

	b->pp_dev = devm_platform_profile_register(dev, DRV_NAME, b,
						   &book14_profile_ops);
	if (IS_ERR(b->pp_dev))
		return dev_err_probe(dev, PTR_ERR(b->pp_dev),
				     "platform_profile register failed\n");

	/*
	 * Everything the pollers touch is devm-managed and registered above,
	 * so (LIFO) the pollers are always stopped before any of it goes away.
	 */
	ret = devm_add_action_or_reset(dev, book14_stop_polling, b);
	if (ret)
		return ret;
	book14_start_polling(b);

	status = acpi_install_notify_handler(b->qec, ACPI_DEVICE_NOTIFY,
					     book14_ec_notify, b);
	if (ACPI_SUCCESS(status)) {
		ret = devm_add_action_or_reset(dev, book14_remove_notify, b);
		if (ret)
			return ret;
	} else {
		dev_dbg(dev, "Q_EC notify handler: %s\n",
			acpi_format_exception(status));
	}

	book14_kbd_init(b);
	book14_debugfs_init(b);

	return 0;
}

/*
 * Suspend/resume never fail (the mainline bitland driver aborts suspend here
 * because its profile_get returns -EINVAL for this firmware's QFAN codes).
 */
static int book14_suspend(struct device *dev)
{
	struct book14 *b = dev_get_drvdata(dev);

	book14_stop_polling(b);
	cancel_work_sync(&b->kbd_work);
	return 0;
}

static int book14_resume(struct device *dev)
{
	struct book14 *b = dev_get_drvdata(dev);

	b->last_lid = STATE_UNKNOWN;

	/* firmware may have restored its own power limits: re-apply ours */
	scoped_guard(mutex, &b->rapl_lock)
		b->cur_pl1 = -1;

	book14_start_polling(b);

	/* restore CapsLock LED without holding up system resume */
	if (b->kbd_handle)
		schedule_work(&b->kbd_work);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(book14_pm_ops, book14_suspend, book14_resume);

static const struct wmi_device_id book14_id_table[] = {
	{ XIAOMI_MIFS_METHOD_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, book14_id_table);

static struct wmi_driver book14_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = pm_sleep_ptr(&book14_pm_ops),
	},
	.id_table = book14_id_table,
	.probe = book14_probe,
	.no_singleton = true,
};
module_wmi_driver(book14_driver);

MODULE_AUTHOR("Yuming Sun <wolf109909@outlook.com>");
MODULE_DESCRIPTION("Xiaomi Book Pro 14 2026 (MIFS v2) platform driver: perf modes, lid, fan tach");
MODULE_LICENSE("GPL");
