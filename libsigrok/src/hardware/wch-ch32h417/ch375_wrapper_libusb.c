/*
 * This file is part of the LogicAnalyzer project.
 *
 * libusb implementation of the CH375 wrapper API.
 *
 * The Windows build talks to WCH's CH375DLL64.DLL, which is a user-mode
 * shim over WCH's kernel driver.  On macOS/Linux we speak USB directly:
 * the device exposes one vendor-specific interface with six bulk endpoints
 * (0x01/0x81 command, 0x02/0x82 logic data, 0x03/0x83 ADC data).
 *
 * The public API of ch375_wrapper.h is kept identical so that libsigrok's
 * ch32h417 driver and PulseView do not need to change.
 */

#include <config.h>
#include "ch375_wrapper.h"
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#include <glib.h>
#include <libusb.h>
#include <string.h>
#include <stdlib.h>

#define LOG_PREFIX "ch375-usb"

#define CH375_VID 0x1a86
#define CH375_PID 0x5537

#define CH375_MAX_DEV 16
#define CH375_NUM_PIPES 4          /* index 1..3 used */
#define CH375_XFERS_PER_PIPE 8    /* in-flight bulk transfers per data pipe */

#define CH375_MIN_CHUNK (64 * 1024)
#define CH375_MAX_CHUNK (4 * 1024 * 1024)

#define CH375_NOTIFY_INTERVAL_US (300 * 1000)

struct ch375_buf {
	uint8_t *data;
	size_t len;
};

struct ch375_dev;

struct ch375_pipe {
	struct ch375_dev *dev;
	unsigned int num;           /* 1, 2, 3 */
	int ep_in;
	int ep_out;

	gboolean enabled;
	gboolean started;
	size_t chunk;

	GMutex lock;
	GCond cond;
	GQueue *queue;              /* struct ch375_buf * */
	size_t queued_bytes;
	unsigned long queued_packets;
	size_t head_off;

	gboolean stop;
	int active;                 /* transfers currently submitted */
	struct libusb_transfer *xfers[CH375_XFERS_PER_PIPE];
};

struct ch375_dev {
	unsigned int index;
	libusb_device_handle *handle;
	struct libusb_device_descriptor desc;
	int refcount;
	gboolean open;

	unsigned long wtimeout;
	unsigned long rtimeout;

	GMutex lock;
	struct ch375_pipe pipes[CH375_NUM_PIPES];

	GThread *event_thread;
	gboolean event_stop;
};

static gboolean ch375_loaded;
static GOnce ch375_once = G_ONCE_INIT;

static GMutex ch375_global_lock;
static struct ch375_dev ch375_devs[CH375_MAX_DEV];
static gboolean ch375_devs_initialized;

/* Hotplug notification */
static GThread *ch375_notify_thread;
static gboolean ch375_notify_stop;
static CH375NotifyCallback ch375_notify_cb;
static gboolean ch375_notify_present;

/* -------------------------------------------------------------------------*/
/* helpers                                                                  */
/* -------------------------------------------------------------------------*/

static int pipe_in_ep(unsigned int pipe)
{
	switch (pipe) {
	case 1: return 0x81;
	case 2: return 0x82;
	case 3: return 0x83;
	default: return -1;
	}
}

static int pipe_out_ep(unsigned int pipe)
{
	switch (pipe) {
	case 1: return 0x01;
	case 2: return 0x02;
	case 3: return 0x03;
	default: return -1;
	}
}

static gpointer ch375_init_once(gpointer data)
{
	int ret;

	(void)data;
	ret = libusb_init(NULL);
	if (ret < 0) {
		sr_err("libusb_init failed: %s", libusb_error_name(ret));
		return GINT_TO_POINTER(-1);
	}
	ch375_loaded = TRUE;
	{
		const struct libusb_version *v = libusb_get_version();
		sr_info("libusb backend initialised (libusb %u.%u.%u)",
			v->major, v->minor, v->micro);
	}
	return GINT_TO_POINTER(0);
}

static gboolean device_matches(const struct libusb_device_descriptor *desc)
{
	return desc->idVendor == CH375_VID && desc->idProduct == CH375_PID;
}

