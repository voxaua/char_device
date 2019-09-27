#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <asm/io.h>
#include <linux/uaccess.h>
#include "platform_test.h"


#define DRV_NAME  "plat_dummy"

#define MEM_SIZE			(0x1000)
#define REG_SIZE			(8)
#define DEVICE_POOLING_TIME_MS		(5) /*500 ms*/

#define PLAT_IO_FLAG_REG		(0) /*Offset of flag register*/
#define PLAT_IO_SIZE_REG		(4) /*Offset of flag register*/
#define PLAT_IO_DATA_READY		(1) /*IO data ready flag */
#define PLAT_WRITE_READY		(1 << 1)
#define MAX_DUMMY_PLAT_THREADS		(1) /*Maximum amount of threads for this */


/*Device has 2 resources:
 * * 1) 4K of memory at address defined by dts - used for data transfer;
 * * 2) Two 32-bit registers at address (defined by dts)
 * *  2.1. Flag Register: @offset 0
 * *	bit 0: PLAT_IO_DATA_READY - set to 1 if data from device ready
 * *	other bits: reserved;
 * * 2.2. Data size Register @offset 4: - Contain data size from device
 * (0..4095);
 * */

/*Following has to be added to dts file to support it
 * *aliases {
 * *		dummy0 = &my_dummy1;
 * *		dummy1 = &my_dummy2;
 * *};
 * *
 * *my_dummy1: dummy@9f200000 {
 * *		compatible = "ti,plat_dummy";
 * *		reg = <0x9f200000 0x1000>,
 * *				<0x9f201000 0x8>;
 * *};
 * *
 * *my_dummy2: dummy@9f210000 {
 * *		compatible = "ti,plat_dummy";
 * *		reg = <0x9f210000 0x1000>,
 * *				<0x9f211000 0x8>;
 * *};
 * *
 * */
#define MEM_BASE_1	(0x88000000)
#define REG_BASE_1	(0x88001000)

#define MEM_BASE_2	(0x88010000)
#define REG_BASE_2	(0x88011000)

struct platform_device *pdev;

struct resource res_1[2] = {
	{
	.start	= MEM_BASE_1,
	.end	= MEM_BASE_1 + MEM_SIZE - 1,
	.name	= "dummy_mem_1",
	.flags	= IORESOURCE_MEM,
	},

	{
	.start	= REG_BASE_1,
	.end	= REG_BASE_1 + REG_SIZE - 1,
	.name	= "dummy_regs_1",
	.flags	= IORESOURCE_MEM,
	}
};

struct resource res_2[2] = {
	{
	.start	= MEM_BASE_2,
	.end	= MEM_BASE_2 + MEM_SIZE - 1,
	.name	= "dummy_mem_2",
	.flags	= IORESOURCE_MEM,
	},

	{
	.start	= REG_BASE_2,
	.end	= REG_BASE_2 + REG_SIZE - 1,
	.name	= "dummy_regs_2",
	.flags	= IORESOURCE_MEM,
	}
};

static struct plat_dummy_device *mydevs[DUMMY_DEVICES];

static u32 plat_dummy_mem_read8(struct plat_dummy_device *my_dev, u32 offset)
{
	return ioread8(my_dev->mem + offset);
}

static u32 plat_dummy_mem_write8(struct plat_dummy_device *my_dev, u32 offset,
				 u32 val)
{
	iowrite8(val, my_dev->mem + offset);
	return 0;
}

static u32 plat_dummy_reg_read32(struct plat_dummy_device *my_dev, u32 offset)
{
	return ioread32(my_dev->regs + offset);
}
static void plat_dummy_reg_write32(struct plat_dummy_device *my_dev, u32 offset
				   , u32 val)
{
	iowrite32(val, my_dev->regs + offset);
}

/* How much space is free? */
static int spacefree(struct plat_dummy_device *my_dev)
{
	if (my_dev->rp == my_dev->wp)
		return my_dev->buffersize - 1;
		return ((my_dev->rp + my_dev->buffersize - my_dev->wp) %
							my_dev->buffersize) - 1;
}

static ssize_t plat_dummy_read(struct plat_dummy_device *my_device,
			       char __user *buf, size_t count)
{
	if (!my_device)
		return -EFAULT;

	if (mutex_lock_interruptible(&my_device->rd_mutex))
		return -ERESTARTSYS;

//	pr_info("++%s: my_device->rp = %p, my_device->wp = %p\n", __func__,
//		my_device->rp, my_device->wp);

	while (my_device->rp == my_device->wp) { /* nothing to read */
		mutex_unlock(&my_device->rd_mutex); /* release the lock */
//		pr_info("\"%s\" reading: going to sleep\n", current->comm);
		if (wait_event_interruptible(my_device->rwq,
					     (my_device->rp != my_device->wp)))
			return -ERESTARTSYS;	/* signal: tell the fs layer to handle it */
		/* otherwise loop, but first reacquire the lock */
		if (mutex_lock_interruptible(&my_device->rd_mutex))
			return -ERESTARTSYS;
	}
	/* ok, data is there, return something */
//	pr_info("count = %zu, my_device->rp = %p, my_device->wp = %p\n", count,
//		my_device->rp, my_device->wp);

	if (my_device->wp > my_device->rp)
		count = min(count, (size_t)(my_device->wp - my_device->rp));
	else /* the write pointer has wrapped, return data up to dev->end */
		count = min(count, (size_t)(my_device->end - my_device->rp));

	if (copy_to_user(buf, my_device->rp, count)) {
		mutex_unlock (&my_device->rd_mutex);
		return -EFAULT;
	}

	my_device->rp += count;
	if (my_device->rp == my_device->end)
		my_device->rp = my_device->buffer; /* wrapped */
	mutex_unlock (&my_device->rd_mutex);

	pr_info("\"%s\" did read %li bytes\n",current->comm, (long)count);
	return count;
}

