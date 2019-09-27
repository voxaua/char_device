#ifndef _PLATFORM_TEST_H_
#define _PLATFORM_TEST_H_

#define DUMMY_IO_BUFF_SIZE (5*1024)
struct plat_dummy_device {
	struct platform_device *pdev;
	void __iomem *mem;
	void __iomem *regs;
	struct delayed_work     dwork;
	struct workqueue_struct *data_read_wq;
	u64 js_pool_time;
	spinlock_t pool_lock;
	wait_queue_head_t rwq;	   /* read queues */
	wait_queue_head_t wwq;
	struct mutex rd_mutex;
	char buffer[DUMMY_IO_BUFF_SIZE];
	char *end;		   /* begin of buf, end of buf */
	char buffer_w[DUMMY_IO_BUFF_SIZE];
	char *end_w;
	char bw_status;
	int  bw_size_copied;
	u32 buffersize;				    /* used in pointer arithmetic */
	char *rp, *wp, *rp_w, *wp_w;			    /* where to read, where to write */
	ssize_t (*dummy_read) (struct plat_dummy_device *my_device,
			       char __user *buf, size_t count);
	ssize_t (*dummy_write) (struct plat_dummy_device *my_device,
				const char __user *bug, size_t count);
	int (*set_poll_interval) (struct plat_dummy_device *my_device,
				  u32 ms_interval);
};

enum dummy_dev {
	DUMMY_DEV_1,
	DUMMY_DEV_2,
	DUMMY_DEVICES
};

struct plat_dummy_device *get_dummy_platform_device(enum dummy_dev devnum);
#endif