static int count_matching_devices(void)
{
	libusb_device **list;
	ssize_t n, i;
	int count = 0;

	n = libusb_get_device_list(NULL, &list);
	if (n < 0)
		return 0;

	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
		    device_matches(&desc))
			count++;
	}
	libusb_free_device_list(list, 1);
	return count;
}

static void dev_init(struct ch375_dev *dev, unsigned int index)
{
	unsigned int p;

	memset(dev, 0, sizeof(*dev));
	dev->index = index;
	dev->wtimeout = 500;
	dev->rtimeout = 500;
	g_mutex_init(&dev->lock);

	for (p = 0; p < CH375_NUM_PIPES; p++) {
		struct ch375_pipe *pipe = &dev->pipes[p];
		pipe->dev = dev;
		pipe->num = p;
		pipe->ep_in = pipe_in_ep(p);
		pipe->ep_out = pipe_out_ep(p);
		pipe->chunk = 256 * 1024;
		pipe->queue = g_queue_new();
		g_mutex_init(&pipe->lock);
		g_cond_init(&pipe->cond);
	}
}

static void dev_init_all(void)
{
	unsigned int i;

	if (ch375_devs_initialized)
		return;
	for (i = 0; i < CH375_MAX_DEV; i++)
		dev_init(&ch375_devs[i], i);
	ch375_devs_initialized = TRUE;
}

/* -------------------------------------------------------------------------*/
/* data pipe                                                                */
/* -------------------------------------------------------------------------*/

static void pipe_drain_locked(struct ch375_pipe *pipe)
{
	struct ch375_buf *buf;

	while ((buf = g_queue_pop_head(pipe->queue))) {
		g_free(buf->data);
		g_free(buf);
	}
	pipe->queued_bytes = 0;
	pipe->queued_packets = 0;
	pipe->head_off = 0;
}

static void LIBUSB_CALL ch375_xfer_cb(struct libusb_transfer *xfer)
{
	struct ch375_pipe *pipe = xfer->user_data;

	g_mutex_lock(&pipe->lock);
	pipe->active--;

	if (xfer->status == LIBUSB_TRANSFER_COMPLETED &&
	    xfer->actual_length > 0 && !pipe->stop) {
		struct ch375_buf *buf = g_new0(struct ch375_buf, 1);
		buf->data = xfer->buffer;
		buf->len = xfer->actual_length;
		g_queue_push_tail(pipe->queue, buf);
		pipe->queued_bytes += buf->len;
		pipe->queued_packets++;

		/* hand the completed buffer to the queue, get a fresh one */
		{
			uint8_t *fresh = g_malloc(pipe->chunk);
			xfer->buffer = fresh;
			xfer->length = (int)pipe->chunk;
			xfer->actual_length = 0;
			if (libusb_submit_transfer(xfer) == 0)
				pipe->active++;
			else
				g_free(fresh);
		}
	} else if (xfer->status == LIBUSB_TRANSFER_TIMED_OUT && !pipe->stop) {
		if (libusb_submit_transfer(xfer) == 0)
			pipe->active++;
	} else {
		/* cancelled, device gone, or stopping */
		if (xfer->buffer) {
			g_free(xfer->buffer);
			xfer->buffer = NULL;
		}
	}

	g_cond_broadcast(&pipe->cond);
	g_mutex_unlock(&pipe->lock);
}

static gboolean pipe_start(struct ch375_pipe *pipe)
{
	int i;
	int submitted = 0;

	g_mutex_lock(&pipe->lock);
	if (pipe->started) {
		g_mutex_unlock(&pipe->lock);
		return TRUE;
	}
	if (!pipe->enabled) {
		g_mutex_unlock(&pipe->lock);
		return FALSE;
	}

	pipe->stop = FALSE;
	pipe->active = 0;
	pipe->head_off = 0;
	for (i = 0; i < CH375_XFERS_PER_PIPE; i++) {
		uint8_t *buf = g_malloc(pipe->chunk);
		struct libusb_transfer *xfer = libusb_alloc_transfer(0);

		if (!xfer) {
			g_free(buf);
			continue;
		}
		libusb_fill_bulk_transfer(xfer, pipe->dev->handle, pipe->ep_in,
					  buf, (int)pipe->chunk,
					  ch375_xfer_cb, pipe,
					  (unsigned int)pipe->dev->rtimeout);
		pipe->xfers[i] = xfer;
		if (libusb_submit_transfer(xfer) == 0) {
			pipe->active++;
			submitted++;
		} else {
			g_free(buf);
			libusb_free_transfer(xfer);
			pipe->xfers[i] = NULL;
		}
	}
	pipe->started = (submitted > 0);
	g_mutex_unlock(&pipe->lock);

	sr_dbg("pipe %u started (%d x %zu bytes in flight)",
	       pipe->num, submitted, pipe->chunk);
	return pipe->started;
}

