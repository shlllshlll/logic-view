/*
 * capture_test.c - minimal libsigrok capture harness for the CH32H417.
 *
 * Usage:
 *   ./tools/capture_test [samplerate] [samples]
 *
 * Build:
 *   clang tools/capture_test.c -o tools/capture_test \
 *     -Iinstall/include -Linstall/lib -lsigrok \
 *     -Wl,-rpath,"$PWD/install/lib" \
 *     $(pkg-config --cflags --libs glib-2.0 gobject-2.0)
 */

#include <libsigrok/libsigrok.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t want_samples;
static uint64_t got_samples;
static uint64_t got_bytes;
static uint64_t transitions;
static uint64_t nonzero;
static uint16_t first_samples[8];
static int first_count;
static int adc_mode;
static uint64_t adc_count;
static double adc_min = 1e9, adc_max = -1e9;
static float adc_first[4];
static int adc_first_count;
static uint16_t prev_sample;
static gboolean have_prev;
static gboolean got_end;

static void datafeed_cb(const struct sr_dev_inst *sdi,
			const struct sr_datafeed_packet *packet,
			void *cb_data)
{
	(void)sdi;
	(void)cb_data;

	switch (packet->type) {
	case SR_DF_LOGIC: {
		const struct sr_datafeed_logic *logic = packet->payload;
		uint64_t n, i;
		const uint8_t *p = logic->data;

		got_bytes += logic->length;
		n = logic->length / logic->unitsize;
		got_samples += n;

		for (i = 0; i < n; i++) {
			uint16_t v;
			if (logic->unitsize == 2)
				v = ((const uint16_t *)p)[i];
			else
				v = p[i];
			if (have_prev && ((v & 1) != (prev_sample & 1)))
				transitions++;
			if (v)
				nonzero++;
			if (first_count < 8)
				first_samples[first_count++] = v;
			prev_sample = v;
			have_prev = TRUE;
		}
		break;
	}
	case SR_DF_ANALOG: {
		const struct sr_datafeed_analog *a = packet->payload;
		const float *f = a->data;
		uint64_t i;

		adc_count += a->num_samples;
		for (i = 0; i < a->num_samples; i++) {
			double v = f[i];
			if (v < adc_min) adc_min = v;
			if (v > adc_max) adc_max = v;
			if (adc_first_count < 4)
				adc_first[adc_first_count++] = (float)v;
		}
		break;
	}
	case SR_DF_END:
		got_end = TRUE;
		break;
	default:
		break;
	}
}

static gboolean timeout_cb(gpointer data)
{
	struct sr_session *session = data;
	fprintf(stderr, "timeout, stopping\n");
	sr_session_stop(session);
	return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
	struct sr_context *ctx = NULL;
	struct sr_session *session = NULL;
	struct sr_dev_driver **drivers;
	struct sr_dev_driver *drv = NULL;
	GSList *devices;
	struct sr_dev_inst *sdi;
	uint64_t rate = 1000000;
	int ret, i;

	if (argc > 1)
		rate = strtoull(argv[1], NULL, 0);
	want_samples = (argc > 2) ? strtoull(argv[2], NULL, 0) : 1000000;

	sr_log_loglevel_set((argc > 3) ? atoi(argv[3]) : SR_LOG_INFO);

	if ((ret = sr_init(&ctx)) != SR_OK) {
		fprintf(stderr, "sr_init failed: %d\n", ret);
		return 1;
	}

	drivers = sr_driver_list(ctx);
	for (i = 0; drivers[i]; i++) {
		if (!strcmp(drivers[i]->name, "wch-ch32h417")) {
			drv = drivers[i];
			break;
		}
	}
	if (!drv) {
		fprintf(stderr, "wch-ch32h417 driver not found\n");
		return 1;
	}
	if ((ret = sr_driver_init(ctx, drv)) != SR_OK) {
		fprintf(stderr, "driver init failed: %d\n", ret);
		return 1;
	}

	devices = sr_driver_scan(drv, NULL);
	if (!devices) {
		fprintf(stderr, "no device found\n");
		return 1;
	}
	sdi = devices->data;
	g_slist_free(devices);

	if ((ret = sr_dev_open(sdi)) != SR_OK) {
		fprintf(stderr, "dev_open failed: %d\n", ret);
		return 1;
	}

	sr_config_set(sdi, NULL, SR_CONF_SAMPLERATE,
		      g_variant_new_uint64(rate));
	sr_config_set(sdi, NULL, SR_CONF_LIMIT_SAMPLES,
		      g_variant_new_uint64(want_samples));
	/* Same default as the UI: 2.3 V threshold (DAC value 33). */
	{
		const char *thr = getenv("CAPTURE_THRESHOLD");
		uint64_t threshold = thr ? strtoull(thr, NULL, 0) : 33;
		sr_config_set(sdi, NULL, SR_CONF_THRESHOLD_VALUE_1,
			      g_variant_new_uint64(threshold));
	}

	if (getenv("CAPTURE_ADC")) {
		adc_mode = 1;
		sr_config_set(sdi, NULL, SR_CONF_PATTERN_MODE,
			      g_variant_new_string("ADC"));
		sr_config_set(sdi, NULL, SR_CONF_ADC_CHANNEL,
			      g_variant_new_uint64(0));
		sr_config_set(sdi, NULL, SR_CONF_ADC_PRECISION,
			      g_variant_new_uint64(10));
	}

	if ((ret = sr_session_new(ctx, &session)) != SR_OK) {
		fprintf(stderr, "session_new failed: %d\n", ret);
		return 1;
	}
	sr_session_datafeed_callback_add(session, datafeed_cb, NULL);
	sr_session_dev_add(session, sdi);

	fprintf(stderr, "capturing %llu samples @ %llu Hz ...\n",
		(unsigned long long)want_samples, (unsigned long long)rate);

	if ((ret = sr_session_start(session)) != SR_OK) {
		fprintf(stderr, "session_start failed: %d\n", ret);
		return 1;
	}

	g_timeout_add_seconds(10, timeout_cb, session);
	sr_session_run(session);

	printf("samples=%llu bytes=%llu nonzero=%llu transitions(ch0)=%llu end=%d\n",
	       (unsigned long long)got_samples,
	       (unsigned long long)got_bytes,
	       (unsigned long long)nonzero,
	       (unsigned long long)transitions,
	       got_end);
	if (adc_mode) {
		printf("adc samples=%llu min=%.3f max=%.3f first:",
		       (unsigned long long)adc_count, adc_min, adc_max);
		for (i = 0; i < adc_first_count; i++)
			printf(" %.3f", adc_first[i]);
		printf("\n");
	}
	printf("first samples:");
	for (i = 0; i < first_count; i++)
		printf(" %04x", first_samples[i]);
	printf("\n");

	sr_session_destroy(session);
	sr_dev_close(sdi);
	sr_exit(ctx);

	return 0;
}
