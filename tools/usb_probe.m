/*
 * usb_probe.m - dump USB descriptors of the CH32H417 logic analyzer.
 *
 * Pure IOKit / IOUSBHost, no libusb.  Build:
 *   clang -fobjc-arc -framework Foundation -framework IOKit -framework IOUSBHost \
 *         tools/usb_probe.m -o tools/usb_probe
 *
 * Usage:
 *   ./tools/usb_probe [vid] [pid]      (defaults 0x1a86 0x5537)
 */

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/usb/IOUSBHostFamilyDefinitions.h>
#import <IOUSBHost/IOUSBHost.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static const char *xfer_type(unsigned attr)
{
	switch (attr & 0x3) {
	case 0: return "control";
	case 1: return "isochronous";
	case 2: return "bulk";
	case 3: return "interrupt";
	}
	return "?";
}

static void dump_config(const uint8_t *p, size_t len)
{
	size_t total, off = 0;

	if (len < 9) {
		printf("  config descriptor too short (%zu)\n", len);
		return;
	}
	total = le16(p + 2);
	if (total > len)
		total = len;
	printf("  config: wTotalLength=%u bNumInterfaces=%u bConfigurationValue=%u\n",
	       le16(p + 2), p[4], p[5]);

	while (off + 2 <= total) {
		uint8_t dlen = p[off];
		uint8_t dtype = p[off + 1];

		if (dlen < 2 || off + dlen > total)
			break;

		if (dtype == 0x04 && dlen >= 9) {          /* interface */
			printf("  interface %u alt=%u class=0x%02x sub=0x%02x proto=0x%02x nEP=%u\n",
			       p[off + 2], p[off + 3], p[off + 5], p[off + 6],
			       p[off + 7], p[off + 4]);
		} else if (dtype == 0x05 && dlen >= 7) {   /* endpoint */
			uint16_t mps = le16(p + off + 4);
			uint8_t attr = p[off + 3];
			printf("    ep 0x%02x %-11s maxpacket=%u interval=%u\n",
			       p[off + 2], xfer_type(attr), mps & 0x7ff, p[off + 6]);
		} else if (dtype == 0x30 && dlen >= 6) {   /* SuperSpeed companion */
			printf("      ss-companion maxburst=%u bytes/interval=%u attrs=0x%02x\n",
			       p[off + 2], le16(p + off + 4), p[off + 3]);
		}
		off += dlen;
	}
}

static IOUSBHostInterface *find_interface(io_service_t device_svc)
{
	io_iterator_t iter;
	io_service_t child;
	IOUSBHostInterface *result = nil;

	if (IORegistryEntryGetChildIterator(device_svc, kIOServicePlane, &iter) != KERN_SUCCESS)
		return nil;

	while ((child = IOIteratorNext(iter))) {
		io_name_t cls;
		if (IOObjectGetClass(child, cls) == KERN_SUCCESS &&
		    strcmp(cls, "IOUSBHostInterface") == 0) {
			NSError *err = nil;
			result = [[IOUSBHostInterface alloc] initWithIOService:child
			                                              options:IOUSBHostObjectInitOptionsNone
			                                                queue:nil
			                                                error:&err
			                                      interestHandler:nil];
			if (!result)
				fprintf(stderr, "interface open failed: %s\n",
				        err.localizedDescription.UTF8String ?: "unknown");
			IOObjectRelease(child);
			break;
		}
		IOObjectRelease(child);
	}
	IOObjectRelease(iter);
	return result;
}

static void hexdump(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x%s", p[i], (i + 1) % 16 == 0 ? "\n" : " ");
	printf("\n");
}

