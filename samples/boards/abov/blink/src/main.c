/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* 100 msec between steps */
#define SLEEP_TIME_MS 100

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led4), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led5), gpios),
};

int main(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			return 0;
		}

		if (gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0) {
			return 0;
		}
	}

	i = 0;
	while (1) {
		gpio_pin_set_dt(&leds[i], 1);
		k_msleep(SLEEP_TIME_MS);
		gpio_pin_set_dt(&leds[i], 0);

		i = (i + 1) % ARRAY_SIZE(leds);
	}

	return 0;
}
