// SPDX-License-Identifier: GPL-2.0-only
/*
 * usbadb_gate - adb only while a non-dumb USB host (a PC) is attached.
 *
 * This driver only DETECTS and PUBLISHES. It runs nothing.
 *
 *   detect     /sys/class/udc/<udc>/state, delivered by the gadget state
 *              notifier (added by patches/0001). Only "configured" means a
 *              host sent SET_CONFIGURATION - a dumb charger never does, so it
 *              can never be configured. The state arrives in the event; it is
 *              not re-read on the decision path.
 *
 *   publish    /dev/usbadb_gate blocks until the verdict changes, then returns
 *              one line: "on configured 7".  /sys/kernel/usbadb_gate/link has
 *              the same verdict for one-shot reads.
 *
 *   act        someone else. patches/usbadb_gate.rc adds one trigger to
 *              KernelSU-Next's KERNEL_SU_RC, so init forks the reader itself:
 *
 *                  exec_background u:r:ksu:s0 root -- sh -c "..."
 *
 *              ...and it runs setprop plus start/stop adbd on each verdict.
 *
 *              Note the shape: one token, so init's tokenizer sees a single
 *              argument. It must be exec_background, not exec: exec sets
 *              SVC_EXEC and leaves Service::is_exec_service_running() true
 *              while the child lives, and this child never exits.
 *
 * Why not run the tools here: measured on a device. A child spawned from a
 * workqueue gets its tools executed (exit 0, even with SELinux permissive) and
 * init acts on neither the property write nor the service control message. The
 * same commands work from a caller init accepts. So the kernel decides and a
 * legitimate caller acts.
 *
 * Event driven throughout: the notifier is edge-triggered, and read() sleeps in
 * a wait queue. No timer, no polling, no periodic wakeup.
 *
 * Control: /sys/kernel/usbadb_gate/state
 *            pc  follow the cable (default)
 *            1   forced on
 *            0   forced off
 * Read:    /sys/kernel/usbadb_gate/link   "adb=%d udc=%s src=%s events=%lu mode=%s"
 */
#define pr_fmt(fmt) "usbadb_gate: " fmt

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/poll.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>

/* folded by Kbuild from CONFIG_USBADB_GATE_UDC_STATE */
const char usbadb_gate_state_path[] = USBADB_GATE_STATE_PATH;

/* line length, including the trailing NUL: "off not attached 4294967295\n\0" */
#define USBADB_GATE_LINE 48

/* auto = follow the cable, 0/1 = forced */
static int usbadb_gate_mode = -1;

/* last published verdict, so only real changes are published */
static int usbadb_gate_applied = -1;

/* the decision input: delivered by the notifier, never read back */
static int usbadb_gate_gadget_state = -1;

/* trigger events seen; the first thing to look at when nothing happens */
static unsigned long usbadb_gate_events;

/* /dev/usbadb_gate, the hand-off */
static DECLARE_WAIT_QUEUE_HEAD(usbadb_gate_wait);
static char usbadb_gate_line[USBADB_GATE_LINE] = "off unknown 0\n";
static size_t usbadb_gate_len = sizeof("off unknown 0\n") - 1;   /* without NUL */
static unsigned int usbadb_gate_seq;

/*
 * Read -> decide -> publish is one step in process context. A notifier callback
 * may only queue the work: it can hold locks that exec-free code must not block
 * under, and it must not block at all.
 */
static DEFINE_MUTEX(usbadb_gate_lock);
static struct work_struct usbadb_gate_work;
static struct kobject *usbadb_gate_kobj;

static void usbadb_gate_queue(void)
{
    schedule_work(&usbadb_gate_work);
}

/*
 * Publish the verdict and wake the reader. Called with the lock held, so two
 * verdicts can never interleave in the buffer.
 */
static void usbadb_gate_publish(int on, const char *state)
{
    usbadb_gate_seq++;
    usbadb_gate_len = snprintf(usbadb_gate_line, sizeof(usbadb_gate_line),
                               "%s %s %u\n", on ? "on" : "off", state,
                               usbadb_gate_seq);
    wake_up_interruptible(&usbadb_gate_wait);
}

