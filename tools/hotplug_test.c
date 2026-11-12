/* hotplug_test.c - watch ch375 hotplug callbacks (wrapper-level test). */
#include <glib.h>
#include <stdio.h>
#include "ch375_wrapper.h"

static void cb(unsigned long ev)
{
	printf("EVENT %lu (%s) t=%.3f\n", ev,
	       ev == CH375_DEVICE_ARRIVAL ? "arrival" : "remove",
	       g_get_monotonic_time() / 1e6);
	fflush(stdout);
}

int main(void)
{
	ch375_init();
	printf("registering callback\n");
	fflush(stdout);
	ch375_set_device_notify(0, NULL, cb);
	g_usleep(15 * G_USEC_PER_SEC);
	ch375_set_device_notify(0, NULL, NULL);
	ch375_cleanup();
	printf("done\n");
	return 0;
}
