/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

/* The devicetree node identifier for the ABOV32 WDT0 peripheral. */
#define WDT_NODE DT_NODELABEL(wdt0)

#define WDT_TIMEOUT_MS 1000
#define WDT_RESET_AFTER 5

static uint32_t underflow_count;

/*
 * Runs in interrupt context: this *is* the WDT's own underflow ISR (see
 * wdt_abov32_isr() in drivers/watchdog/wdt_abov32.c), not deferred work.
 * Each call means WDT_TIMEOUT_MS elapsed without a wdt_feed() -- reload
 * from here to keep the board alive for up to WDT_RESET_AFTER underflows,
 * then let the last one actually reset it.
 *
 * The reset on the 5th underflow is triggered by sys_reboot(), not by the
 * WDT's own hardware reset-on-underflow (WDT_FLAG_RESET_SOC / CR.WDTRE):
 * this IP has no separate delay/window register between "underflow" and
 * "reset asserts", so there's no documented guarantee that reloading from
 * this ISR would win a race against an underflow that's already wired
 * straight to a hardware reset. Driving the reset from the interrupt count
 * in software sidesteps that race entirely and is what actually gives you
 * "reload 4 times, reset on the 5th" deterministically.
 */
static void wdt_callback(const struct device *dev, int channel_id)
{
	underflow_count++;

	printk("WDT0 underflow #%u\n", underflow_count);

	if (underflow_count < WDT_RESET_AFTER) {
		wdt_feed(dev, channel_id);
		return;
	}

	printk("Reached %d underflows -- resetting now\n", WDT_RESET_AFTER);
	sys_reboot(SYS_REBOOT_COLD);
}

int main(void)
{
	const struct device *const wdt = DEVICE_DT_GET(WDT_NODE);
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = WDT_TIMEOUT_MS},
		.callback = wdt_callback,
		.flags = WDT_FLAG_RESET_NONE,
	};
	int channel_id;
	int ret;

	if (!device_is_ready(wdt)) {
		printk("WDT0 device is not ready\n");
		return -ENODEV;
	}

	channel_id = wdt_install_timeout(wdt, &cfg);
	if (channel_id < 0) {
		printk("wdt_install_timeout failed: %d\n", channel_id);
		return channel_id;
	}

	ret = wdt_setup(wdt, 0);
	if (ret != 0) {
		printk("wdt_setup failed: %d\n", ret);
		return ret;
	}

	printk("ABOV32 WDT0 driver sample -- %d ms timeout, resets after %d "
	       "un-fed underflows\n", WDT_TIMEOUT_MS, WDT_RESET_AFTER);
	printk("main() never calls wdt_feed() -- every reload below comes from "
	       "the underflow interrupt itself.\n");

	while (1) {
		k_sleep(K_MSEC(100));
	}

	return 0;
}