static ssize_t plat_dummy_write(struct plat_dummy_device *my_device,
				const char __user *buf, size_t count)
{
	int volume, n;

	if (!my_device)
		return -EFAULT;

	if (mutex_lock_interruptible(&my_device->rd_mutex))
		return -ERESTARTSYS;

	if (my_device->bw_status) {
		mutex_unlock(&my_device->rd_mutex);
		wait_event_interruptible(my_device->wwq, !(my_device->bw_status));
		if(mutex_lock_interruptible(&my_device->rd_mutex))
			return -ERESTARTSYS;
	}

	volume = min((int)MEM_SIZE, (int)count);
	n = copy_from_user(my_device->buffer_w, buf, volume);
	my_device->bw_status = 1;
	my_device->bw_size_copied = volume - n;
	mutex_unlock(&my_device->rd_mutex);

	return volume - n;
}

/*intervals in ms*/
#define MIN_PULL_INTERVAL 10
#define MAX_PULL_INTERVAL 10000

int set_poll_interval(struct plat_dummy_device *my_device, u32 ms_interval)
{
	pr_info("++%s(%p)\n", __func__, my_device);
	if (!my_device)
		return -EFAULT;

	if ((ms_interval < MIN_PULL_INTERVAL) 
	    || (ms_interval > MAX_PULL_INTERVAL)) {

		pr_err("%s: Value out of range %d\n", __func__, ms_interval);
		return -EFAULT;
	}
	spin_lock(&my_device->pool_lock);
	my_device->js_pool_time = msecs_to_jiffies(ms_interval);
	spin_unlock(&my_device->pool_lock);
	pr_info("%s: Setting Poliing Interval to %d ms\n", __func__, ms_interval);
	return 0;
}

static void plat_dummy_work(struct work_struct *work)
{
	struct plat_dummy_device *my_device;
	u32 i, size, status, count;
	u64 js_time;

	my_device = container_of(work, struct plat_dummy_device, dwork.work);

	spin_lock(&my_device->pool_lock);
	js_time = my_device->js_pool_time;
	spin_unlock(&my_device->pool_lock);

	status = plat_dummy_reg_read32(my_device, PLAT_IO_FLAG_REG);

	if (status & PLAT_IO_DATA_READY) {
		if (mutex_lock_interruptible(&my_device->rd_mutex))
			goto exit_wq;
		size = plat_dummy_reg_read32(my_device, PLAT_IO_SIZE_REG);

		if (size > MEM_SIZE)
			size = MEM_SIZE;

		count = min((size_t)size, (size_t)spacefree(my_device));

		if (count < size) {
			mutex_unlock(&my_device->rd_mutex);
			goto exit_wq;
		}

		for(i = 0; i < count; i++) {
			*my_device->wp++ = plat_dummy_mem_read8(my_device, i); /*my_device->residue + i*/
			if (my_device->wp == my_device->end)
			my_device->wp = my_device->buffer; /* wrapped */
		}

		mutex_unlock (&my_device->rd_mutex);
		wake_up_interruptible(&my_device->rwq);
		rmb();
		status &= ~PLAT_IO_DATA_READY;
		status ^= PLAT_WRITE_READY;
		plat_dummy_reg_write32(my_device, PLAT_IO_FLAG_REG, status);
	}

	if (status & PLAT_WRITE_READY) {
		if (my_device->bw_status) {
			if(mutex_lock_interruptible(&my_device->rd_mutex))
				goto exit_wq;
			for(i = 0; i < MEM_SIZE; i++) {
				plat_dummy_mem_write8(my_device, i,
						      my_device->buffer_w[i]);
			}
			plat_dummy_reg_write32(my_device, PLAT_IO_SIZE_REG,
					       MEM_SIZE);
			status ^= PLAT_IO_DATA_READY;
			status &= ~PLAT_WRITE_READY;
			plat_dummy_reg_write32(my_device, PLAT_IO_FLAG_REG,
					       status);
			my_device->bw_status = 0;
			plat_dummy_reg_write32(my_device, PLAT_IO_SIZE_REG,
					       my_device->bw_size_copied);
			wake_up_interruptible(&my_device->wwq);
			mutex_unlock(&my_device->rd_mutex);
		}
	}

exit_wq:
	queue_delayed_work(my_device->data_read_wq, &my_device->dwork, js_time);
}

