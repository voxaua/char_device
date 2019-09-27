ifneq ($(KERNELRELEASE),)
#kbuild part of makefile
obj-m  := platform_cdev.o platform_test.o
else
#normal makefile
KDIR ?= /home/vivashchenko/Documents/renesas-bsp
CC = aarch64-linux-gnu-gcc

default:
	$(MAKE) -C $(KDIR) M=$$PWD

sender:
	$(CC) sender.c -o sender

send_ioctl:
	$(CC) send_ioctl.c -o send_ioctl

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean && rm -f sender send_ioctl
endif