static void pipe_stop(struct ch375_pipe *pipe)
{
	int i;

	g_mutex_lock(&pipe->lock);
	if (!pipe->started) {
		pipe_drain_locked(pipe);
		g_mutex_unlock(&pipe->lock);
		return;
	}
	pipe->stop = TRUE;
	g_mutex_unlock(&pipe->lock);

	for (i = 0; i < CH375_XFERS_PER_PIPE; i++) {
		if (pipe->xfers[i])
			libusb_cancel_transfer(pipe->xfers[i]);
	}

	g_mutex_lock(&pipe->lock);
	while (pipe->active > 0)
		g_cond_wait(&pipe->cond, &pipe->lock);
	pipe_drain_locked(pipe);
	pipe->started = FALSE;
	g_mutex_unlock(&pipe->lock);

	for (i = 0; i < CH375_XFERS_PER_PIPE; i++) {
		if (!pipe->xfers[i])
			continue;
		if (pipe->xfers[i]->buffer)
			g_free(pipe->xfers[i]->buffer);
		libusb_free_transfer(pipe->xfers[i]);
		pipe->xfers[i] = NULL;
	}
}

/* -------------------------------------------------------------------------*/
/* device event thread                                                      */
/* -------------------------------------------------------------------------*/

static gpointer ch375_event_thread(gpointer arg)
{
	struct ch375_dev *dev = arg;

	while (TRUE) {
		struct timeval tv;
		int ret;

		g_mutex_lock(&dev->lock);
		if (dev->event_stop) {
			g_mutex_unlock(&dev->lock);
			break;
		}
		g_mutex_unlock(&dev->lock);

		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000;
		ret = libusb_handle_events_timeout_completed(NULL, &tv, NULL);
		if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED)
			g_usleep(1000);
	}

	return NULL;
}

/* -------------------------------------------------------------------------*/
/* init / cleanup                                                           */
/* -------------------------------------------------------------------------*/

int ch375_init(void)
{
	g_once(&ch375_once, ch375_init_once, NULL);
	if (!ch375_loaded)
		return SR_ERR;
	g_mutex_lock(&ch375_global_lock);
	dev_init_all();
	g_mutex_unlock(&ch375_global_lock);
	return SR_OK;
}

int ch375_is_loaded(void)
{
	return ch375_loaded;
}

void ch375_cleanup(void)
{
	unsigned int i;

	if (!ch375_loaded)
		return;

	/* stop hotplug polling */
	ch375_set_device_notify(0, NULL, NULL);

	for (i = 0; i < CH375_MAX_DEV; i++) {
		gboolean open;

		g_mutex_lock(&ch375_global_lock);
		open = ch375_devs_initialized && ch375_devs[i].open;
		g_mutex_unlock(&ch375_global_lock);

		if (open)
			ch375_close_device(i);
	}

	/* deliberately do not call libusb_exit(NULL): PulseView keeps using
	 * the default libusb context for its own hotplug polling. */
}

/* -------------------------------------------------------------------------*/
/* device management                                                        */
/* -------------------------------------------------------------------------*/

static int open_matching_device(unsigned int index,
				libusb_device_handle **out,
				struct libusb_device_descriptor *out_desc)
{
	libusb_device **list;
	ssize_t n, i;
	int seen = 0;
	int ret = LIBUSB_ERROR_NOT_FOUND;

	n = libusb_get_device_list(NULL, &list);
	if (n < 0)
		return (int)n;

	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;
		if (!device_matches(&desc))
			continue;
		if (seen++ != (int)index)
			continue;

		ret = libusb_open(list[i], out);
		if (ret == 0)
			*out_desc = desc;
		break;
	}

	libusb_free_device_list(list, 1);
	return ret;
}

static struct ch375_dev *dev_by_index(unsigned long index)
{
	if (index >= CH375_MAX_DEV)
		return NULL;
	return &ch375_devs[index];
}

