/* Verify that configuring the CH32H417 IAP device makes it visible to hidapi. */
#include <libusb.h>
#include <hidapi.h>
#include <stdio.h>
#include <unistd.h>
#include <wchar.h>

#define IAP_VID 0x1a86
#define IAP_PID 0xfe17

int main(void)
{
	libusb_context *ctx = NULL;
	libusb_device **list;
	ssize_t n, i;
	int configured = 0;

	libusb_init(&ctx);
	n = libusb_get_device_list(ctx, &list);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;
		if (desc.idVendor != IAP_VID || desc.idProduct != IAP_PID)
			continue;
		libusb_device_handle *h = NULL;
		if (libusb_open(list[i], &h) == 0) {
			int r = libusb_set_configuration(h, 1);
			printf("set_configuration(1) -> %d (%s)\n", r,
			       r == 0 ? "ok" : libusb_error_name(r));
			configured = 1;
			libusb_close(h);
		}
	}
	libusb_free_device_list(list, 1);
	printf("configured=%d\n", configured);

	usleep(500000);

	hid_init();
	hid_device *d = hid_open(IAP_VID, IAP_PID, NULL);
	printf("hid_open -> %p\n", (void *)d);
	if (d) {
		wchar_t buf[128] = {0};
		hid_get_product_string(d, buf, 128);
		printf("product: %ls\n", buf);
		hid_close(d);
	} else {
		printf("hid_error: %ls\n", hid_error(NULL));
	}
	hid_exit();
	libusb_exit(ctx);
	return 0;
}