static void dummy_init_data_buffer(struct plat_dummy_device *my_device)
{
	my_device->buffersize = DUMMY_IO_BUFF_SIZE;
	my_device->end = my_device->buffer + my_device->buffersize;
	my_device->rp = my_device->wp = my_device->buffer;
}

struct plat_dummy_device *get_dummy_platform_device(enum dummy_dev devnum)
{
	if (mydevs[devnum])
		return mydevs[devnum];

	return NULL;
}

EXPORT_SYMBOL(get_dummy_platform_device);

static int plat_dummy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct plat_dummy_device *my_device;
	struct resource *res;
	static int id = 0;
	rmb();

	my_device = devm_kzalloc(dev, sizeof(struct plat_dummy_device), GFP_KERNEL);
	if (!my_device)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	my_device->mem = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(my_device->mem))
		return PTR_ERR(my_device->mem);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	my_device->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(my_device->regs))
		return PTR_ERR(my_device->regs);
	platform_set_drvdata(pdev, my_device);
	pr_info("Memory mapped to %p\n", my_device->mem);
	pr_info("Registers mapped to %p\n", my_device->regs);
	/*Init data read WQ*/
	my_device->data_read_wq = alloc_workqueue(res->name,
	WQ_UNBOUND, MAX_DUMMY_PLAT_THREADS);
	if (!my_device->data_read_wq)
		return -ENOMEM;
	mutex_init(&my_device->rd_mutex);
	init_waitqueue_head(&my_device->rwq);
	init_waitqueue_head(&my_device->wwq);
	dummy_init_data_buffer(my_device);
	my_device->dummy_read = plat_dummy_read;
	my_device->dummy_write = plat_dummy_write;
	my_device->set_poll_interval = set_poll_interval;
	spin_lock_init(&my_device->pool_lock);
	my_device->bw_status = 0;
	my_device->bw_size_copied = 0;
	INIT_DELAYED_WORK(&my_device->dwork, plat_dummy_work);
	my_device->js_pool_time = msecs_to_jiffies(DEVICE_POOLING_TIME_MS);
	queue_delayed_work(my_device->data_read_wq, &my_device->dwork, 0);
	my_device->pdev = pdev;
	mydevs[id] = my_device;
	id++;
	plat_dummy_reg_write32(my_device, PLAT_IO_FLAG_REG, PLAT_WRITE_READY);

	return PTR_ERR_OR_ZERO(my_device->mem);
}

static int plat_dummy_remove(struct platform_device *pdev)
{
	struct plat_dummy_device *my_device = platform_get_drvdata(pdev);

	if (my_device->data_read_wq) {
		/* Destroy work Queue */
		cancel_delayed_work_sync(&my_device->dwork);
		destroy_workqueue(my_device->data_read_wq);
	}
	pr_info("Platform device has been removed.\n");
	return 0;
}

static struct platform_driver plat_dummy_driver = {
	.driver = {
			.name =	DRV_NAME,
		  },

	.probe =	plat_dummy_probe,
	.remove =	plat_dummy_remove,
};

static int __init plat_dummy_device_add(struct resource *res)
{
	int err;
	struct platform_device *pdev = NULL;

	pdev = platform_device_alloc(DRV_NAME, res[0].start);
	if (!pdev) {
		err = -ENOMEM;
		pr_err("Device allocation failed\n");
		goto exit;
	}

	err = platform_device_add_resources(pdev, res, 2);
	if (err) {
		pr_err("Device resource addition failed (%d)\n", err);
		goto exit_device_put;
	}
	err = platform_device_add(pdev);
	if (err) {
		pr_err("Device addition failed (%d)\n", err);
		goto exit_device_put;
	}
	pr_info("Platform device has been added.\n");
	return 0;

exit_device_put:
	platform_device_put(pdev);
exit:
	pdev = NULL;
	return err;
}

static int plat_dummy_driver_register(void)
{
	int res;

	res = platform_driver_register(&plat_dummy_driver);
	if (res)
		goto exit;

	res = plat_dummy_device_add(res_1);
	if (res)
		goto exit_unreg_driver;

	res = plat_dummy_device_add(res_2);
	if (res)
		goto exit_unreg_driver;

	return 0;

exit_unreg_driver:
	platform_driver_unregister(&plat_dummy_driver);
exit:
	return res;
}

static void plat_dummy_unregister(void)
{
	platform_device_unregister(mydevs[0]->pdev);
	platform_device_unregister(mydevs[1]->pdev);
	platform_driver_unregister(&plat_dummy_driver);
}

int __init plat_dummy_init_module(void)
{
	pr_err("Platform dummy test module init\n");
	return plat_dummy_driver_register();
}

void __exit plat_dummy_cleanup_module(void)
{
	pr_err("Platform dummy test module exit\n");
	plat_dummy_unregister();
}


module_init(plat_dummy_init_module);
module_exit(plat_dummy_cleanup_module);

MODULE_AUTHOR("Vitaliy Vasylskyy <vitaliy.vasylskyy@globallogic.com>");
MODULE_DESCRIPTION("Dummy platform driver");
MODULE_LICENSE("GPL");