/* Send CMD_GET_VERSION (0xAC) on EP 0x01 and read the 0xBC response on 0x81. */
static void probe_protocol(IOUSBHostDevice *dev, io_service_t device_svc)
{
	NSError *err = nil;
	IOUSBHostInterface *intf;
	IOUSBHostPipe *ep_out, *ep_in;
	uint8_t cmd[64] = {0};
	NSUInteger sent = 0, got = 0;

	printf("configuring device (bConfigurationValue=1)...\n");
	if (![dev configureWithValue:1 error:&err])
		fprintf(stderr, "  configure failed (may already be configured): %s\n",
		        err.localizedDescription.UTF8String ?: "unknown");

	usleep(300000);

	intf = find_interface(device_svc);
	if (!intf) {
		fprintf(stderr, "no IOUSBHostInterface child found\n");
		return;
	}

	ep_out = [intf copyPipeWithAddress:0x01 error:&err];
	if (!ep_out)
		fprintf(stderr, "copyPipe 0x01 failed: %s\n",
		        err.localizedDescription.UTF8String ?: "unknown");

	ep_in = [intf copyPipeWithAddress:0x81 error:&err];
	if (!ep_in)
		fprintf(stderr, "copyPipe 0x81 failed: %s\n",
		        err.localizedDescription.UTF8String ?: "unknown");

	if (ep_out && ep_in) {
		cmd[0] = 0xAC;   /* CMD_GET_VERSION */
		cmd[1] = 0x00;

		NSMutableData *out = [NSMutableData dataWithBytes:cmd length:sizeof(cmd)];
		if ([ep_out sendIORequestWithData:out
		                 bytesTransferred:&sent
		                completionTimeout:1000
		                            error:&err]) {
			printf("sent 0xAC, %lu bytes\n", (unsigned long)sent);

			NSMutableData *in = [NSMutableData dataWithLength:32];
			if ([ep_in sendIORequestWithData:in
			                bytesTransferred:&got
			               completionTimeout:2000
			                           error:&err]) {
				printf("got %lu bytes:\n", (unsigned long)got);
				hexdump(in.bytes, got);
				if (got >= 5 && ((const uint8_t *)in.bytes)[0] == 0xBC)
					printf("  => firmware=%u hardware=%u (status 0x%02x)\n",
					       ((const uint8_t *)in.bytes)[3],
					       ((const uint8_t *)in.bytes)[4],
					       ((const uint8_t *)in.bytes)[2]);
			} else {
				fprintf(stderr, "EP 0x81 read failed: %s\n",
				        err.localizedDescription.UTF8String ?: "unknown");
			}
		} else {
			fprintf(stderr, "EP 0x01 write failed: %s\n",
			        err.localizedDescription.UTF8String ?: "unknown");
		}
	}

	[intf destroy];
}


/* ---- raw logic capture (diagnostic, bypasses libsigrok) ------------------ */

static int send_cmd(IOUSBHostPipe *ep, uint8_t cmd, uint8_t len, const uint8_t *data)
{
	uint8_t pkt[64] = {0};
	NSError *err = nil;
	NSUInteger sent = 0;

	pkt[0] = cmd;
	pkt[1] = len;
	if (len && data)
		memcpy(&pkt[2], data, len);

	NSMutableData *d = [NSMutableData dataWithBytes:pkt length:sizeof(pkt)];
	return [ep sendIORequestWithData:d
	                bytesTransferred:&sent
	               completionTimeout:1000
	                           error:&err] ? 1 : 0;
}

static int read_resp(IOUSBHostPipe *ep, uint8_t *out, NSUInteger *got)
{
	NSError *err = nil;
	NSMutableData *d = [NSMutableData dataWithLength:32];

	if (![ep sendIORequestWithData:d
	              bytesTransferred:got
	             completionTimeout:2000
	                         error:&err])
		return 0;
	memcpy(out, d.bytes, MIN(*got, (NSUInteger)32));
	return 1;
}

