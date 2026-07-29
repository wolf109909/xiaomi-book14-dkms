// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Xiaomi Book Pro 14 2026 (Panther Lake, Bitland MIFS v2) platform driver.
 *
 * Reverse-engineered from the laptop's ACPI tables (DSDT/SSDT24 "WMID"
 * device, UID "MIFS") and the firmware's EC shared-memory window.
 *
 * Why this driver exists:
 *  - The mainline bitland-mifs-wmi driver targets different MIFS firmware:
 *    this machine's WMAA method only implements FUN2 0x0800/0x0A00/0x0C00/
 * 0x1000, perf-mode values are QFAN codes {2,3,4,9,10} (not 0..3), and it
 *    has no WMI fan-speed/CPU-temp functions at all.
 *  - The firmware never defines ECON, so \_SB.PC00.LPCB.Q_EC._STA fails and
 *    the ACPI EC driver never binds: all _Qxx handlers (lid, Fn+K, ...) are
 *    dead. We poll the EC window directly instead.
 *  - INTC10D4 (Intel DTT) has no Linux driver, so changing TDP must go
 *    through RAPL MSR writes.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_profile.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <asm/msr.h>

/* WMID XGNS mailbox (SSDT24): shared BIOS<->OS event block */
#define MBOX_PHYS		0x6FDB8000
#define MBOX_SIZE		0x2C
#define MBOX_OFF_EVTP		0x24
#define MBOX_OFF_EVFN		0x25
#define EC_GPE			0x6E

#define DRV_NAME		"xiaomi-book14"
#define XIAOMI_MIFS_METHOD_GUID	"B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B"
#define XIAOMI_MIFS_EVENT_GUID	"46C93E13-EE9B-4262-8488-563BCA757FEF"

/* WMAA protocol (SSDT24): 32-byte in/out, little-endian */
#define MIFS_OP_GET		0xFA00
#define MIFS_OP_SET		0xFB00
#define MIFS_FN_PERF_MODE	0x0800
#define MIFS_STATUS_OK		0x8000

/* EC shared-memory window (DSDT \_SB.PC00.LPCB.Q_EC, region ERAM) */
#define EC_MEM_BASE		0xFE0B0300
#define EC_MEM_SIZE		0x100
#define EC_REG_LID		0x14	/* bit0: LSTE, 1 = lid open */
#define EC_LID_OPEN		BIT(0)
#define EC_REG_QFAN		0x68	/* perf mode set by FW/EC */
#define EC_REG_FAN1		0x69	/* u16 LE, RPM, 0 when stopped */
#define EC_REG_FAN2		0x6B	/* u16 LE, RPM */

/* XGNS mailbox event codes (EC writes these directly; matches QV20 in SSDT24) */
#define MBOX_EVTP_HOTKEY	0x01
#define MBOX_EVFN_SETTINGS_PRESS	0x25	/* dedicated settings key down (_Q15) */
#define MBOX_EVFN_SETTINGS_RELEASE	0x26	/* dedicated settings key up (_Q16) */

/* Perf modes (QFAN values, from SSDT24 WMAA/EV20) */
#define QFAN_QUIET		0x02
#define QFAN_BALANCED		0x03
#define QFAN_SPEED		0x04
#define QFAN_EXTREME		0x09
#define QFAN_EXTREME2		0x0A

/* RAPL MSRs come from asm/msr-index.h via asm/msr.h */

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

/* per-profile PL1 in mW, tunable at load time */
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
MODULE_PARM_DESC(poll_ms, "EC poll interval (lid switch, perf-mode watch)");

static uint mbox_poll_ms = 50;
module_param(mbox_poll_ms, uint, 0644);
MODULE_PARM_DESC(mbox_poll_ms, "XGNS mailbox poll interval (hotkeys)");

