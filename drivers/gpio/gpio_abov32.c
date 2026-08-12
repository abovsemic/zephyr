/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_gpio

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>

/* Platform HAL headers */
#include <hal_pcu.h>

struct gpio_abov32_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	PCU_ID_e port_id;
};

struct gpio_abov32_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
};

static int gpio_abov32_pin_configure(const struct device *dev, gpio_pin_t pin,
				      gpio_flags_t flags)
{
	const struct gpio_abov32_config *config = dev->config;
	PCU_ID_e port = config->port_id;
	PCU_PUPD_e pupd;

	if ((flags & GPIO_OUTPUT) != 0U) {
		if (HAL_PCU_SetInOutMode(port, (PCU_PIN_ID_e)pin,
					  (flags & GPIO_OPEN_DRAIN) != 0U
						  ? PCU_INOUT_OUTPUT_OPEN_DRAIN
						  : PCU_INOUT_OUTPUT_PUSH_PULL) != HAL_ERR_OK) {
			return -EINVAL;
		}

		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			HAL_PCU_SetOutputValue(port, (PCU_PIN_ID_e)pin, PCU_PORT_HIGH);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			HAL_PCU_SetOutputValue(port, (PCU_PIN_ID_e)pin, PCU_PORT_LOW);
		}
	} else if ((flags & GPIO_INPUT) != 0U) {
		if (HAL_PCU_SetInOutMode(port, (PCU_PIN_ID_e)pin, PCU_INOUT_INPUT) !=
		    HAL_ERR_OK) {
			return -EINVAL;
		}
	} else {
		return -ENOTSUP;
	}

	if ((flags & GPIO_PULL_UP) != 0U) {
		pupd = PCU_PUPD_UP;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		pupd = PCU_PUPD_DOWN;
	} else {
		pupd = PCU_PUPD_DISABLED;
	}

	if (HAL_PCU_SetPullUpDown(port, (PCU_PIN_ID_e)pin, pupd) != HAL_ERR_OK) {
		return -EINVAL;
	}

	return 0;
}

static int gpio_abov32_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_abov32_config *config = dev->config;
	gpio_port_value_t result = 0;

	for (uint32_t pin = 0U; pin < PCU_PIN_ID_MAX; pin++) {
		PCU_PORT_e level;

		if (HAL_PCU_GetInputValue(config->port_id, (PCU_PIN_ID_e)pin, &level) ==
			    HAL_ERR_OK &&
		    level == PCU_PORT_HIGH) {
			result |= (gpio_port_value_t)BIT(pin);
		}
	}

	*value = result;

	return 0;
}

static int gpio_abov32_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					    gpio_port_value_t value)
{
	const struct gpio_abov32_config *config = dev->config;

	for (uint32_t pin = 0U; pin < PCU_PIN_ID_MAX; pin++) {
		if ((mask & BIT(pin)) == 0U) {
			continue;
		}

		HAL_PCU_SetOutputValue(config->port_id, (PCU_PIN_ID_e)pin,
					(value & BIT(pin)) != 0U ? PCU_PORT_HIGH : PCU_PORT_LOW);
	}

	return 0;
}

static int gpio_abov32_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return gpio_abov32_port_set_masked_raw(dev, pins, pins);
}

static int gpio_abov32_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return gpio_abov32_port_set_masked_raw(dev, pins, 0U);
}

static int gpio_abov32_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_abov32_config *config = dev->config;

	for (uint32_t pin = 0U; pin < PCU_PIN_ID_MAX; pin++) {
		PCU_PORT_e level;

		if ((pins & BIT(pin)) == 0U) {
			continue;
		}

		if (HAL_PCU_GetInputValue(config->port_id, (PCU_PIN_ID_e)pin, &level) !=
		    HAL_ERR_OK) {
			continue;
		}

		HAL_PCU_SetOutputValue(config->port_id, (PCU_PIN_ID_e)pin,
					level == PCU_PORT_HIGH ? PCU_PORT_LOW : PCU_PORT_HIGH);
	}

	return 0;
}

static DEVICE_API(gpio, gpio_abov32_driver_api) = {
	.pin_configure = gpio_abov32_pin_configure,
	.port_get_raw = gpio_abov32_port_get_raw,
	.port_set_masked_raw = gpio_abov32_port_set_masked_raw,
	.port_set_bits_raw = gpio_abov32_port_set_bits_raw,
	.port_clear_bits_raw = gpio_abov32_port_clear_bits_raw,
	.port_toggle_bits = gpio_abov32_port_toggle_bits,
};

#define GPIO_ABOV32_INIT(inst)                                                                   \
	static const struct gpio_abov32_config gpio_abov32_config_##inst = {                     \
		.common = {                                                                       \
			.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(inst),                   \
		},                                                                                 \
		.port_id = (PCU_ID_e)DT_INST_PROP(inst, port_id),                                 \
	};                                                                                         \
	static struct gpio_abov32_data gpio_abov32_data_##inst;                                   \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &gpio_abov32_data_##inst,                         \
			      &gpio_abov32_config_##inst, POST_KERNEL,                            \
			      CONFIG_GPIO_INIT_PRIORITY, &gpio_abov32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_ABOV32_INIT)
