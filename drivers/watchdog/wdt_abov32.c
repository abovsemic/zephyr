/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_wdt

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>

#include <hll_wdt.h>

/*
 * This IP counts down from a loaded value at a fixed clock rate; it has no
 * count-match (window) mode on this SoC series (see hal_wdt_v_01_00_02.h's
 * "Unused Macro" section), so only a single, non-windowed timeout is
 * supported and window.min must be 0.
 *
 * The counter runs from the internal 32 kHz LSI oscillator routed through
 * the MCCR mux with no further division (MCCR div = 1, WDT prediv = 1), so
 * 1 ms of timeout is 32 counter ticks.
 */
#define WDT_ABOV32_CLK_HZ 32000U

struct wdt_abov32_config {
	WDT_ID_e id;
};

struct wdt_abov32_data {
	wdt_callback_t callback;
	uint32_t timeout_ticks;
	bool timeout_installed;
	bool running;
};

static int wdt_abov32_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	const struct wdt_abov32_config *config = dev->config;
	struct wdt_abov32_data *data = dev->data;
	uint64_t ticks;

	if (data->running) {
		return -EBUSY;
	}

	if (data->timeout_installed) {
		/* Only one channel/timeout is supported by this IP. */
		return -ENOMEM;
	}

	if (cfg->window.min != 0 || cfg->window.max == 0) {
		return -EINVAL;
	}

	ticks = (uint64_t)cfg->window.max * WDT_ABOV32_CLK_HZ / 1000U;
	if (ticks == 0 || ticks > UINT32_MAX) {
		return -EINVAL;
	}

	data->timeout_ticks = (uint32_t)ticks;
	data->callback = cfg->callback;
	data->timeout_installed = true;

	HLL_WDT_SetWriteEnable(config->id);

	HLL_WDT_SetUnderflowIntrEnable(config->id, cfg->callback != NULL);
	HLL_WDT_SetRstEnable(config->id, (cfg->flags & WDT_FLAG_RESET_MASK) != WDT_FLAG_RESET_NONE);

	HLL_WDT_SetLoadValue(config->id, data->timeout_ticks);
	HLL_WDT_Reload(config->id, data->timeout_ticks);

	HLL_WDT_SetWriteDisable(config->id);

	return 0;
}

static int wdt_abov32_setup(const struct device *dev, uint8_t options)
{
	const struct wdt_abov32_config *config = dev->config;
	struct wdt_abov32_data *data = dev->data;

	if (!data->timeout_installed) {
		return -EINVAL;
	}

	if (options != 0) {
		/* Pause-in-sleep / pause-when-halted-by-debugger not supported. */
		return -ENOTSUP;
	}

	HLL_WDT_SetWriteEnable(config->id);
	HLL_WDT_SetCountEnable(config->id, true);
	HLL_WDT_SetWriteDisable(config->id);

	data->running = true;

	return 0;
}

static int wdt_abov32_disable(const struct device *dev)
{
	const struct wdt_abov32_config *config = dev->config;
	struct wdt_abov32_data *data = dev->data;

	if (!data->running) {
		return -EFAULT;
	}

	HLL_WDT_SetWriteEnable(config->id);
	HLL_WDT_SetCountEnable(config->id, false);
	HLL_WDT_SetRstEnable(config->id, false);
	HLL_WDT_SetUnderflowIntrEnable(config->id, false);
	HLL_WDT_SetWriteDisable(config->id);

	data->running = false;
	data->timeout_installed = false;

	return 0;
}

static int wdt_abov32_feed(const struct device *dev, int channel_id)
{
	const struct wdt_abov32_config *config = dev->config;
	struct wdt_abov32_data *data = dev->data;

	if (channel_id != 0 || !data->timeout_installed) {
		return -EINVAL;
	}

	HLL_WDT_Reload(config->id, data->timeout_ticks);

	return 0;
}

static void wdt_abov32_isr(const struct device *dev)
{
	const struct wdt_abov32_config *config = dev->config;
	struct wdt_abov32_data *data = dev->data;

	if (HLL_WDT_GetUnderflowFlag(config->id)) {
		HLL_WDT_ClearUnderflowFlag(config->id);

		if (data->callback != NULL) {
			data->callback(dev, 0);
		}
	}
}

static DEVICE_API(wdt, wdt_abov32_api) = {
	.setup = wdt_abov32_setup,
	.disable = wdt_abov32_disable,
	.install_timeout = wdt_abov32_install_timeout,
	.feed = wdt_abov32_feed,
};

static int wdt_abov32_init(const struct device *dev)
{
	const struct wdt_abov32_config *config = dev->config;

	HLL_WDT_SetAccessEnable(config->id, true);

	/* MCCR div = 1 (bypass): route the raw 32 kHz LSI straight through. */
	HLL_WDT_SetClkSource(config->id, WDT_CLK_MCCR, WDT_CLK_MCCR_LSI, 1);
	HLL_WDT_SetClkPreDiv(config->id, WDT_CLK_PREDIV_1);

	return 0;
}

#define ABOV32_WDT_INIT(n)                                                                        \
	static void wdt_abov32_irq_config_##n(void)                                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), wdt_abov32_isr,            \
			    DEVICE_DT_INST_GET(n), 0);                                            \
		irq_enable(DT_INST_IRQN(n));                                                      \
	}                                                                                          \
                                                                                                   \
	static const struct wdt_abov32_config wdt_abov32_cfg_##n = {                             \
		.id = (WDT_ID_e)DT_INST_PROP(n, wdt_id),                                          \
	};                                                                                         \
                                                                                                   \
	static struct wdt_abov32_data wdt_abov32_data_##n;                                        \
                                                                                                   \
	static int wdt_abov32_init_##n(const struct device *dev)                                 \
	{                                                                                          \
		wdt_abov32_irq_config_##n();                                                      \
                                                                                                   \
		return wdt_abov32_init(dev);                                                      \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, wdt_abov32_init_##n, NULL, &wdt_abov32_data_##n,                \
			      &wdt_abov32_cfg_##n, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
			      &wdt_abov32_api);

DT_INST_FOREACH_STATUS_OKAY(ABOV32_WDT_INIT)
