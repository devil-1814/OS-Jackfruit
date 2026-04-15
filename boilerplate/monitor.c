#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1
#define CONTAINER_ID_LEN 32

/* ---------------- MONITOR ENTRY STRUCT ---------------- */

struct monitor_entry {

    pid_t pid;
    char container_id[CONTAINER_ID_LEN];

    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;

    bool soft_limit_triggered;

    struct list_head list;
};

static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_lock);
static struct timer_list monitor_timer;

/* ---------------- RSS HELPER ---------------- */

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }

    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------- TIMER CHECK ---------------- */

static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;

    mutex_lock(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {

        long rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss > entry->hard_limit_bytes) {

            struct task_struct *task;

            rcu_read_lock();
            task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);

            if (task)
                send_sig(SIGKILL, task, 1);

            rcu_read_unlock();

            printk(KERN_WARNING
                   "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld\n",
                   entry->container_id, entry->pid, rss);

            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (!entry->soft_limit_triggered &&
            rss > entry->soft_limit_bytes) {

            printk(KERN_WARNING
                   "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld\n",
                   entry->container_id, entry->pid, rss);

            entry->soft_limit_triggered = true;
        }
    }

    mutex_unlock(&monitored_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------- IOCTL ---------------- */

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        struct monitor_entry *entry;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        strncpy(entry->container_id, req.container_id, CONTAINER_ID_LEN);

        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;

        entry->soft_limit_triggered = false;

        mutex_lock(&monitored_lock);
        list_add(&entry->list, &monitored_list);
        mutex_unlock(&monitored_lock);

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);
    }

    return 0;
}

/* ---------------- DEVICE SETUP ---------------- */

static dev_t dev_number;
static struct class *monitor_class;
static struct cdev monitor_cdev;

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl
};

/* ---------------- MODULE INIT ---------------- */

static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);

    cdev_init(&monitor_cdev, &fops);
    cdev_add(&monitor_cdev, dev_number, 1);

    monitor_class = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(monitor_class, NULL, dev_number, NULL, DEVICE_NAME);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/container_monitor\n");

    return 0;
}

/* ---------------- MODULE EXIT ---------------- */

static void __exit monitor_exit(void)
{
    struct monitor_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitored_lock);

    device_destroy(monitor_class, dev_number);
    class_destroy(monitor_class);

    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_number, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

/* ---------------- MODULE REGISTRATION ---------------- */

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Container Memory Monitor Kernel Module");