static u8 __iomem *ec_mem;
static struct device *hwmon_dev;
static struct device *pp_dev;
static struct delayed_work ec_poll_work;
static struct delayed_work mbox_poll_work;
static struct mutex call_lock;
static u8 last_lid = 0xff;
static u8 last_qfan = 0xff;
static int cur_pl1 = -1;
static void __iomem *mbox;
static u8 last_mbox_evfn;
static acpi_handle qec_handle;
static struct input_dev *settings_input;
static bool settings_down;

/* ---------- WMAA ---------- */

static struct wmi_device *mifs_wdev;

static int mifs_call(u16 op, u16 fun2, u16 fun3, u32 fun4, struct mifs_out *out)
{
	struct mifs_in in = {
		.fun1 = cpu_to_le16(op),
		.fun2 = cpu_to_le16(fun2),
		.fun3 = cpu_to_le16(fun3),
		.fun4 = cpu_to_le32(fun4),
	};
	struct wmi_buffer in_buf = { .length = sizeof(in), .data = &in };
	struct wmi_buffer out_buf = { 0 };
	struct mifs_out res;
	int ret;

	if (!mifs_wdev)
		return -ENODEV;

	guard(mutex)(&call_lock);
	ret = wmidev_invoke_method(mifs_wdev, 0, 1, &in_buf, &out_buf, sizeof(res));
	if (ret)
		return ret;

	memcpy(&res, out_buf.data, sizeof(res));
	kfree(out_buf.data);

	if (le16_to_cpu(res.sger) != MIFS_STATUS_OK) {
		pr_debug("WMAA %04x/%04x failed, sger=%04x\n", op, fun2,
			 le16_to_cpu(res.sger));
		return -EIO;
	}
	if (out)
		*out = res;
	return 0;
}

/* ---------- RAPL ---------- */

static void rapl_set_pl1(int mw)
{
	u64 unit_msr, limit_msr, pl2;
	u32 power_unit_uw;
	int ret;

	if (mw == cur_pl1 || mw <= 0)
		return;

	ret = rdmsrq_safe(MSR_RAPL_POWER_UNIT, &unit_msr);
	if (ret)
		return;
	/* PU = 1/2^(bits 3:0) W -> microwatts per unit */
	power_unit_uw = 1000000 >> (unit_msr & 0xf);
	if (!power_unit_uw)
		return;

	ret = rdmsrq_safe(MSR_PKG_POWER_LIMIT, &limit_msr);
	if (ret)
		return;

	/* PL1 in bits 14:0, PL2 in bits 46:32; keep other bits */
	pl2 = (u64)(mw + mw / 4) * 1000 / power_unit_uw;
	limit_msr &= ~((u64)0x7fff | ((u64)0x7fff << 32));
	limit_msr |= ((u64)(mw * 1000 / power_unit_uw) & 0x7fff);
	limit_msr |= (pl2 & 0x7fff) << 32;

	if (!wrmsrq_safe(MSR_PKG_POWER_LIMIT, limit_msr))
		cur_pl1 = mw;
}

/* ---------- platform profile ---------- */

static int pp_to_qfan(enum platform_profile_option p, u16 *v)
{
	switch (p) {
	case PLATFORM_PROFILE_LOW_POWER:	*v = QFAN_QUIET;	break;
	case PLATFORM_PROFILE_BALANCED:		*v = QFAN_BALANCED;	break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE: *v = QFAN_SPEED;	break;
	case PLATFORM_PROFILE_PERFORMANCE:	*v = QFAN_EXTREME;	break;
	default: return -EOPNOTSUPP;
	}
	return 0;
}

static enum platform_profile_option qfan_to_pp(u8 q)
{
	switch (q) {
	case QFAN_QUIET:	return PLATFORM_PROFILE_LOW_POWER;
	case QFAN_BALANCED:	return PLATFORM_PROFILE_BALANCED;
	case QFAN_SPEED:	return PLATFORM_PROFILE_BALANCED_PERFORMANCE;
	case QFAN_EXTREME:
	case QFAN_EXTREME2:	return PLATFORM_PROFILE_PERFORMANCE;
	default:		return PLATFORM_PROFILE_BALANCED;
	}
}

