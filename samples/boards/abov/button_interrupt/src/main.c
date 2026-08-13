/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <inttypes.h>

#define SLEEP_TIME_MS	1
#define NUM_BUTTONS	2

/*
 * The STK starter-kit wires two buttons (sw0/PF6, sw1/PF7). Both are
 * mandatory.
 */
static const struct gpio_dt_spec buttons[NUM_BUTTONS] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
};
static struct gpio_callback button_cb_data[NUM_BUTTONS];

/*
 * led0/led1 are optional. When present, each mirrors its matching button.
 */
static struct gpio_dt_spec leds[NUM_BUTTONS] = {
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0}),
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0}),
};

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	for (size_t i = 0; i < NUM_BUTTONS; i++) {
		if (cb == &button_cb_data[i]) {
			printk("Button %d pressed at %" PRIu32 "\n", (int)i, k_cycle_get_32());
			return;
		}
	}
}

int main(void)
{
	bool have_leds = false;

	for (size_t i = 0; i < NUM_BUTTONS; i++) {
		int ret;

		if (!gpio_is_ready_dt(&buttons[i])) {
			printk("Error: button%d device %s is not ready\n", (int)i,
			       buttons[i].port->name);
			return 0;
		}

		ret = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
		if (ret != 0) {
			printk("Error %d: failed to configure %s pin %d\n", ret,
			       buttons[i].port->name, buttons[i].pin);
			return 0;
		}

		ret = gpio_pin_interrupt_configure_dt(&buttons[i], GPIO_INT_EDGE_TO_ACTIVE);
		if (ret != 0) {
			printk("Error %d: failed to configure interrupt on %s pin %d\n", ret,
			       buttons[i].port->name, buttons[i].pin);
			return 0;
		}

		gpio_init_callback(&button_cb_data[i], button_pressed, BIT(buttons[i].pin));
		gpio_add_callback(buttons[i].port, &button_cb_data[i]);
		printk("Set up button%d at %s pin %d\n", (int)i, buttons[i].port->name,
		       buttons[i].pin);

		if (leds[i].port && !gpio_is_ready_dt(&leds[i])) {
			printk("LED%d device %s is not ready; ignoring it\n", (int)i,
			       leds[i].port->name);
			leds[i].port = NULL;
		}
		if (leds[i].port) {
			if (gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT) != 0) {
				leds[i].port = NULL;
			} else {
				printk("Set up LED%d at %s pin %d\n", (int)i,
				       leds[i].port->name, leds[i].pin);
				have_leds = true;
			}
		}
	}

	printk("Press the buttons\n");
	if (have_leds) {
		while (1) {
			for (size_t i = 0; i < NUM_BUTTONS; i++) {
				if (leds[i].port) {
					int val = gpio_pin_get_dt(&buttons[i]);

					if (val >= 0) {
						gpio_pin_set_dt(&leds[i], val);
					}
				}
			}
			k_msleep(SLEEP_TIME_MS);
		}
	}
	return 0;
}
