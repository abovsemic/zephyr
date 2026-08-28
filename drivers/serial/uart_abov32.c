/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_uart

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>

#include <hll_uart.h>

struct uart_abov32_config {
	UART_ID_e id;
	const struct pinctrl_dev_config *pcfg;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct uart_abov32_data {
	struct uart_config cfg;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t callback;
	void *cb_data;
#endif
};

static int uart_abov32_data_bits(uint8_t data_bits, UART_DATA_e *out)
{
	switch (data_bits) {
	case UART_CFG_DATA_BITS_5:
		*out = UART_DATA_5;
		break;
	case UART_CFG_DATA_BITS_6:
		*out = UART_DATA_6;
		break;
	case UART_CFG_DATA_BITS_7:
		*out = UART_DATA_7;
		break;
	case UART_CFG_DATA_BITS_8:
		*out = UART_DATA_8;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int uart_abov32_parity(uint8_t parity, UART_PARITY_e *out)
{
	switch (parity) {
	case UART_CFG_PARITY_NONE:
		*out = UART_PARITY_NONE;
		break;
	case UART_CFG_PARITY_ODD:
		*out = UART_PARITY_ODD;
		break;
	case UART_CFG_PARITY_EVEN:
		*out = UART_PARITY_EVEN;
		break;
	case UART_CFG_PARITY_MARK:
		*out = UART_PARITY_SP_1;
		break;
	case UART_CFG_PARITY_SPACE:
		*out = UART_PARITY_SP_0;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int uart_abov32_stop_bits(uint8_t stop_bits, UART_STOP_e *out)
{
	switch (stop_bits) {
	case UART_CFG_STOP_BITS_1:
		*out = UART_STOP_1;
		break;
	case UART_CFG_STOP_BITS_2:
		*out = UART_STOP_2;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int uart_abov32_configure(const struct device *dev, const struct uart_config *cfg)
{
	const struct uart_abov32_config *config = dev->config;
	struct uart_abov32_data *data = dev->data;
	UART_DATA_e data_bits;
	UART_PARITY_e parity;
	UART_STOP_e stop_bits;
	uint32_t bdr, bfr;
	int ret;

	if (cfg->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) {
		return -ENOTSUP;
	}

	ret = uart_abov32_data_bits(cfg->data_bits, &data_bits);
	if (ret != 0) {
		return ret;
	}

	ret = uart_abov32_parity(cfg->parity, &parity);
	if (ret != 0) {
		return ret;
	}

	ret = uart_abov32_stop_bits(cfg->stop_bits, &stop_bits);
	if (ret != 0) {
		return ret;
	}

	HLL_UART_SetFormat(config->id, parity, data_bits, stop_bits);

	HLL_UART_CalcUartBaud(cfg->baudrate, &bdr, &bfr);
	HLL_UART_SetBaudRate(config->id, bdr, bfr);

	data->cfg = *cfg;

	return 0;
}

static int uart_abov32_config_get(const struct device *dev, struct uart_config *cfg)
{
	struct uart_abov32_data *data = dev->data;

	*cfg = data->cfg;

	return 0;
}

static int uart_abov32_poll_in(const struct device *dev, unsigned char *c)
{
	const struct uart_abov32_config *config = dev->config;

	if (!HLL_UART_GetRxReady(config->id)) {
		return -1;
	}

	*c = (unsigned char)HLL_UART_ReceiveByte(config->id);

	return 0;
}

static void uart_abov32_poll_out(const struct device *dev, unsigned char c)
{
	const struct uart_abov32_config *config = dev->config;

	HLL_UART_TransmitByte(config->id, c);
	while (!HLL_UART_GetTxReady(config->id)) {
	}
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int uart_abov32_fifo_fill(const struct device *dev, const uint8_t *tx_data, int len)
{
	const struct uart_abov32_config *config = dev->config;
	int num_tx = 0;

	while (num_tx < len && HLL_UART_GetTxReady(config->id)) {
		HLL_UART_TransmitByte(config->id, tx_data[num_tx]);
		num_tx++;
	}

	return num_tx;
}

static int uart_abov32_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
	const struct uart_abov32_config *config = dev->config;
	int num_rx = 0;

	while (num_rx < size && HLL_UART_GetRxReady(config->id)) {
		rx_data[num_rx++] = HLL_UART_ReceiveByte(config->id);
	}

	return num_rx;
}

static void uart_abov32_irq_tx_enable(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	HLL_UART_SetTxReadyIntrEnable(config->id, true);
}

static void uart_abov32_irq_tx_disable(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	HLL_UART_SetTxReadyIntrEnable(config->id, false);
}

static int uart_abov32_irq_tx_ready(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	return HLL_UART_GetTxReady(config->id) ? 1 : 0;
}

static int uart_abov32_irq_tx_complete(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	return HLL_UART_GetTxComplete(config->id) ? 1 : 0;
}

static void uart_abov32_irq_rx_enable(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	HLL_UART_SetRxIntrEnable(config->id, true);
}

static void uart_abov32_irq_rx_disable(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	HLL_UART_SetRxIntrEnable(config->id, false);
}

static int uart_abov32_irq_rx_ready(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;

	return HLL_UART_GetRxReady(config->id) ? 1 : 0;
}

static int uart_abov32_irq_is_pending(const struct device *dev)
{
	return uart_abov32_irq_tx_ready(dev) || uart_abov32_irq_rx_ready(dev);
}

static void uart_abov32_irq_update(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static void uart_abov32_irq_callback_set(const struct device *dev,
					  uart_irq_callback_user_data_t cb, void *cb_data)
{
	struct uart_abov32_data *data = dev->data;

	data->callback = cb;
	data->cb_data = cb_data;
}

static void uart_abov32_isr(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;
	struct uart_abov32_data *data = dev->data;

	/* Reading IIR/ELSR acknowledges the latched interrupt cause on this
	 * 16550-style IP; the callback is expected to poll irq_tx_ready() /
	 * irq_rx_ready() itself to see what actually needs servicing.
	 */
	(void)HLL_UART_GetIntrStatus(config->id);
	(void)HLL_UART_GetLineStatus(config->id);

	if (data->callback) {
		data->callback(dev, data->cb_data);
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static DEVICE_API(uart, uart_abov32_driver_api) = {
	.poll_in = uart_abov32_poll_in,
	.poll_out = uart_abov32_poll_out,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = uart_abov32_configure,
	.config_get = uart_abov32_config_get,
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = uart_abov32_fifo_fill,
	.fifo_read = uart_abov32_fifo_read,
	.irq_tx_enable = uart_abov32_irq_tx_enable,
	.irq_tx_disable = uart_abov32_irq_tx_disable,
	.irq_tx_ready = uart_abov32_irq_tx_ready,
	.irq_tx_complete = uart_abov32_irq_tx_complete,
	.irq_rx_enable = uart_abov32_irq_rx_enable,
	.irq_rx_disable = uart_abov32_irq_rx_disable,
	.irq_rx_ready = uart_abov32_irq_rx_ready,
	.irq_is_pending = uart_abov32_irq_is_pending,
	.irq_update = uart_abov32_irq_update,
	.irq_callback_set = uart_abov32_irq_callback_set,
#endif
};

static int uart_abov32_init(const struct device *dev)
{
	const struct uart_abov32_config *config = dev->config;
	struct uart_abov32_data *data = dev->data;
	int ret;

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}

	HLL_UART_SetClockEnable(config->id, true);

	/*
	 * This IP's baud-rate generator numerator comes from the MCCR mux
	 * (see UART_GetNumer() in hal_uart_v1x.h -- UART_FEATURE_MCCR is set
	 * for this SoC), not a fixed PCLK like the USART peripheral. Without
	 * selecting a source here it stays unset and every baud rate divides
	 * out to 0. MCLK/2 matches the vendor's own UART debug-console
	 * default for this chip (see DEBUG_UART_CLK_MCCR/_DIV in
	 * debug_a31c15x.h).
	 */
	HLL_UART_SetClkSource(config->id, UART_CLK_MCCR_MCLK, 2);

	HLL_UART_ClearControl(config->id);

	ret = uart_abov32_configure(dev, &data->cfg);
	if (ret != 0) {
		return ret;
	}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	config->irq_config_func(dev);
#endif

	return 0;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
#define ABOV32_UART_IRQ_HANDLER(n)                                                                \
	static void uart_abov32_irq_config_##n(const struct device *dev)                         \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                  \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), uart_abov32_isr,           \
			    DEVICE_DT_INST_GET(n), 0);                                            \
		irq_enable(DT_INST_IRQN(n));                                                      \
	}
#define ABOV32_UART_IRQ_FUNC_INIT(n) .irq_config_func = uart_abov32_irq_config_##n,
#else
#define ABOV32_UART_IRQ_HANDLER(n)
#define ABOV32_UART_IRQ_FUNC_INIT(n)
#endif

#define ABOV32_UART_INIT(n)                                                                      \
	PINCTRL_DT_INST_DEFINE(n);                                                                \
	ABOV32_UART_IRQ_HANDLER(n)                                                                \
                                                                                                   \
	static const struct uart_abov32_config uart_abov32_cfg_##n = {                           \
		.id = (UART_ID_e)DT_INST_PROP(n, uart_id),                                        \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                        \
		ABOV32_UART_IRQ_FUNC_INIT(n)                                                      \
	};                                                                                         \
                                                                                                   \
	static struct uart_abov32_data uart_abov32_data_##n = {                                  \
		.cfg = {                                                                          \
			.baudrate = DT_INST_PROP(n, current_speed),                               \
			.parity = UART_CFG_PARITY_NONE,                                           \
			.stop_bits = UART_CFG_STOP_BITS_1,                                        \
			.data_bits = UART_CFG_DATA_BITS_8,                                        \
			.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,                                     \
		},                                                                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, uart_abov32_init, NULL, &uart_abov32_data_##n,                  \
			      &uart_abov32_cfg_##n, PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY,    \
			      &uart_abov32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ABOV32_UART_INIT)