void *ch375_open_device(unsigned long index)
{
	struct ch375_dev *dev;
	libusb_device_handle *handle = NULL;
	struct libusb_device_descriptor desc;
	int ret;

	if (!ch375_loaded)
		return CH375_INVALID_HANDLE;

	g_mutex_lock(&ch375_global_lock);
	dev_init_all();
	dev = dev_by_index(index);
	if (!dev) {
		g_mutex_unlock(&ch375_global_lock);
		return CH375_INVALID_HANDLE;
	}

	if (dev->open) {
		dev->refcount++;
		g_mutex_unlock(&ch375_global_lock);
		return dev;
	}

	ret = open_matching_device((unsigned int)index, &handle, &desc);
	if (ret != 0) {
		sr_dbg("open device %lu failed: %s", index, libusb_error_name(ret));
		g_mutex_unlock(&ch375_global_lock);
		return CH375_INVALID_HANDLE;
	}

	/* Select configuration 1.  A device that is already configured may
	 * return BUSY; that is fine, just continue and claim the interface. */
	ret = libusb_set_configuration(handle, 1);
	if (ret != 0 && ret != LIBUSB_ERROR_BUSY)
		sr_dbg("set_configuration(1) returned %s", libusb_error_name(ret));

	ret = libusb_claim_interface(handle, 0);
	if (ret != 0) {
		sr_err("claim interface 0 failed: %s", libusb_error_name(ret));
		libusb_close(handle);
		g_mutex_unlock(&ch375_global_lock);
		return CH375_INVALID_HANDLE;
	}

	dev->handle = handle;
	dev->desc = desc;
	dev->open = TRUE;
	dev->refcount = 1;
	dev->event_stop = FALSE;
	dev->event_thread = g_thread_new("ch375-event", ch375_event_thread, dev);

	g_mutex_unlock(&ch375_global_lock);

	sr_info("opened %04x:%04x on bus %u address %u",
		desc.idVendor, desc.idProduct,
		libusb_get_bus_number(libusb_get_device(handle)),
		libusb_get_device_address(libusb_get_device(handle)));
	return dev;
}

void ch375_close_device(unsigned long index)
{
	struct ch375_dev *dev;
	unsigned int p;

	g_mutex_lock(&ch375_global_lock);
	dev = dev_by_index(index);
	if (!dev || !dev->open || dev->refcount <= 0) {
		g_mutex_unlock(&ch375_global_lock);
		return;
	}
	dev->refcount--;
	if (dev->refcount > 0) {
		g_mutex_unlock(&ch375_global_lock);
		return;
	}

	for (p = 1; p < CH375_NUM_PIPES; p++)
		pipe_stop(&dev->pipes[p]);

	g_mutex_lock(&dev->lock);
	dev->event_stop = TRUE;
	g_mutex_unlock(&dev->lock);
	if (dev->event_thread) {
		g_thread_join(dev->event_thread);
		dev->event_thread = NULL;
	}

	if (dev->handle) {
		libusb_release_interface(dev->handle, 0);
		libusb_close(dev->handle);
		dev->handle = NULL;
	}
	dev->open = FALSE;
	g_mutex_unlock(&ch375_global_lock);

	sr_info("closed device %lu", index);
}

unsigned long ch375_get_usb_id(unsigned long index)
{
	struct ch375_dev *dev;

	if (index >= CH375_MAX_DEV)
		return 0;
	dev = &ch375_devs[index];
	if (!dev->open)
		return 0;
	return ((unsigned long)dev->desc.idProduct << 16) |
	       dev->desc.idVendor;
}

const char *ch375_get_device_name(unsigned long index)
{
	(void)index;
	return "WCH USB3.0 DEVICE";
}

int ch375_get_device_descr(unsigned long index, void *buffer, unsigned long *length)
{
	struct ch375_dev *dev;
	const unsigned long want = 18;

	if (!buffer || !length || index >= CH375_MAX_DEV)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;

	if (*length < want) {
		*length = 0;
		return CH375_FALSE;
	}

	/* libusb_device_descriptor has the same layout as the 18-byte USB
	 * device descriptor (same fields, same order, same sizes). */
	memcpy(buffer, &dev->desc, want);
	*length = want;
	return CH375_TRUE;
}

