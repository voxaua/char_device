#include <linux/init.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/uio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>

#include <asm/uaccess.h>

#include "platform_test.h"
#include "platform_cdev.h"

struct my_dummy_cdev {
	struct  cdev cdev;
	struct	plat_dummy_device *my_device;
	struct	mutex mutex;
	int	dummy_major;
	atomic_t num_open;
};

struct class *dummy_class;

struct my_dummy_cdev dummy_cdevs[DUMMY_DEVICES];

ssize_t dummy_cdev_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	struct my_dummy_cdev *cdevice = filp->private_data;

	if (filp->f_flags & O_NONBLOCK)
		return -EAGAIN;

	if (cdevice->my_device && cdevice->my_device->dummy_read)
		return cdevice->my_device->dummy_read(cdevice->my_device, buf,
						      count);
	return -1;
}

ssize_t dummy_cdev_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	struct my_dummy_cdev *cdevice = filp->private_data;
	ssize_t bytes_written = 0;

	bytes_written = cdevice->my_device->dummy_write(cdevice->my_device,
							     buf, count);
	return bytes_written;
}

#define MAX_OPEN 2

static int dummy_cdev_open(struct inode *inode, struct file *filp)
{
	struct my_dummy_cdev *cdevice;
	const int minor = iminor(inode);

	pr_info("++%s(%d) point 1\n", __func__, minor);
	cdevice = container_of(inode->i_cdev, struct my_dummy_cdev, cdev);
	if (atomic_inc_return(&cdevice->num_open) > MAX_OPEN) {
		pr_err("Maximum anount of open request is exceeded\n");
		atomic_dec(&cdevice->num_open);
		return -ENODEV;
	}
	filp->private_data = cdevice;
	pr_info("++%s(%d) point 2 \n", __func__, minor);

	return nonseekable_open(inode, filp);
}

/*
 *   When the final fd which refers to this character-special node is closed,
 *   we make its ->mapping point back at its own i_data.
 *
 */
static int dummy_cdev_release(struct inode *inode, struct file *filp)
{
	struct my_dummy_cdev *cdevice;
	const int minor= iminor(inode);

	cdevice = container_of(inode->i_cdev, struct my_dummy_cdev, cdev);
	pr_info("++%s(%d)\n", __func__, minor);
	atomic_dec(&cdevice->num_open);

	return 0;
}

long dummy_cdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int err = 0;
	u32 interval;
	struct my_dummy_cdev *cdevice = filp->private_data;

	/* don't even decode wrong cmds: better
	 * returning  ENOTTY than EFAULT */
	if (_IOC_TYPE(cmd) != DUMMY_IOC_MAGIC)
		return -ENOTTY;

	if (_IOC_NR(cmd) > DUMMY_IOC_MAXNR)
		return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg,
				 _IOC_SIZE(cmd));

	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok(VERIFY_READ, (void __user *)arg,
				  _IOC_SIZE(cmd));

	if (err)
		return -EFAULT;

	switch(cmd) {
		case DUMMY_SET_POOLING:
			if (cdevice->my_device &&
			    cdevice->my_device->set_poll_interval) {

				err = __get_user(interval, (u32 __user *)arg);
				if (err)
					break;

				err = cdevice->my_device->set_poll_interval(cdevice->my_device,
									    interval);
			} else {
				err = -EINVAL;
			}
			break;

		default:  /* redundant, as cmd was checked against MAXNR */
			return -ENOTTY;
	}

	return err;
}


static char *dummy_cdev_node(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "dummy/%s", dev_name(dev));
}

static const struct file_operations dummy_cdev_fops = {
	.read	= dummy_cdev_read,
	.write	= dummy_cdev_write,
	.open	= dummy_cdev_open,
	.release	= dummy_cdev_release,
	.unlocked_ioctl = dummy_cdev_ioctl,
	.owner		= THIS_MODULE,
};


int dummy_major = 0; /*Just for info*/

static int __init dummy_cdev_init(void)
{
	dev_t dev = 0;
	int ret, i, devnum, minor = 0;

	if (dummy_major) {
		dev = MKDEV(dummy_major, 0);
		ret = register_chrdev_region(dev, DUMMY_DEVICES, "dummy_cdevs");
	} else {
		ret = alloc_chrdev_region(&dev, 0, DUMMY_DEVICES, "dummy_cdevs");
		dummy_major = MAJOR(dev);
	}

	if (ret < 0)
		return ret;

	/* Initialize each device. */
	for (i = 0; i < DUMMY_DEVICES; i++) {
		dummy_cdevs[i].dummy_major = dummy_major;
		mutex_init(&dummy_cdevs[i].mutex);
		dummy_cdevs[i].my_device = get_dummy_platform_device(i);
		if (!dummy_cdevs[i].my_device) {
			pr_err("can't get device%d\n", i);
			goto error_region;
		}
		devnum = MKDEV(dummy_major, i);
		cdev_init(&dummy_cdevs[i].cdev, &dummy_cdev_fops);
		ret = cdev_add (&dummy_cdevs[i].cdev, devnum, 1);
		/* Fail gracefully if need be */
		if (ret) {
			pr_err( "Error %d adding scull%d", ret, i);
			goto error_region;
		}
	}

	dummy_class = class_create(THIS_MODULE, "dummy");
	if (IS_ERR(dummy_class)) {
		pr_err("Error creating dummy class.\n");
		cdev_del(&dummy_cdevs[0].cdev);
		cdev_del(&dummy_cdevs[1].cdev);
		ret = PTR_ERR(dummy_class);
		goto error_region;
	}

	dummy_class->devnode = dummy_cdev_node;
	device_create(dummy_class, NULL, MKDEV(dummy_major, 0), NULL,
		      "dummy" "%d", minor++);
	device_create(dummy_class, NULL, MKDEV(dummy_major, 1), NULL,
		      "dummy" "%d", minor);
	return 0;

error_region:
	unregister_chrdev_region(dev, DUMMY_DEVICES);
	return ret;
}

static void __exit dummy_cdev_exit(void)
{
	int i;
	device_destroy(dummy_class, MKDEV(dummy_major, 0));
	device_destroy(dummy_class, MKDEV(dummy_major, 1));
	class_destroy(dummy_class);

	for (i = 0; i < DUMMY_DEVICES; i++) {
		cdev_del(&dummy_cdevs[i].cdev);
	}
	unregister_chrdev_region(MKDEV(dummy_major, 0), DUMMY_DEVICES);
}

module_init(dummy_cdev_init);
module_exit(dummy_cdev_exit);
MODULE_LICENSE("GPL");
