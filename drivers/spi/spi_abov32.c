/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_spi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <hll_spi.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_abov32, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

/*
 * Controller-mode only, polling-based, for the dedicated SPI0/SPI1
 * peripheral (see hll_spi.h) -- as opposed to spi_abov32_usart.c, which runs
 * a USART instance in its SPI mode instead. Unlike that driver's CPOL/CPHA
 * mapping (USART's clock-polarity enum names are ambiguous), CR.CPOL/CR.CPHA
 * here have standard, unambiguous meanings per hll_spi.h's own doc comments
 * ("idle-high"/"sample on second edge"), so the mapping to Zephyr's
 * SPI_MODE_CPOL/SPI_MODE_CPHA bits below is a direct, confident one, not a
 * guess.
 *
 * Native hardware SS (CR.SSMO/CR.SSMOD/CR.SSPOL) is left untouched beyond
 * putting it in manual mode so the peripheral doesn't wait on an
 * unconnected/unmuxed SS pin: CS is handled entirely by spi_context's own
 * cs-gpios support (or omitted, e.g. for a bare master/slave point-to-point
 * test with nothing to select).
 */

struct spi_abov32_config {
	SPI_ID_e id;
	const struct pinctrl_dev_config *pcfg;
};

struct spi_abov32_data {
	struct spi_context ctx;
};

static int spi_abov32_configure(const struct device *dev, const struct spi_config *config)
{
	const struct spi_abov32_config *config_dev = dev->config;
	struct spi_abov32_data *data = dev->data;
	bool is_slave = SPI_OP_MODE_GET(config->operation) == SPI_OP_MODE_SLAVE;
	uint32_t divider;

	if (spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if (SPI_WORD_SIZE_GET(config->operation) != 8) {
		return -ENOTSUP;
	}

	if ((config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		return -ENOTSUP;
	}

	if (config->operation & (SPI_HALF_DUPLEX | SPI_CS_ACTIVE_HIGH)) {
		return -ENOTSUP;
	}

	HLL_SPI_SetMasterMode(config_dev->id, !is_slave);
	HLL_SPI_SetBitOrder(config_dev->id, !(config->operation & SPI_TRANSFER_LSB));
	HLL_SPI_SetClockPolarity(config_dev->id, (config->operation & SPI_MODE_CPOL) != 0);
	HLL_SPI_SetClockPhase(config_dev->id, (config->operation & SPI_MODE_CPHA) != 0);
	HLL_SPI_SetDataBit(config_dev->id, SPI_DATA_8);

	/* No SS pin muxed/connected for this sample -- keep SS under manual
	 * (software) control so the peripheral doesn't wait on it.
	 */
	HLL_SPI_SetSSManual(config_dev->id, true);

	if (!is_slave) {
		/*
		 * hll_spi.h's own note: HAL_SPI never derives BR.BR from a
		 * target bit rate either (just clamps/passes it through), so
		 * this divide -- matching the same convention used for
		 * HLL_I2C_CalcSclPeriod()/the USART-SPI driver -- isn't a
		 * formula ported from vendor code.
		 */
		if (config->frequency == 0) {
			return -EINVAL;
		}
		divider = SystemPeriClock / (2U * config->frequency);
		if (divider > 0U) {
			divider -= 1U;
		}
		HLL_SPI_SetBaudRate(config_dev->id, (uint16_t)divider);
	}

	HLL_SPI_SetEnable(config_dev->id, true);

	data->ctx.config = config;

	return 0;
}

static int spi_abov32_transceive(const struct device *dev, const struct spi_config *config,
				  const struct spi_buf_set *tx_bufs,
				  const struct spi_buf_set *rx_bufs)
{
	const struct spi_abov32_config *config_dev = dev->config;
	struct spi_abov32_data *data = dev->data;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, config);

	ret = spi_abov32_configure(dev, config);
	if (ret != 0) {
		goto done;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);
	spi_context_cs_control(&data->ctx, true);

	/*
	 * k_yield() in the wait loops below is a no-op when nothing else of
	 * equal-or-higher priority is ready (the common case), but it matters
	 * when this controller-mode instance and a peripheral-mode instance
	 * of one of these drivers are being polled from two threads sharing
	 * one CPU (e.g. a same-board master<->slave bring-up test): without
	 * it, a non-yielding busy loop here would starve the other thread of
	 * any CPU time until this whole transfer completes.
	 */
	while (spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx)) {
		uint8_t tx_byte = spi_context_tx_buf_on(&data->ctx) ? *data->ctx.tx_buf : 0U;
		uint8_t rx_byte;

		while (!HLL_SPI_GetTxReady(config_dev->id)) {
			k_yield();
		}
		HLL_SPI_TransmitData(config_dev->id, tx_byte);

		while (!HLL_SPI_GetRxReady(config_dev->id)) {
			k_yield();
		}
		rx_byte = (uint8_t)HLL_SPI_ReceiveData(config_dev->id);

		if (spi_context_rx_buf_on(&data->ctx)) {
			*data->ctx.rx_buf = rx_byte;
		}

		spi_context_update_tx(&data->ctx, 1, 1);
		spi_context_update_rx(&data->ctx, 1, 1);
	}

	spi_context_cs_control(&data->ctx, false);

done:
	spi_context_release(&data->ctx, ret);

	return ret;
}

static int spi_abov32_release(const struct device *dev, const struct spi_config *config)
{
	struct spi_abov32_data *data = dev->data;

	ARG_UNUSED(config);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_abov32_api) = {
	.transceive = spi_abov32_transceive,
	.release = spi_abov32_release,
};

static int spi_abov32_init(const struct device *dev)
{
	const struct spi_abov32_config *config = dev->config;
	struct spi_abov32_data *data = dev->data;
	int ret;

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret != 0) {
		return ret;
	}

	if (HLL_SPI_SetClockEnable(config->id, true) != HAL_ERR_OK) {
		return -EIO;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define ABOV32_SPI_INIT(n)                                                                        \
	PINCTRL_DT_INST_DEFINE(n);                                                                \
                                                                                                   \
	static const struct spi_abov32_config spi_abov32_cfg_##n = {                             \
		.id = (SPI_ID_e)DT_INST_PROP(n, spi_id),                                          \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                        \
	};                                                                                         \
                                                                                                   \
	static struct spi_abov32_data spi_abov32_data_##n = {                                    \
		SPI_CONTEXT_INIT_LOCK(spi_abov32_data_##n, ctx),                                  \
		SPI_CONTEXT_INIT_SYNC(spi_abov32_data_##n, ctx),                                  \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                            \
                                                                                                   \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_abov32_init, NULL, &spi_abov32_data_##n,                \
				  &spi_abov32_cfg_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,     \
				  &spi_abov32_api);

DT_INST_FOREACH_STATUS_OKAY(ABOV32_SPI_INIT)