static void probe_capture(IOUSBHostDevice *dev, io_service_t device_svc)
{
	NSError *err = nil;
	IOUSBHostInterface *intf;
	IOUSBHostPipe *ep_out = nil, *ep_in = nil, *ep_data = nil;
	uint8_t resp[32];
	NSUInteger got = 0;
	uint8_t params[2] = {0x10, 0x00};   /* 16-bit, divider 0 (200 MHz) */
	uint8_t level[2] = {0x00, 0x21};    /* DAC value 33 */

	if (![dev configureWithValue:1 error:&err])
		fprintf(stderr, "configure: %s\n", err.localizedDescription.UTF8String);
	usleep(300000);

	intf = find_interface(device_svc);
	if (!intf) { fprintf(stderr, "no interface\n"); return; }

	ep_out = [intf copyPipeWithAddress:0x01 error:&err];
	ep_in = [intf copyPipeWithAddress:0x81 error:&err];
	ep_data = [intf copyPipeWithAddress:0x82 error:&err];
	if (!ep_out || !ep_in || !ep_data) {
		fprintf(stderr, "pipe open failed: %s\n",
		        err.localizedDescription.UTF8String ?: "unknown");
		[intf destroy];
		return;
	}

	send_cmd(ep_out, 0xA3, 0, NULL);            /* stop */
	usleep(50000);
	send_cmd(ep_out, 0xA0, 2, params);         /* set logic params */
	read_resp(ep_in, resp, &got);
	printf("0xA0 -> 0x%02x len=%u status=0x%02x\n",
	       resp[0], resp[1], resp[2]);
	send_cmd(ep_out, 0xA1, 2, level);          /* threshold */
	read_resp(ep_in, resp, &got);
	printf("0xA1 -> 0x%02x len=%u status=0x%02x\n",
	       resp[0], resp[1], resp[2]);
	send_cmd(ep_out, 0xA4, 0, NULL);           /* get logic params */
	read_resp(ep_in, resp, &got);

	send_cmd(ep_out, 0xA2, 0, NULL);           /* start */
	usleep(20000);

	NSMutableData *buf = [NSMutableData dataWithLength:512 * 1024];
	if ([ep_data sendIORequestWithData:buf
	                  bytesTransferred:&got
	                 completionTimeout:3000
	                             error:&err]) {
		const uint8_t *p = buf.bytes;
		NSUInteger nz = 0;
		for (NSUInteger i = 0; i < got; i++)
			if (p[i]) nz++;
		printf("raw capture: %lu bytes, nonzero=%lu\n",
		       (unsigned long)got, (unsigned long)nz);
		hexdump(p, MIN(got, (NSUInteger)64));
	} else {
		fprintf(stderr, "data read failed: %s\n",
		        err.localizedDescription.UTF8String ?: "unknown");
	}

	send_cmd(ep_out, 0xA3, 0, NULL);           /* stop */
	[intf destroy];
}


/* Measure how the firmware's divider byte affects data production rate. */
static void probe_rates(IOUSBHostDevice *dev, io_service_t device_svc)
{
	static const int divs[] = {0, 2, 4, 8, 12, 16, 20, 22, 24};
	NSError *err = nil;
	IOUSBHostInterface *intf;
	IOUSBHostPipe *ep_out, *ep_in, *ep_data;
	uint8_t resp[32];
	NSUInteger got = 0;
	uint8_t level[2] = {0x00, 0x21};

	if (![dev configureWithValue:1 error:&err])
		fprintf(stderr, "configure: %s\n", err.localizedDescription.UTF8String);
	usleep(300000);

	intf = find_interface(device_svc);
	if (!intf) { fprintf(stderr, "no interface\n"); return; }
	ep_out = [intf copyPipeWithAddress:0x01 error:&err];
	ep_in = [intf copyPipeWithAddress:0x81 error:&err];
	ep_data = [intf copyPipeWithAddress:0x82 error:&err];
	if (!ep_out || !ep_in || !ep_data) { [intf destroy]; return; }

	send_cmd(ep_out, 0xA3, 0, NULL);
	usleep(50000);
	send_cmd(ep_out, 0xA1, 2, level);
	read_resp(ep_in, resp, &got);

	printf("%-5s %-10s %-10s %-10s\n", "div", "bytes", "MB/s", "MS/s(16b)");
	for (size_t i = 0; i < sizeof(divs) / sizeof(divs[0]); i++) {
		uint8_t params[2] = {0x10, (uint8_t)divs[i]};
		NSUInteger total = 0;
		NSDate *t0;
		double elapsed;

		send_cmd(ep_out, 0xA3, 0, NULL);
		usleep(30000);
		send_cmd(ep_out, 0xA0, 2, params);
		read_resp(ep_in, resp, &got);
		send_cmd(ep_out, 0xA2, 0, NULL);

		t0 = [NSDate date];
		while ([(NSDate *)t0 timeIntervalSinceNow] > -0.5) {
			NSMutableData *buf = [NSMutableData dataWithLength:1024 * 1024];
			if (![ep_data sendIORequestWithData:buf
			                  bytesTransferred:&got
			                 completionTimeout:500
			                             error:&err])
				break;
			total += got;
			if (got == 0)
				break;
		}
		elapsed = -[(NSDate *)t0 timeIntervalSinceNow];
		send_cmd(ep_out, 0xA3, 0, NULL);

		printf("%-5d %-10lu %-10.1f %-10.2f\n", divs[i],
		       (unsigned long)total,
		       elapsed > 0 ? total / elapsed / 1e6 : 0.0,
		       elapsed > 0 ? total / elapsed / 2e6 : 0.0);
	}

	[intf destroy];
}


