/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_frt

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>

#include <hll_frt.h>

/*
 * This IP has a single 32-bit up-counter (CNT) and a single match register
 * (MCNT). Per the SVD, CTRL.MODE selects "Free-Run Timer" (wraps at
 * 0xFFFFFFFF, OVFIF) or "Periodic Timer" (wraps/reloads at MCNT, MATCHIF) --
 * "Periodic Timer" is exactly Counter API's top-value semantics, so this
 * driver always runs in that mode and maps MCNT directly onto
 * counter_set_top_value()/counter_get_top_value(). MCNT resets to
 * 0xFFFFFFFF, matching the driver's default/max top value, so this behaves
 * like free-run until counter_set_top_value() sets something smaller.
 *
 * There is no second, independent compare register, so channel alarms
 * (counter_set_channel_alarm()) are not supported -- channels = 0.
 *
 * The counter runs from the internal 32 kHz LSI oscillator routed through
 * the MCCR mux with no further division (MCCR div = 1); see hll_frt.h's
 * file-level note that SET_FRT_CR_CLK_SEL()/SET_FRT_CR_CLK_PREDIV() are
 * both no-ops on this IP version, so the MCCR mux is the only real clock
 * config surface.
 */
#define COUNTER_ABOV32_CLK_HZ 32000U

struct counter_abov32_config {
	/* Must be first: cast to struct counter_config_info by common inline
	 * helpers such as counter_is_counting_up().
	 */
	struct counter_config_info info;
	FRT_ID_e id;
};

struct counter_abov32_data {
	counter_top_callback_t top_callback;
	void *top_user_data;
};

static int counter_abov32_start(const struct device *dev)
{
	const struct counter_abov32_config *config = dev->config;

	HLL_FRT_SetEnable(config->id, true);

	return 0;
}

static int counter_abov32_stop(const struct device *dev)
{
	const struct counter_abov32_config *config = dev->config;

	HLL_FRT_SetEnable(config->id, false);

	return 0;
}

static int counter_abov32_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct counter_abov32_config *config = dev->config;

	*ticks = HLL_FRT_GetCount(config->id);

	return 0;
}

static int counter_abov32_set_top_value(const struct device *dev,
					 const struct counter_top_cfg *cfg)
{
	const struct counter_abov32_config *config = dev->config;
	struct counter_abov32_data *data = dev->data;

	data->top_callback = cfg->callback;
	data->top_user_data = cfg->user_data;

	HLL_FRT_SetMatchValue(config->id, cfg->ticks);
	HLL_FRT_SetMatchIntrEnable(config->id, cfg->callback != NULL);

	if (!(cfg->flags & COUNTER_TOP_CFG_DONT_RESET)) {
		/*
		 * HLL_FRT_SetCount() requires the counter to already be
		 * running to take effect (see its doc comment) -- if it's
		 * currently stopped this is a silent no-op, matching this
		 * IP's own behavior rather than something this driver can
		 * work around.
		 */
		HLL_FRT_SetCount(config->id, 0);
	}

	return 0;
}

static uint32_t counter_abov32_get_top_value(const struct device *dev)
{
	const struct counter_abov32_config *config = dev->config;

	return HLL_FRT_GetMatchValue(config->id);
}

static int counter_abov32_set_alarm(const struct device *dev, uint8_t chan_id,
				     const struct counter_alarm_cfg *alarm_cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);
	ARG_UNUSED(alarm_cfg);

	/* See the file-level note: MCNT is already fully committed to
	 * backing the top value, so there's no channel alarm support.
	 */
	return -ENOTSUP;
}

static int counter_abov32_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);

	return -ENOTSUP;
}

static uint32_t counter_abov32_get_pending_int(const struct device *dev)
{
	const struct counter_abov32_config *config = dev->config;

	return HLL_FRT_GetMatchFlag(config->id) ? 1 : 0;
}

static void counter_abov32_isr(const struct device *dev)
{
	const struct counter_abov32_config *config = dev->config;
	struct counter_abov32_data *data = dev->data;

	if (HLL_FRT_GetMatchFlag(config->id)) {
		HLL_FRT_ClearMatchFlag(config->id);

		if (data->top_callback != NULL) {
			data->top_callback(dev, data->top_user_data);
		}
	}
}

static DEVICE_API(counter, counter_abov32_api) = {
	.start = counter_abov32_start,
	.stop = counter_abov32_stop,
	.get_value = counter_abov32_get_value,
	.set_alarm = counter_abov32_set_alarm,
	.cancel_alarm = counter_abov32_cancel_alarm,
	.set_top_value = counter_abov32_set_top_value,
	.get_pending_int = counter_abov32_get_pending_int,
	.get_top_value = counter_abov32_get_top_value,
};

static int counter_abov32_init(const struct device *dev)
{
	const struct counter_abov32_config *config = dev->config;

	HLL_FRT_SetClockEnable(config->id, true);

	/* MCCR div = 1 (bypass): route the raw 32 kHz LSI straight through. */
	HLL_FRT_SetClkSource(config->id, FRT_CLK_MCCR, FRT_CLK_MCCR_LSI, 1);

	/* Always run in Periodic mode -- see the file-level note. */
	HLL_FRT_SetMode(config->id, FRT_MODE_MATCH);

	return 0;
}

#define ABOV32_COUNTER_INIT(n)                                                                   \
	static void counter_abov32_irq_config_##n(void)                                          \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), counter_abov32_isr,       \
			    DEVICE_DT_INST_GET(n), 0);                                            \
		irq_enable(DT_INST_IRQN(n));                                                      \
	}                                                                                          \
                                                                                                   \
	static const struct counter_abov32_config counter_abov32_cfg_##n = {                     \
		.info =                                                                           \
			{                                                                          \
				.max_top_value = UINT32_MAX,                                      \
				.freq = COUNTER_ABOV32_CLK_HZ,                                    \
				.flags = COUNTER_CONFIG_INFO_COUNT_UP,                            \
				.channels = 0,                                                    \
			},                                                                         \
		.id = (FRT_ID_e)DT_INST_PROP(n, frt_id),                                          \
	};                                                                                         \
                                                                                                   \
	static struct counter_abov32_data counter_abov32_data_##n;                               \
                                                                                                   \
	static int counter_abov32_init_##n(const struct device *dev)                             \
	{                                                                                          \
		counter_abov32_irq_config_##n();                                                  \
                                                                                                   \
		return counter_abov32_init(dev);                                                  \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, counter_abov32_init_##n, NULL, &counter_abov32_data_##n,        \
			      &counter_abov32_cfg_##n, POST_KERNEL, CONFIG_COUNTER_INIT_PRIORITY, \
			      &counter_abov32_api);

DT_INST_FOREACH_STATUS_OKAY(ABOV32_COUNTER_INIT)
