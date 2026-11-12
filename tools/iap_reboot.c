/* Send the IAP "end" command to make the bootloader jump back to the app. */
#include <libusb.h>
#include <hidapi.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IAP_VID 0x1a86
#define IAP_PID 0xfe17

int main(void)
{
	libusb_context *ctx = NULL;
	libusb_device **list;
	ssize_t n, i;

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
			printf("set_configuration -> %d (%s)\n", r, libusb_error_name(r));
			libusb_close(h);
		}
	}
	libusb_free_device_list(list, 1);
	usleep(500000);

	hid_init();
	hid_device *d = hid_open(IAP_VID, IAP_PID, NULL);
	if (!d) {
		printf("hid_open failed: %ls\n", hid_error(NULL));
		return 1;
	}

	uint8_t buf[65] = {0};
	buf[0] = 0;      /* report ID */
	buf[1] = 0x83;   /* CMD_IAP_END */
	buf[2] = 0;      /* len */

	int w = hid_write(d, buf, sizeof(buf));
	printf("hid_write(0x83) -> %d\n", w);
	hid_close(d);
	hid_exit();
	libusb_exit(ctx);
	return 0;
}
