/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* The devicetree node identifier for the ABOV32 FRT0 peripheral. */
#define FRT_NODE DT_NODELABEL(frt0)

#define TOP_PERIOD_MS 1000

/*
 * Runs in interrupt context (this is the FRT's own match ISR -- see
 * counter_abov32_isr() in drivers/counter/counter_abov32.c).
 *
 * If this never fires, or the raw count printed below never wraps back to a
 * small value, that means CTRL.MODE = 1 ("Periodic Timer" per the SVD)
 * doesn't actually reload the counter at MCNT on this chip -- the one
 * assumption in counter_abov32.c that hasn't been confirmed on real
 * hardware yet.
 */
static volatile uint32_t top_count;

static void top_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	top_count++;
}

int main(void)
{
	const struct device *const frt = DEVICE_DT_GET(FRT_NODE);
	struct counter_top_cfg top_cfg = {
		.callback = top_callback,
		.user_data = NULL,
		.flags = 0,
	};
	int ret;

	if (!device_is_ready(frt)) {
		printk("FRT0 device is not ready\n");
		return -ENODEV;
	}

	printk("ABOV32 FRT0 counter driver sample -- %u Hz, %d ms top period\n",
	       counter_get_frequency(frt), TOP_PERIOD_MS);

	top_cfg.ticks = counter_us_to_ticks(frt, TOP_PERIOD_MS * 1000U);

	ret = counter_set_top_value(frt, &top_cfg);
	if (ret != 0) {
		printk("counter_set_top_value failed: %d\n", ret);
		return ret;
	}

	ret = counter_start(frt);
	if (ret != 0) {
		printk("counter_start failed: %d\n", ret);
		return ret;
	}

	while (1) {
		uint32_t value;

		k_sleep(K_MSEC(TOP_PERIOD_MS));

		counter_get_value(frt, &value);
		printk("top #%u -- raw count now %u (top value %u)\n", top_count, value,
		       counter_get_top_value(frt));
	}

	return 0;
}