int ch375_reset_device(unsigned long index)
{
	struct ch375_dev *dev;

	if (index >= CH375_MAX_DEV)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;
	return libusb_reset_device(dev->handle) == 0 ? CH375_TRUE : CH375_FALSE;
}

/* -------------------------------------------------------------------------*/
/* timeouts / io mode                                                       */
/* -------------------------------------------------------------------------*/

int ch375_set_timeout(unsigned long index, unsigned long writeTimeout,
		      unsigned long readTimeout)
{
	struct ch375_dev *dev;

	if (index >= CH375_MAX_DEV)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	dev->wtimeout = writeTimeout ? writeTimeout : 500;
	dev->rtimeout = readTimeout ? readTimeout : 500;
	return CH375_TRUE;
}

int ch375_set_timeout_ex(unsigned long index, unsigned long writeTimeout,
			 unsigned long readTimeout, unsigned long auxTimeout,
			 unsigned long interTimeout)
{
	(void)auxTimeout;
	(void)interTimeout;
	return ch375_set_timeout(index, writeTimeout, readTimeout);
}

int ch375_set_io_mode(unsigned long index, unsigned long sync)
{
	(void)index;
	(void)sync;
	/* The WCH DLL uses this to switch its internal IO mode.  libusb
	 * transfers are always synchronous from the caller's point of view. */
	return CH375_TRUE;
}

/* -------------------------------------------------------------------------*/
/* endpoints                                                                */
/* -------------------------------------------------------------------------*/

int ch375_write_endpoint(unsigned long index, unsigned long pipe_num,
			 void *buffer, unsigned long *length)
{
	struct ch375_dev *dev;
	int ep, ret, transferred = 0;

	if (index >= CH375_MAX_DEV || !buffer || !length)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;

	ep = pipe_out_ep((unsigned int)pipe_num);
	if (ep < 0)
		return CH375_FALSE;

	ret = libusb_bulk_transfer(dev->handle, ep, buffer, (int)*length,
				   &transferred, (unsigned int)dev->wtimeout);
	if (ret != 0) {
		sr_dbg("bulk write pipe %lu failed: %s", pipe_num,
		       libusb_error_name(ret));
		return CH375_FALSE;
	}
	*length = (unsigned long)transferred;
	return CH375_TRUE;
}

static int read_command(struct ch375_dev *dev, void *buffer,
			unsigned long *length)
{
	int transferred = 0;
	int ret;

	ret = libusb_bulk_transfer(dev->handle, 0x81, buffer, (int)*length,
				   &transferred, (unsigned int)dev->rtimeout);
	if (ret != 0) {
		sr_dbg("bulk read EP1 failed: %s", libusb_error_name(ret));
		return CH375_FALSE;
	}
	*length = (unsigned long)transferred;
	return CH375_TRUE;
}

static int read_data(struct ch375_dev *dev, unsigned long pipe_num,
		     void *buffer, unsigned long *length)
{
	struct ch375_pipe *pipe;
	size_t total = 0;
	size_t want;
	gint64 deadline;

	if (pipe_num >= CH375_NUM_PIPES)
		return CH375_FALSE;
	pipe = &dev->pipes[pipe_num];
	if (!pipe->enabled)
		return CH375_FALSE;
	if (!pipe->started && !pipe_start(pipe))
		return CH375_FALSE;

	want = *length;
	deadline = g_get_monotonic_time() +
		   (gint64)dev->rtimeout * 1000;

	g_mutex_lock(&pipe->lock);
	while (total < want) {
		struct ch375_buf *head;
		size_t avail, n;

		if (pipe->stop)
			break;

		head = g_queue_peek_head(pipe->queue);
		if (!head) {
			if (total > 0)
				break;
			if (!g_cond_wait_until(&pipe->cond, &pipe->lock,
					       deadline))
				break;
			continue;
		}

		avail = head->len - pipe->head_off;
		n = MIN(avail, want - total);
		memcpy((uint8_t *)buffer + total,
		       head->data + pipe->head_off, n);
		total += n;
		pipe->head_off += n;
		pipe->queued_bytes -= n;

		if (pipe->head_off >= head->len) {
			g_queue_pop_head(pipe->queue);
			if (pipe->queued_packets)
				pipe->queued_packets--;
			pipe->head_off = 0;
			g_free(head->data);
			g_free(head);
		}
	}
	g_mutex_unlock(&pipe->lock);

	*length = (unsigned long)total;
	return total > 0 ? CH375_TRUE : CH375_FALSE;
}

