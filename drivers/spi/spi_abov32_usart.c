/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_usart_spi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <hll_usart.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_abov32_usart, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

/*
 * Polling-based, supports both controller and (experimental,
 * CONFIG_SPI_SLAVE) peripheral mode -- this reuses one of the otherwise-idle
 * USART instances (USART10 is this board's console; USART11/USART12 are
 * free) running in its SPI mode instead of the dedicated SPI0/SPI1
 * peripheral (see spi_abov32.c / hll_spi.h). See abov,abov32-usart-spi.yaml:
 * a USART instance can be wired up as either that compatible or the async
 * abov,abov32-usart one, never both at once.
 *
 * CPOL/CPHA mapping (USART_CLKPOL_e/USART_CLKPHA_e) to Zephyr's
 * SPI_MODE_CPOL/SPI_MODE_CPHA bits is a best-effort, order-preserving
 * mapping (bit clear -> the enum's first/0 value, bit set -> its second/1
 * value) -- hal_usart.c/hll_usart.h don't document which physical clock
 * edge each enum value corresponds to beyond their own naming
 * (USART_CLKPOL_TXD_RISE_RXD_FALL / _TXD_FALL_RXD_RISE), so this hasn't
 * been cross-checked against a scope. Flag any CPOL/CPHA mismatch seen on
 * real hardware and this mapping can be flipped.
 *
 * Native hardware chip-select generation (HLL_USART_SetSpiSSEnable()) is a
 * no-op on this chip (SET_USART_CR_SSEN is stubbed in the active version
 * header), and USART_FEATURE_MASTER_SS_PIN_BY_GPIO isn't defined either, so
 * this driver never calls it -- CS is handled entirely by spi_context's own
 * cs-gpios support (or omitted, e.g. for a loopback test with nothing to
 * select).
 */

struct spi_abov32_usart_config {
	USART_ID_e id;
	const struct pinctrl_dev_config *pcfg;
};

struct spi_abov32_usart_data {
	struct spi_context ctx;
};

static int spi_abov32_usart_configure(const struct device *dev, const struct spi_config *config)
{
	const struct spi_abov32_usart_config *config_dev = dev->config;
	struct spi_abov32_usart_data *data = dev->data;
	bool is_slave = SPI_OP_MODE_GET(config->operation) == SPI_OP_MODE_SLAVE;
	USART_BIT_ORDER_e bit_order;
	USART_CLKPOL_e clk_pol;
	USART_CLKPHA_e clk_pha;
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

	bit_order = (config->operation & SPI_TRANSFER_LSB) ? USART_BIT_ORDER_LSB
							    : USART_BIT_ORDER_MSB;
	clk_pol = (config->operation & SPI_MODE_CPOL) ? USART_CLKPOL_TXD_FALL_RXD_RISE
						       : USART_CLKPOL_TXD_RISE_RXD_FALL;
	clk_pha = (config->operation & SPI_MODE_CPHA) ? USART_CLKPHA_SETUP : USART_CLKPHA_SAMPLE;

	HLL_USART_ClearControl(config_dev->id);
	HLL_USART_SetMode(config_dev->id, USART_MODE_SPI);
	HLL_USART_SetSpiFormat(config_dev->id, is_slave ? USART_MS_SLAVE : USART_MS_MASTER, bit_order,
				clk_pol, clk_pha, false);

	if (!is_slave) {
		/*
		 * HAL_USART_SetConfig() never derives this divider from a
		 * target frequency either -- it just writes
		 * SPI_CFG_t::un16BaudRate straight through -- so this is a
		 * plain "PCLK / (2 * target Hz)" divide, not a formula ported
		 * from vendor code. A slave doesn't drive its own clock, so
		 * this is skipped entirely in that role.
		 */
		if (config->frequency == 0) {
			return -EINVAL;
		}
		divider = SystemPeriClock / (2U * config->frequency);
		if (divider > 0U) {
			divider -= 1U;
		}
		HLL_USART_SetSpiBaudRaw(config_dev->id, (uint16_t)divider);
	}

	HLL_USART_SetTxEnable(config_dev->id, true);
	HLL_USART_SetRxEnable(config_dev->id, true);
	HLL_USART_SetEnable(config_dev->id, true);

	data->ctx.config = config;

	return 0;
}

static int spi_abov32_usart_transceive(const struct device *dev, const struct spi_config *config,
					const struct spi_buf_set *tx_bufs,
					const struct spi_buf_set *rx_bufs)
{
	const struct spi_abov32_usart_config *config_dev = dev->config;
	struct spi_abov32_usart_data *data = dev->data;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, config);

	ret = spi_abov32_usart_configure(dev, config);
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

		while (!HLL_USART_GetTxReady(config_dev->id)) {
			k_yield();
		}
		HLL_USART_TransmitByte(config_dev->id, tx_byte);

		while (!HLL_USART_GetRxReady(config_dev->id)) {
			k_yield();
		}
		rx_byte = (uint8_t)HLL_USART_ReceiveByte(config_dev->id);

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

static int spi_abov32_usart_release(const struct device *dev, const struct spi_config *config)
{
	struct spi_abov32_usart_data *data = dev->data;

	ARG_UNUSED(config);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_abov32_usart_api) = {
	.transceive = spi_abov32_usart_transceive,
	.release = spi_abov32_usart_release,
};

static int spi_abov32_usart_init(const struct device *dev)
{
	const struct spi_abov32_usart_config *config = dev->config;
	struct spi_abov32_usart_data *data = dev->data;
	int ret;

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret != 0) {
		return ret;
	}

	if (HLL_USART_SetClockEnable(config->id, true) != HAL_ERR_OK) {
		return -EIO;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define ABOV32_USART_SPI_INIT(n)                                                                 \
	PINCTRL_DT_INST_DEFINE(n);                                                                \
                                                                                                   \
	static const struct spi_abov32_usart_config spi_abov32_usart_cfg_##n = {                 \
		.id = (USART_ID_e)DT_INST_PROP(n, usart_id),                                      \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                        \
	};                                                                                         \
                                                                                                   \
	static struct spi_abov32_usart_data spi_abov32_usart_data_##n = {                        \
		SPI_CONTEXT_INIT_LOCK(spi_abov32_usart_data_##n, ctx),                            \
		SPI_CONTEXT_INIT_SYNC(spi_abov32_usart_data_##n, ctx),                            \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                            \
                                                                                                   \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_abov32_usart_init, NULL, &spi_abov32_usart_data_##n,     \
				  &spi_abov32_usart_cfg_##n, POST_KERNEL,                         \
				  CONFIG_SPI_INIT_PRIORITY, &spi_abov32_usart_api);

DT_INST_FOREACH_STATUS_OKAY(ABOV32_USART_SPI_INIT)
