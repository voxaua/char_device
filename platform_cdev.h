#ifndef _PLATFORM_CDEV_H_
#define _PLATFORM_CDEV_H_

#include <linux/types.h>

#define DUMMY_IOC_MAGIC 'V'
#define DUMMY_IOC_MAXNR 0x01

#define DUMMY_SET_POOLING _IOW(DUMMY_IOC_MAGIC, 0x01, uint32_t)

#endif