static int qfan_to_pl1(u8 q)
{
	switch (qfan_to_pp(q)) {
	case PLATFORM_PROFILE_LOW_POWER:	return pl1_low_power;
	case PLATFORM_PROFILE_BALANCED:		return pl1_balanced;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE: return pl1_balanced_perf;
	default:				return pl1_performance;
	}
}

static int book14_profile_get(struct device *dev,
			      enum platform_profile_option *profile)
{
	struct mifs_out out;
	int ret = mifs_call(MIFS_OP_GET, MIFS_FN_PERF_MODE, 0, 0, &out);

	if (ret)
		return ret;
	*profile = qfan_to_pp(le16_to_cpu(out.frd0) & 0xff);
	return 0;
}

static int book14_profile_set(struct device *dev,
			      enum platform_profile_option profile)
{
	u16 v;
	int ret = pp_to_qfan(profile, &v);

	if (ret)
		return ret;
	ret = mifs_call(MIFS_OP_SET, MIFS_FN_PERF_MODE, v, 0, NULL);
	if (ret)
		return ret;
	rapl_set_pl1(qfan_to_pl1(v));
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

static umode_t book14_hwmon_visible(const void *data, enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	return 0444;
}

static int book14_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	u16 off;

	if (type != hwmon_fan)
		return -EINVAL;
	off = channel ? EC_REG_FAN2 : EC_REG_FAN1;
	*val = get_unaligned_le16(ec_mem + off);
	return 0;
}

static int book14_hwmon_read_string(struct device *dev,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel, const char **str)
{
	if (type == hwmon_fan && attr == hwmon_fan_label) {
		*str = channel ? "Right Fan" : "Left Fan";
		return 0;
	}
	return -EINVAL;
}

static const struct hwmon_channel_info *book14_hwmon_info[] = {
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
static void ec_poll(struct work_struct *work)
{
	u8 lid, qfan;
	acpi_status status;

	lid = readb(ec_mem + EC_REG_LID) & EC_LID_OPEN;
	if (lid != last_lid && last_lid != 0xff) {
		pr_info("lid %s\n", lid ? "opened" : "closed");
		status = acpi_evaluate_object(NULL,
				lid ? "\\_SB.PC00.LPCB.Q_EC._Q0D"
				    : "\\_SB.PC00.LPCB.Q_EC._Q0C",
				NULL, NULL);
		if (ACPI_FAILURE(status)) {
			/* methods may be gated by ECAV; notify LID0 directly */
			acpi_evaluate_object(NULL,
				"\\_SB.PC00.LPCB.Q_EC.LID0._LID", NULL, NULL);
			acpi_bus_generate_netlink_event(
				"button/lid", "PNP0C0D:00", 0x80, lid);
		}
	}
	last_lid = lid;

	qfan = readb(ec_mem + EC_REG_QFAN);
	if (qfan != last_qfan && last_qfan != 0xff) {
		pr_info("perf mode changed to 0x%02x (Fn+K or EC)\n", qfan);
		rapl_set_pl1(qfan_to_pl1(qfan));
		if (pp_dev)
			platform_profile_notify(pp_dev);
	}
	last_qfan = qfan;

	schedule_delayed_work(&ec_poll_work, msecs_to_jiffies(poll_ms));
}

/*
 * Hotkey mailbox poll: separate, faster worker. The EC only holds
 * EVFN for the duration of the physical press, so the 250ms EC poll
 * would skip quick taps; 50ms catches them.
 */
static void mbox_poll(struct work_struct *work)
{
	if (mbox) {
		u8 evtp = readb(mbox + MBOX_OFF_EVTP);
		u8 evfn = readb(mbox + MBOX_OFF_EVFN);

		if (evfn != last_mbox_evfn) {
			pr_info("mbox event: EVTP=%02x EVFN=%02x\n", evtp, evfn);
			last_mbox_evfn = evfn;

			if (settings_input && evtp == MBOX_EVTP_HOTKEY) {
				if (evfn == MBOX_EVFN_SETTINGS_PRESS) {
					input_report_key(settings_input, KEY_CONFIG, 1);
					input_sync(settings_input);
					settings_down = true;
				} else if (evfn == MBOX_EVFN_SETTINGS_RELEASE) {
					if (!settings_down) {
						/* tap fully inside one poll window: synth press */
						input_report_key(settings_input, KEY_CONFIG, 1);
						input_sync(settings_input);
					}
					input_report_key(settings_input, KEY_CONFIG, 0);
					input_sync(settings_input);
					settings_down = false;
				}
			}
		}
	}

	schedule_delayed_work(&mbox_poll_work, msecs_to_jiffies(mbox_poll_ms));
}

/* ---------- Q_EC notify: EC query events (hotkeys etc.) ---------- */

static void book14_ec_notify(acpi_handle handle, u32 event, void *context)
{
	pr_info("Q_EC notify 0x%02x\n", event);
}

/* ---------- debugfs: raw mode control (verification/tuning) ---------- */

static struct dentry *dbg_dir;

static ssize_t qfan_mode_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	u8 v;
	int ret;

	ret = kstrtou8_from_user(ubuf, count, 0, &v);
	if (ret)
		return ret;
	ret = mifs_call(MIFS_OP_SET, MIFS_FN_PERF_MODE, v, 0, NULL);
	if (ret)
		return ret;
	return count;
}