int ch375_read_endpoint(unsigned long index, unsigned long pipe_num,
			void *buffer, unsigned long *length)
{
	struct ch375_dev *dev;

	if (index >= CH375_MAX_DEV || !buffer || !length)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;

	if (pipe_num == 1)
		return read_command(dev, buffer, length);

	return read_data(dev, pipe_num, buffer, length);
}

int ch375_write_read(unsigned long index, void *iBuffer, void *oBuffer,
		     unsigned long *ioLength)
{
	unsigned long out_len, in_len;

	if (!ioLength)
		return CH375_FALSE;
	out_len = *ioLength;
	if (!ch375_write_endpoint(index, 1, iBuffer, &out_len))
		return CH375_FALSE;
	in_len = *ioLength;
	if (!ch375_read_endpoint(index, 1, oBuffer, &in_len))
		return CH375_FALSE;
	*ioLength = in_len;
	return CH375_TRUE;
}

int ch375_abort_endpoint_read(unsigned long index, unsigned long pipe_num)
{
	struct ch375_dev *dev;

	if (index >= CH375_MAX_DEV || pipe_num >= CH375_NUM_PIPES)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;
	g_mutex_lock(&dev->pipes[pipe_num].lock);
	g_cond_broadcast(&dev->pipes[pipe_num].cond);
	g_mutex_unlock(&dev->pipes[pipe_num].lock);
	return CH375_TRUE;
}

int ch375_abort_endpoint_write(unsigned long index, unsigned long pipe_num)
{
	(void)index;
	(void)pipe_num;
	return CH375_TRUE;
}

int ch375_reset_in_endpoint(unsigned long index, unsigned long pipe_num)
{
	struct ch375_dev *dev;
	int ep = pipe_in_ep((unsigned int)pipe_num);

	if (index >= CH375_MAX_DEV || ep < 0)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;
	return libusb_clear_halt(dev->handle, ep) == 0 ? CH375_TRUE : CH375_FALSE;
}

int ch375_reset_out_endpoint(unsigned long index, unsigned long pipe_num)
{
	struct ch375_dev *dev;
	int ep = pipe_out_ep((unsigned int)pipe_num);

	if (index >= CH375_MAX_DEV || ep < 0)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;
	return libusb_clear_halt(dev->handle, ep) == 0 ? CH375_TRUE : CH375_FALSE;
}

/* -------------------------------------------------------------------------*/
/* buffer upload                                                            */
/* -------------------------------------------------------------------------*/

int ch375_set_buf_upload_ex(unsigned long index, unsigned long enable,
			    unsigned long pipe_num, unsigned long transferSize)
{
	struct ch375_dev *dev;
	struct ch375_pipe *pipe;
	size_t chunk;

	if (index >= CH375_MAX_DEV || pipe_num >= CH375_NUM_PIPES)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	pipe = &dev->pipes[pipe_num];

	if (transferSize) {
		chunk = (size_t)transferSize;
		if (chunk < CH375_MIN_CHUNK)
			chunk = CH375_MIN_CHUNK;
		if (chunk > CH375_MAX_CHUNK)
			chunk = CH375_MAX_CHUNK;
	} else {
		chunk = pipe->chunk;
	}

	g_mutex_lock(&pipe->lock);
	pipe->enabled = enable ? TRUE : FALSE;
	if (chunk)
		pipe->chunk = chunk;
	g_cond_broadcast(&pipe->cond);
	g_mutex_unlock(&pipe->lock);

	if (!enable)
		pipe_stop(pipe);

	return CH375_TRUE;
}

int ch375_query_buf_upload_ex(unsigned long index, unsigned long pipe_num,
			      unsigned long *transferCount,
			      unsigned long *totalDataLen)
{
	struct ch375_dev *dev;
	struct ch375_pipe *pipe;

	if (index >= CH375_MAX_DEV || pipe_num >= CH375_NUM_PIPES)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;
	pipe = &dev->pipes[pipe_num];

	if (!pipe->enabled)
		return CH375_FALSE;
	if (!pipe->started && !pipe_start(pipe))
		return CH375_FALSE;

	g_mutex_lock(&pipe->lock);
	if (transferCount)
		*transferCount = pipe->queued_packets;
	if (totalDataLen)
		*totalDataLen = (unsigned long)pipe->queued_bytes;
	g_mutex_unlock(&pipe->lock);

	return CH375_TRUE;
}