/* read the sysfs node, for the case where no event has arrived yet */
static int usbadb_gate_read_state(char *buf, size_t len)
{
    struct file *f;
    ssize_t n;

    f = filp_open(usbadb_gate_state_path, O_RDONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    n = kernel_read(f, buf, len - 1, &f->f_pos);
    filp_close(f, NULL);
    if (n < 0)
        return n;

    buf[n] = '\0';
    strim(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the decision                                                       */
/* ------------------------------------------------------------------ */

static void usbadb_gate_work_fn(struct work_struct *work)
{
    char state[32];
    int on, err;

    (void)work;

    mutex_lock(&usbadb_gate_lock);

    if (usbadb_gate_gadget_state >= 0) {
        /* authoritative: the state came with the event */
        on = usbadb_gate_gadget_state == USB_STATE_CONFIGURED;
        strscpy(state, usb_state_string(usbadb_gate_gadget_state), sizeof(state));
    } else {
        /*
         * No event yet (cable already in at boot). Best effort: this read is
         * refused in kernel context on some devices, and "refused" must mean
         * unknown - never "unplugged".
         */
        err = usbadb_gate_read_state(state, sizeof(state));
        if (err) {
            strscpy(state, "unknown", sizeof(state));
            pr_warn_once("%s unreadable here (%d), waiting for the first event\n",
                         usbadb_gate_state_path, err);
            on = usbadb_gate_applied > 0;
        } else {
            on = !strcmp(state, "configured");
        }
    }

    if (usbadb_gate_mode >= 0)
        on = usbadb_gate_mode;

    if (on != usbadb_gate_applied) {
        usbadb_gate_applied = on;
        /* wake the reader first: it is the only one that can make it happen */
        usbadb_gate_publish(on, state);
        pr_info("udc=%s -> adb %s (was %s), published\n", state,
                on ? "on" : "off",
                usbadb_gate_applied < 0 ? "unset" : on ? "off" : "on");
    }

    mutex_unlock(&usbadb_gate_lock);
}

/* ------------------------------------------------------------------ */
/* trigger: the gadget state notifier                                 */
/* ------------------------------------------------------------------ */

static int usbadb_gate_udc_notify(struct notifier_block *nb,
                                  unsigned long state, void *data)
{
    (void)nb;
    (void)data;

    WRITE_ONCE(usbadb_gate_gadget_state, (int)state);
    usbadb_gate_events++;
    usbadb_gate_queue();

    return NOTIFY_OK;
}

static struct notifier_block usbadb_gate_udc_nb = {
    .notifier_call = usbadb_gate_udc_notify,
};

/* ------------------------------------------------------------------ */
/* /sys/kernel/usbadb_gate/{state,link}                               */
/* ------------------------------------------------------------------ */

static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
                          char *buf)
{
    (void)kobj;
    (void)attr;

    if (usbadb_gate_mode < 0)
        return sysfs_emit(buf, "pc\n");
    return sysfs_emit(buf, "%d\n", usbadb_gate_mode);
}

static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
    (void)kobj;
    (void)attr;

    if (sysfs_streq(buf, "0"))
        usbadb_gate_mode = 0;
    else if (sysfs_streq(buf, "1"))
        usbadb_gate_mode = 1;
    else if (!strncmp(buf, "pc", 2))
        usbadb_gate_mode = -1;
    else
        return -EINVAL;

    /* decide again now, without pretending an event arrived */
    usbadb_gate_gadget_state = -1;
    usbadb_gate_queue();

    return count;
}

static ssize_t link_show(struct kobject *kobj, struct kobj_attribute *attr,
                         char *buf)
{
    char state[32];
    const char *src;

    (void)kobj;
    (void)attr;

    if (READ_ONCE(usbadb_gate_gadget_state) >= 0) {
        strscpy(state, usb_state_string(READ_ONCE(usbadb_gate_gadget_state)),
                sizeof(state));
        src = "event";
    } else if (!usbadb_gate_read_state(state, sizeof(state))) {
        src = "sysfs";
    } else {
        strscpy(state, "unknown", sizeof(state));
        src = "none";
    }

    return sysfs_emit(buf, "adb=%d udc=%s src=%s events=%lu mode=%s\n",
                      READ_ONCE(usbadb_gate_applied) > 0 ? 1 : 0, state, src,
                      READ_ONCE(usbadb_gate_events),
                      usbadb_gate_mode < 0 ? "pc" : usbadb_gate_mode ? "1" : "0");
}

static struct kobj_attribute usbadb_gate_state_attr =
    __ATTR(state, 0644, state_show, state_store);
static struct kobj_attribute usbadb_gate_link_attr = __ATTR_RO(link);

static struct attribute *usbadb_gate_attrs[] = {
    &usbadb_gate_state_attr.attr,
    &usbadb_gate_link_attr.attr,
    NULL,
};

static const struct attribute_group usbadb_gate_attr_group = {
    .attrs = usbadb_gate_attrs,
};

/* ------------------------------------------------------------------ */
/* /dev/usbadb_gate: one line per verdict, blocking read              */
/* ------------------------------------------------------------------ */

/*
 * f_pos has to mean something for a device that returns lines rather than a
 * byte stream, and read() has to honour whatever count it is handed: the shell
 * read() builtin asks a few bytes at a time to find the delimiter, and a driver
 * that refuses anything under one line turns that into -EINVAL. That is not a
 * theoretical concern - it is how the first on-device run failed, with `cat`
 * working (64 KiB buffer) and `read` reporting "Invalid argument".
 *
 * So f_pos is packed: the high bits are the sequence number of the next line to
 * deliver, the low bits are how many bytes of it have already gone out.
 *
 *   next > seq   caught up, block
 *   next == seq  that line's bytes from `off` onwards are available
 *   next < seq   fell behind; the newest line wins and delivery restarts at 0
 *
 * A line is at most USBADB_GATE_LINE bytes, so six low bits are plenty.
 */
#define USBADB_GATE_SHIFT 6
#define USBADB_GATE_MASK  ((1U << USBADB_GATE_SHIFT) - 1)

static unsigned int usbadb_gate_next(struct file *f)
{
    return (unsigned int)(READ_ONCE(f->f_pos) >> USBADB_GATE_SHIFT);
}

static size_t usbadb_gate_off(struct file *f)
{
    return (size_t)(READ_ONCE(f->f_pos) & USBADB_GATE_MASK);
}

static void usbadb_gate_set_pos(struct file *f, unsigned int next, size_t off)
{
    WRITE_ONCE(f->f_pos, ((loff_t)next << USBADB_GATE_SHIFT) | (loff_t)off);
}

/* would read() return bytes right now, or block? */
static bool usbadb_gate_ready(struct file *f)
{
    unsigned int next = usbadb_gate_next(f);
    unsigned int seq = READ_ONCE(usbadb_gate_seq);

    if (next < seq)
        return true;                            /* behind: latest wins */
    if (next > seq)
        return false;                           /* caught up */
    return usbadb_gate_off(f) < READ_ONCE(usbadb_gate_len);
}

static ssize_t usbadb_gate_dev_read(struct file *f, char __user *ubuf,
                                    size_t len, loff_t *ppos)
{
    unsigned int next, seq;
    size_t off, have, n;
    ssize_t ret;

    (void)ppos;                    /* f_pos is the same thing, used directly */

    if (!len)
        return 0;

    for (;;) {
        if (usbadb_gate_ready(f))
            break;
        if (f->f_flags & O_NONBLOCK)
            return -EAGAIN;
        ret = wait_event_interruptible(usbadb_gate_wait, usbadb_gate_ready(f));
        if (ret)
            return ret;            /* a signal is not an event */
    }

    mutex_lock(&usbadb_gate_lock);

    next = usbadb_gate_next(f);
    off = usbadb_gate_off(f);
    seq = READ_ONCE(usbadb_gate_seq);

    if (next < seq) {
        /* the line this fd was part-way through has been replaced, so a
         * half-delivered line is abandoned rather than spliced onto a new one */
        next = seq;
        off = 0;
    }
    if (next > seq) {              /* a second reader raced a publish */
        mutex_unlock(&usbadb_gate_lock);
        return -EAGAIN;
    }

    have = READ_ONCE(usbadb_gate_len);
    if (off > have)                /* the line got shorter under us */
        off = 0;

    n = min(len, have - off);
    if (n && copy_to_user(ubuf, usbadb_gate_line + off, n)) {
        mutex_unlock(&usbadb_gate_lock);
        return -EFAULT;
    }

    off += n;
    if (off >= have) {             /* this line is fully delivered */
        next++;
        off = 0;
    }
    usbadb_gate_set_pos(f, next, off);

    mutex_unlock(&usbadb_gate_lock);

    return n;
}

static __poll_t usbadb_gate_dev_poll(struct file *f, poll_table *wait)
{
    poll_wait(f, &usbadb_gate_wait, wait);

    return usbadb_gate_ready(f) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static int usbadb_gate_dev_open(struct inode *inode, struct file *f)
{
    /*
     * This single log line answers the question nothing else could: whether the
     * rc block's child ever reached the device at all. "the driver published a
     * verdict" and "someone was listening" are different facts, and only this
     * one tells them apart.
     */
    pr_info("/dev/usbadb_gate opened by pid %d (%.16s)\n",
            task_pid_nr(current), current->comm);

    /*
     * Start after the current verdict, not on it: a reader that appears after
     * an event has already been published should wait for the next one. The rc
     * reconciles with what is true now via the sysfs link.
     */
    usbadb_gate_set_pos(f, READ_ONCE(usbadb_gate_seq) + 1, 0);

    /*
     * nonseekable_open(), not an .llseek of our own: it is present in every
     * kernel this targets (5.4 through 6.12+), while the obvious alternative,
     * .llseek = no_llseek, was removed in 6.12. It clears FMODE_LSEEK|PREAD|
     * PWRITE but leaves f_pos alone, which is all this driver needs from it.
     */
    return nonseekable_open(inode, f);
}

static const struct file_operations usbadb_gate_dev_fops = {
    .owner  = THIS_MODULE,
    .open   = usbadb_gate_dev_open,
    .read   = usbadb_gate_dev_read,
    .poll   = usbadb_gate_dev_poll,
    /* no .llseek: usbadb_gate_dev_open() calls nonseekable_open() */
};

static struct miscdevice usbadb_gate_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "usbadb_gate",
    .fops  = &usbadb_gate_dev_fops,
    .mode  = 0600, /* root only; the reader runs as root */
};

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

/*
 * late_initcall, not module_init: the gadget may already be up, and we want any
 * stray event to have been registered for before the first decision.
 */
static int __init usbadb_gate_init(void)
{
    int ret;

    INIT_WORK(&usbadb_gate_work, usbadb_gate_work_fn);

    usbadb_gate_kobj = kobject_create_and_add("usbadb_gate", kernel_kobj);
    if (!usbadb_gate_kobj) {
        pr_err("cannot create /sys/kernel/usbadb_gate\n");
        return -ENOMEM;
    }

    ret = sysfs_create_group(usbadb_gate_kobj, &usbadb_gate_attr_group);
    if (ret) {
        pr_err("cannot create sysfs attributes: %d\n", ret);
        goto err_kobj;
    }

    ret = misc_register(&usbadb_gate_misc);
    if (ret) {
        pr_err("cannot register /dev/usbadb_gate: %d\n", ret);
        goto err_sysfs;
    }

    ret = usb_gadget_register_state_notifier(&usbadb_gate_udc_nb);
    if (ret) {
        pr_err("cannot register udc state notifier: %d\n", ret);
        goto err_misc;
    }

    pr_info("detect only: the verdict is published, patches/usbadb_gate.rc acts\n");
    pr_info("watch: gadget state notifier, edge triggered; no polling, no timers\n");
    pr_info("hand-off: /dev/usbadb_gate blocks until the verdict changes\n");
    pr_info("read: /sys/kernel/usbadb_gate/link, control: .../state\n");

    /* the first decision, now that everything is registered */
    usbadb_gate_queue();

    return 0;

err_misc:
    misc_deregister(&usbadb_gate_misc);
err_sysfs:
    sysfs_remove_group(usbadb_gate_kobj, &usbadb_gate_attr_group);
err_kobj:
    kobject_put(usbadb_gate_kobj);
    return ret;
}

/*
 * Built in, which is the normal case here, there is nothing to unload and no
 * exit to register. Guarded rather than unconditional: an unconditional
 * module_exit() makes the compiler parse a macro the build may not provide.
 */
#ifdef MODULE
static void __exit usbadb_gate_exit(void)
{
    usb_gadget_unregister_state_notifier(&usbadb_gate_udc_nb);
    cancel_work_sync(&usbadb_gate_work);

    misc_deregister(&usbadb_gate_misc);
    sysfs_remove_group(usbadb_gate_kobj, &usbadb_gate_attr_group);
    kobject_put(usbadb_gate_kobj);

    pr_info("removed\n");
}
module_exit(usbadb_gate_exit);
#endif

late_initcall(usbadb_gate_init);