static int qfan_mode_show(struct seq_file *s, void *unused)
{
	struct mifs_out out;

	if (!mifs_call(MIFS_OP_GET, MIFS_FN_PERF_MODE, 0, 0, &out))
		seq_printf(s, "0x%02x\n", le16_to_cpu(out.frd0) & 0xff);
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
	if (!settings_input)
		return -ENODEV;
	input_report_key(settings_input, KEY_CONFIG, 1);
	input_sync(settings_input);
	input_report_key(settings_input, KEY_CONFIG, 0);
	input_sync(settings_input);
	return count;
}
static const struct file_operations settings_key_test_fops = {
	.owner = THIS_MODULE,
	.write = settings_key_test_write,
};

/* ---------- WMI driver ---------- */

static int book14_probe(struct wmi_device *wdev, const void *context)
{
	struct mifs_out out;
	int ret;

	mifs_wdev = wdev;

	/* sanity: firmware must understand the perf-mode query */
	ret = mifs_call(MIFS_OP_GET, MIFS_FN_PERF_MODE, 0, 0, &out);
	if (ret) {
		dev_err(&wdev->dev, "MIFS perf-mode query failed: %d\n", ret);
		return ret;
	}
	dev_info(&wdev->dev, "MIFS v2 online, QFAN=0x%02x\n",
		 le16_to_cpu(out.frd0) & 0xff);

	pp_dev = devm_platform_profile_register(&wdev->dev, DRV_NAME, NULL,
						&book14_profile_ops);
	if (IS_ERR(pp_dev))
		return dev_err_probe(&wdev->dev, PTR_ERR(pp_dev),
				     "platform_profile register failed\n");

	ec_mem = ioremap(EC_MEM_BASE, EC_MEM_SIZE);
	if (!ec_mem) {
		dev_err(&wdev->dev, "cannot map EC window %#x\n", EC_MEM_BASE);
		return -ENOMEM;
	}

	hwmon_dev = devm_hwmon_device_register_with_info(&wdev->dev, "xiaomi_book14",
							 NULL, &book14_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		iounmap(ec_mem);
		return dev_err_probe(&wdev->dev, PTR_ERR(hwmon_dev),
				     "hwmon register failed\n");
	}

	mutex_init(&call_lock);
	INIT_DELAYED_WORK(&ec_poll_work, ec_poll);
	schedule_delayed_work(&ec_poll_work, msecs_to_jiffies(poll_ms));
	INIT_DELAYED_WORK(&mbox_poll_work, mbox_poll);
	schedule_delayed_work(&mbox_poll_work, msecs_to_jiffies(mbox_poll_ms));

	mbox = memremap(MBOX_PHYS, MBOX_SIZE, MEMREMAP_WB);
	if (mbox)
		pr_info("mailbox: EVTP=%02x EVFN=%02x\n",
			readb(mbox + MBOX_OFF_EVTP), readb(mbox + MBOX_OFF_EVFN));

	/* settings key: EC drops EVFN 0x25/0x26 in the XGNS mailbox */
	settings_input = devm_input_allocate_device(&wdev->dev);
	if (!settings_input) {
		cancel_delayed_work_sync(&mbox_poll_work);
		cancel_delayed_work_sync(&ec_poll_work);
		if (mbox)
			memunmap(mbox);
		iounmap(ec_mem);
		return -ENOMEM;
	}
	settings_input->name = "Xiaomi Book 14 Settings Key";
	settings_input->phys = DRV_NAME "/input0";
	settings_input->id.bustype = BUS_HOST;
	settings_input->id.vendor = 0x1D6B; /* Linux Foundation (no Xiaomi VID) */
	input_set_capability(settings_input, EV_KEY, KEY_CONFIG);
	ret = input_register_device(settings_input);
	if (ret) {
		cancel_delayed_work_sync(&mbox_poll_work);
		cancel_delayed_work_sync(&ec_poll_work);
		if (mbox)
			memunmap(mbox);
		iounmap(ec_mem);
		return dev_err_probe(&wdev->dev, ret, "input register failed\n");
	}

	if (!ACPI_FAILURE(acpi_get_handle(NULL, "\\_SB.PC00.LPCB.Q_EC",
					  &qec_handle))) {
		acpi_status st = acpi_install_notify_handler(qec_handle,
				ACPI_DEVICE_NOTIFY, book14_ec_notify, NULL);
		if (ACPI_FAILURE(st))
			pr_warn("Q_EC notify handler failed: %s\n",
				acpi_format_exception(st));
	}

	/* align RAPL with the current mode */
	rapl_set_pl1(qfan_to_pl1(readb(ec_mem + EC_REG_QFAN)));

	dbg_dir = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("qfan_mode", 0600, dbg_dir, NULL, &qfan_mode_fops);
	debugfs_create_file("settings_key_test", 0200, dbg_dir, NULL,
			    &settings_key_test_fops);

	return 0;
}