int ch375_clear_buf_upload(unsigned long index, unsigned long pipe_num)
{
	struct ch375_dev *dev;

	if (index >= CH375_MAX_DEV || pipe_num >= CH375_NUM_PIPES)
		return CH375_FALSE;
	dev = &ch375_devs[index];
	if (!dev->open)
		return CH375_FALSE;

	g_mutex_lock(&dev->pipes[pipe_num].lock);
	pipe_drain_locked(&dev->pipes[pipe_num]);
	g_mutex_unlock(&dev->pipes[pipe_num].lock);
	return CH375_TRUE;
}

int ch375_set_buf_download_ex(unsigned long index, unsigned long enable,
			      unsigned long pipe_num, unsigned long transferSize)
{
	(void)index;
	(void)enable;
	(void)pipe_num;
	(void)transferSize;
	return CH375_TRUE;
}

/* -------------------------------------------------------------------------*/
/* raw data helpers (unused by the ch32h417 driver)                         */
/* -------------------------------------------------------------------------*/

int ch375_read_data(unsigned long index, void *buffer, unsigned long *length)
{
	return ch375_read_endpoint(index, 2, buffer, length);
}

int ch375_write_data(unsigned long index, void *buffer, unsigned long *length)
{
	return ch375_write_endpoint(index, 2, buffer, length);
}

int ch375_abort_read(unsigned long index)
{
	return ch375_abort_endpoint_read(index, 2);
}

int ch375_abort_write(unsigned long index)
{
	(void)index;
	return CH375_TRUE;
}

/* -------------------------------------------------------------------------*/
/* hotplug notification                                                     */
/* -------------------------------------------------------------------------*/

static guint ch375_notify_interval_us(void)
{
	const char *ms = g_getenv("CH375_NOTIFY_INTERVAL_MS");
	long value;

	if (!ms)
		return CH375_NOTIFY_INTERVAL_US;
	value = strtol(ms, NULL, 10);
	if (value <= 0)
		return CH375_NOTIFY_INTERVAL_US;
	return (guint)value * 1000;
}

static gpointer ch375_notify_thread_fn(gpointer arg)
{
	guint interval_us = ch375_notify_interval_us();

	(void)arg;

	while (TRUE) {
		gboolean present;

		g_mutex_lock(&ch375_global_lock);
		if (ch375_notify_stop) {
			g_mutex_unlock(&ch375_global_lock);
			break;
		}
		g_mutex_unlock(&ch375_global_lock);

		present = count_matching_devices() > 0;

		if (present != ch375_notify_present) {
			CH375NotifyCallback cb;

			ch375_notify_present = present;

			g_mutex_lock(&ch375_global_lock);
			cb = ch375_notify_cb;
			g_mutex_unlock(&ch375_global_lock);

			if (cb)
				cb(present ? CH375_DEVICE_ARRIVAL
					   : CH375_DEVICE_REMOVE);
		}

		g_usleep(interval_us);
	}

	return NULL;
}

int ch375_set_device_notify(unsigned long index, char *deviceID,
			    CH375NotifyCallback callback)
{
	(void)index;
	(void)deviceID;

	g_mutex_lock(&ch375_global_lock);
	if (!callback) {
		if (ch375_notify_thread) {
			ch375_notify_stop = TRUE;
			g_mutex_unlock(&ch375_global_lock);
			g_thread_join(ch375_notify_thread);
			ch375_notify_thread = NULL;
			g_mutex_lock(&ch375_global_lock);
		}
		ch375_notify_cb = NULL;
		g_mutex_unlock(&ch375_global_lock);
		return CH375_TRUE;
	}

	ch375_notify_cb = callback;
	if (!ch375_notify_thread) {
		ch375_notify_present = count_matching_devices() > 0;
		ch375_notify_stop = FALSE;
		ch375_notify_thread =
			g_thread_new("ch375-hotplug", ch375_notify_thread_fn, NULL);
	}
	g_mutex_unlock(&ch375_global_lock);

	return CH375_TRUE;
}
