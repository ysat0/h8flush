#include <usb.h>
#include <stdio.h>
#include "h8flash.h"

#define USB_TIMEOUT 100000

static struct usb_dev_handle *handle;

static int usb_send_data(const unsigned char *buf, int len)
{
	return usb_bulk_write(handle, 0x01, (const char *)buf, len, USB_TIMEOUT);
}

int usb_read_byte(unsigned char *data)
{
	static unsigned char buf[64];
	static unsigned char *rp;
	static int count = 0;

	if (count == 0) {
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

/* connection target CPU */
static int connect_target(void)
{
	unsigned char req = 0x55;
	int r;
	usb_bulk_write(handle, 0x01, (const char *)&req, 1, USB_TIMEOUT);
	while ((r = usb_bulk_read(handle, 0x82, (char *)&req, 1, USB_TIMEOUT)) == 0)
		usleep(100000);
	if (r < 0 || req != 0xe6)
		return 0;
	else
		return 1;
}

static void port_close(void)
{
	usb_close(handle);
}

static struct port_t usb_port = {
	.type = usb,
	.send_data = usb_send_data,
	.receive_byte = usb_read_byte,
	.connect_target = connect_target,
	.setbaud = NULL,
	.close = port_close,
};

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
			    (dev->descriptor.idProduct == pid)) 
				goto found;
		}
	}
found:
	if (dev == NULL)
		return NULL;
	handle = usb_open(dev);
	if (handle == NULL) {
		puts(usb_strerror());
		return NULL;
	}
	usb_claim_interface(handle, dev->config->interface->altsetting->bInterfaceNumber);
	return &usb_port;
}