static void book14_remove(struct wmi_device *wdev)
{
	debugfs_remove_recursive(dbg_dir);
	if (qec_handle)
		acpi_remove_notify_handler(qec_handle, ACPI_DEVICE_NOTIFY,
					   book14_ec_notify);
	cancel_delayed_work_sync(&mbox_poll_work);
	cancel_delayed_work_sync(&ec_poll_work);
	if (mbox)
		memunmap(mbox);
	if (ec_mem)
		iounmap(ec_mem);
}

/*
 * Suspend/resume: never fail (mainline bitland driver aborts suspend here
 * because its profile_get returns -EINVAL for this firmware's QFAN codes).
 */
static int book14_suspend(struct device *dev)
{
	cancel_delayed_work_sync(&mbox_poll_work);
	cancel_delayed_work_sync(&ec_poll_work);
	return 0;
}

static int book14_resume(struct device *dev)
{
	last_lid = 0xff;
	last_qfan = 0xff;
	schedule_delayed_work(&ec_poll_work, msecs_to_jiffies(0));
	schedule_delayed_work(&mbox_poll_work, msecs_to_jiffies(0));
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
	.remove = book14_remove,
	.no_singleton = true,
};
module_wmi_driver(book14_driver);

MODULE_AUTHOR("reverse-engineered for Xiaomi Book Pro 14 2026");
MODULE_DESCRIPTION("Xiaomi Book Pro 14 2026 (MIFS v2) platform driver: perf modes, lid, fan tach");
MODULE_LICENSE("GPL");
