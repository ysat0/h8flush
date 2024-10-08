/*
 *  Renesas CPU On-chip Flash memory writer
 *  USB I/O
 *
 * Yoshinori Sato <ysato@users.sourceforge.jp>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License version 2.1 (or later).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#ifdef HAVE_USB_H
#include <usb.h>
#include "h8flash.h"

#define USB_TIMEOUT 100000

static struct usb_dev_handle *handle;
static char target[32];

/* 
EP1: bulk out
EP2: bulk in
*/

/* send byte stream */
static int send_data(const unsigned char *buf, int len)
{
	return usb_bulk_write(handle, 0x01, (const char *)buf, len, USB_TIMEOUT);
}

/* receive 1byte */ 
static int read_byte(unsigned char *data)
{
	static unsigned char buf[64];
	static unsigned char *rp;
	static int count = 0;

	if (count == 0) {
		/* refilling */
		int r = usb_bulk_read(handle, 0x82, (char *)buf, 64, USB_TIMEOUT);
		if (r < 0)
			return r;
		count = r;
		rp = buf;
	}
	count--;
	*data = *rp++;
	return 1;
}

/* connect to target CPU */
static int connect_target(char *port)
{
	unsigned char req = 0x55;
	int r;
	printf("now connecting to %s", port); 
	fflush(stdout);
	usb_bulk_write(handle, 0x01, (const char *)&req, 1, USB_TIMEOUT);
	do {
		putchar('.');
		fflush(stdout);
		r = usb_bulk_read(handle, 0x82, (char *)&req, 1, USB_TIMEOUT);
		if (r == 0)
			usleep(100000);
	} while (r == 0);
	putchar('\n');
	if (r < 0 || req != 0xe6)
		return 0;
	else
		return req;
}

static void port_close(void)
{
	usb_close(handle);
}

static struct port_t usb_port = {
	.type = usb,
	.dev = target,
	.send_data = send_data,
	.receive_byte = read_byte,
	.connect_target = connect_target,
	.setbaud = NULL,
	.close = port_close,
};

/* USB port open */
struct port_t *open_usb(unsigned short vid, unsigned short pid)
{
	struct usb_bus *busses;
	struct usb_bus *bus;
	struct usb_device *dev = NULL;
	usb_init();
	usb_get_busses();
	usb_find_busses();
	usb_find_devices();

	busses = usb_get_busses();
	
	for (bus = busses; bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next) {
			if ((dev->descriptor.idVendor == vid) && 
			    (dev->descriptor.idProduct == pid)) {
				goto found;
			}
		}
		dev = NULL;
	}
found:
	if (dev == NULL) {
		printf("USB device %04x:%04x not found\n", vid, pid);
		return NULL;
	}
	handle = usb_open(dev);
	if (handle == NULL) {
		puts(usb_strerror());
		return NULL;
	}
	usb_claim_interface(handle, dev->config->interface->altsetting->bInterfaceNumber);
	snprintf(target, sizeof(target), "USB(%04x:%04x)", vid, pid);
	return &usb_port;
}
#else
struct port_t *open_usb(unsigned short vid, unsigned short pid)
{
	return NULL;
}
#endif