/* Force a USB reset, which terminates and re-enumerates the device. */
static void probe_reset(IOUSBHostDevice *dev)
{
	NSError *err = nil;

	printf("resetting device...\n");
	if ([dev resetWithError:&err])
		printf("reset issued\n");
	else
		fprintf(stderr, "reset failed: %s\n",
		        err.localizedDescription.UTF8String ?: "unknown");
}

int main(int argc, char *argv[])
{
	uint32_t vid = 0x1a86, pid = 0x5537;
	CFMutableDictionaryRef match;
	io_iterator_t iter;
	io_service_t svc;
	int found = 0;
	int do_version = 0;
	int do_capture = 0;
	int do_rates = 0;
	int do_reset = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version"))
			do_version = 1;
		if (!strcmp(argv[i], "--capture"))
			do_capture = 1;
		if (!strcmp(argv[i], "--rates"))
			do_rates = 1;
		if (!strcmp(argv[i], "--reset"))
			do_reset = 1;
	}

	if (argc >= 3 && argv[1][0] != '-') {
		vid = (uint32_t)strtoul(argv[1], NULL, 0);
		pid = (uint32_t)strtoul(argv[2], NULL, 0);
	}

	@autoreleasepool {
		match = IOServiceMatching("IOUSBHostDevice");
		if (!match) {
			fprintf(stderr, "IOServiceMatching failed\n");
			return 1;
		}
		CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
		CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
		CFDictionarySetValue(match, CFSTR("idVendor"), nv);
		CFDictionarySetValue(match, CFSTR("idProduct"), np);
		CFRelease(nv);
		CFRelease(np);

		if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) {
			fprintf(stderr, "IOServiceGetMatchingServices failed\n");
			return 1;
		}

		while ((svc = IOIteratorNext(iter))) {
			NSError *err = nil;
			IOUSBHostDevice *dev =
				[[IOUSBHostDevice alloc] initWithIOService:svc
				                                   options:IOUSBHostObjectInitOptionsNone
				                                     queue:nil
				                                     error:&err
				                           interestHandler:nil];
			if (!dev) {
				fprintf(stderr, "open failed: %s\n",
				        err.localizedDescription.UTF8String ?: "unknown");
				IOObjectRelease(svc);
				continue;
			}

			const IOUSBDeviceDescriptor *dd = dev.deviceDescriptor;
			if (dd) {
				printf("device %04x:%04x bcdUSB=0x%04x class=%02x/%02x/%02x "
				       "bMaxPacketSize0=%u bNumConfigurations=%u\n",
				       dd->idVendor, dd->idProduct, dd->bcdUSB,
				       dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
				       dd->bMaxPacketSize0, dd->bNumConfigurations);
			} else {
				printf("device %04x:%04x (no device descriptor)\n", vid, pid);
			}

			/* GET_DESCRIPTOR(CONFIGURATION, index 0) */
			NSMutableData *buf = [NSMutableData dataWithLength:1024];
			IOUSBDeviceRequest req = {
				.bmRequestType = 0x80,
				.bRequest = 0x06,
				.wValue = 0x0200,
				.wIndex = 0,
				.wLength = 1024,
			};
			NSUInteger got = 0;
			if ([dev sendDeviceRequest:req
			                      data:buf
			          bytesTransferred:&got
			         completionTimeout:2000
			                     error:&err]) {
				printf("  got %lu bytes of configuration descriptor\n",
				       (unsigned long)got);
				dump_config(buf.bytes, got);
			} else {
				fprintf(stderr, "  GET_DESCRIPTOR failed: %s\n",
				        err.localizedDescription.UTF8String ?: "unknown");
			}

			if (do_version)
				probe_protocol(dev, svc);
			if (do_capture)
				probe_capture(dev, svc);
			if (do_rates)
				probe_rates(dev, svc);
			if (do_reset)
				probe_reset(dev);

			[dev destroy];
			IOObjectRelease(svc);
			found++;
		}
		IOObjectRelease(iter);
	}

	if (!found)
		fprintf(stderr, "no %04x:%04x IOUSBHostDevice found\n", vid, pid);

	return found ? 0 : 2;
}
