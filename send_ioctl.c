#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include "platform_cdev.h"

#define MAX_DEVICES	2
#define MIN_PULL_INTERVAL 10
#define MAX_PULL_INTERVAL 10000

extern int errno;

struct my_devs {
	const char *cdev;
};

static struct my_devs cdevs[MAX_DEVICES] = {
	{
		.cdev = "/dev/dummy/dummy0"
	},
	{
		.cdev = "/dev/dummy/dummy1"
	},
};

int usage(char **argv)
{
	printf("Program sends DUMMY_SET_POOLING ioctl to the specific device\n");
	printf("Usage: %s <device> <interval>", argv[0]);
	printf("Legal values for devices: 0,1. Legal interval in ms: 10 ~ 10000\n");
	return -1;
}

int main(int argc, char **argv)
{
	int fd;
	uint32_t interval, device;
	if (argc != 3) {
		return usage(argv);
	}

	device = atoi(argv[1]);
	if (device >= MAX_DEVICES)
		return usage(argv);

	interval = atoi(argv[2]);
	if ((interval < MIN_PULL_INTERVAL) || (interval > MAX_PULL_INTERVAL))
		return usage(argv);

	fd = open(cdevs[device].cdev, O_RDWR);
	if (fd < 0) {
		printf("file open error %s\n",cdevs[device].cdev);
		return -1;
	}
	return  ioctl(fd, DUMMY_SET_POOLING, &interval);
}

