/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define DT_DRV_COMPAT abov_abov32_i2c

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <hll_i2c.h>

#include "i2c-priv.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_abov32, CONFIG_I2C_LOG_LEVEL);

/*
 * This IP has no independent "controller" vs. "target" hardware mode (see
 * hll_i2c.h's file-level note): SCLL/SCLH always divide SystemPeriClock, and
 * whichever role currently owns the bus is purely a software distinction.
 *
 * i2c_abov32_transfer() (controller/master role) is polling-only, modeled on
 * HAL_I2C_Transmit()/HAL_I2C_Receive()'s own polling branch: it busy-waits on
 * CR.IIF. Unlike the vendor's own polling-mode code, CR.I2CnIEN (INTEN) must
 * actually be set to 1 on this silicon for IIF to latch at all -- confirmed
 * on real hardware, and presumably a gap in the vendor's own polling-mode
 * example. That alone doesn't raise the NVIC interrupt, though: the IRQ
 * vector is only ever IRQ_CONNECT()-ed and enabled inside
 * i2c_abov32_target_register(), not at init() time, so a master-only
 * instance's transfer() can safely set INTEN without i2c_abov32_isr() ever
 * actually running and racing its own polling loop for the same event. Once
 * a target *is* registered, i2c_abov32_isr() drives target-role status codes
 * from the NVIC vector as normal. Because only one of the two is ever wired
 * to the NVIC at a time, the ISR only ever needs to handle target-role
 * status codes -- it is never entered mid-transfer(). This mirrors HAL_I2C's
 * own design, which likewise only ever runs one fixed role (I2C_MODE_e) per
 * instance; a single instance switching between the two roles at the same
 * time on the same bus is not supported here, same as it isn't in HAL_I2C.
 */

#define I2C_ABOV32_POLL_TIMEOUT 100000

struct i2c_abov32_config {
	I2C_ID_e id;
	uint32_t bitrate;
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config_func)(const struct device *dev);
};

struct i2c_abov32_data {
	struct k_sem lock;
	uint32_t dev_config;
	struct i2c_target_config *target_cfg;
	uint8_t last_rx_byte;
	bool last_rx_valid;
};

static void i2c_abov32_dump_regs(I2C_ID_e id, const char *tag)
{
	I2C_Type *reg = HLL_I2C_REG(id);

	LOG_ERR("id %d [%s]: DR=0x%08x SR=0x%08x SAR=0x%08x CR=0x%08x SCLL=0x%08x SCLH=0x%08x SDH=0x%08x",
		(int)id, tag, reg->DR, reg->SR, reg->SAR, reg->CR, reg->SCLL, reg->SCLH,
		reg->SDH);
}

static int i2c_abov32_wait_status(I2C_ID_e id, uint32_t *status)
{
	int32_t timeout = I2C_ABOV32_POLL_TIMEOUT;

	while (!HLL_I2C_GetIntrFlag(id)) {
		if (--timeout <= 0) {
			i2c_abov32_dump_regs(id, "timeout");
			return -ETIMEDOUT;
		}
	}

	*status = HLL_I2C_GetStatus(id);

	return 0;
}

static int i2c_abov32_write_msg(I2C_ID_e id, struct i2c_msg *msg)
{
	uint32_t status;
	uint32_t idx = 0;
	int ret;

	ret = i2c_abov32_wait_status(id, &status);
	if (ret != 0) {
		return ret;
	}

	if (status != I2C_MASTER_TX_ADDR_ACK) {
		return -EIO;
	}

	while (idx < msg->len) {
		HLL_I2C_ClearStatus(id);
		HLL_I2C_TransmitByte(id, msg->buf[idx]);

		ret = i2c_abov32_wait_status(id, &status);
		if (ret != 0) {
			return ret;
		}

		if (status != I2C_TX_DATA_ACK) {
			return -EIO;
		}

		idx++;
	}

	return 0;
}

static int i2c_abov32_read_msg(I2C_ID_e id, struct i2c_msg *msg)
{
	uint32_t status;
	uint32_t idx = 0;
	int ret;

	ret = i2c_abov32_wait_status(id, &status);
	if (ret != 0) {
		return ret;
	}

	if (status != I2C_MASTER_RX_ADDR_ACK) {
		return -EIO;
	}

	if (msg->len == 1) {
		HLL_I2C_SetAck(id, false);
	}

	while (idx < msg->len) {
		HLL_I2C_ClearStatus(id);

		ret = i2c_abov32_wait_status(id, &status);
		if (ret != 0) {
			return ret;
		}

		if (status != I2C_RX_DATA_ACK && status != I2C_MASTER_RX_DATA_NOACK) {
			return -EIO;
		}

		msg->buf[idx] = HLL_I2C_ReceiveByte(id);
		idx++;

		if (idx + 1 == msg->len) {
			HLL_I2C_SetAck(id, false);
		}
	}

	return 0;
}

/*
 * All messages in one transfer() call share a single START: each message
 * re-addresses the bus with a repeated START (see HAL_I2C's own
 * I2C_MASTER_TX_ADDR_NOACK retry, which re-triggers HLL_I2C_SetStart() on an
 * already-active bus the same way), and only the very last message ends
 * with a real STOP. Per-message I2C_MSG_STOP/I2C_MSG_RESTART flags are not
 * consulted -- this covers the common write-register-then-read pattern our
 * samples use, not arbitrary flag combinations.
 */
static int i2c_abov32_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
				uint16_t addr)
{
	const struct i2c_abov32_config *config = dev->config;
	struct i2c_abov32_data *data = dev->data;
	I2C_ID_e id = config->id;
	uint32_t stop_status;
	int ret = 0;

	if (num_msgs == 0) {
		return 0;
	}

	k_sem_take(&data->lock, K_FOREVER);

	/*
	 * Unlike vendor's own polling-mode HAL_I2C_Transmit()/Receive() (which
	 * leaves CR.INTEN disabled and just polls CR.IIF), this silicon needs
	 * CR.INTEN=1 for IIF to latch at all -- confirmed on real hardware.
	 * This is safe to leave enabled without an actual NVIC interrupt ever
	 * firing here: the IRQ vector is only connected in
	 * i2c_abov32_target_register(), never at init() time, specifically so
	 * a master-only instance's polling loop never races
	 * i2c_abov32_isr() for the same event.
	 */
	HLL_I2C_SetIntrEnable(id, true);

	for (uint8_t i = 0; i < num_msgs; i++) {
		struct i2c_msg *msg = &msgs[i];
		bool is_read = (msg->flags & I2C_MSG_READ) != 0;

		HLL_I2C_SetAck(id, true);
		HLL_I2C_ClearStatus(id);
		HLL_I2C_SetStart(id, true);
		HLL_I2C_TransmitByte(id, (uint8_t)((addr << 1) | (is_read ? 1U : 0U)));

		ret = is_read ? i2c_abov32_read_msg(id, msg) : i2c_abov32_write_msg(id, msg);
		if (ret != 0) {
			break;
		}
	}

	HLL_I2C_SetStop(id, true);
	(void)i2c_abov32_wait_status(id, &stop_status);
	HLL_I2C_SetStop(id, false);
	HLL_I2C_ClearStatus(id);

	k_sem_give(&data->lock);

	return ret;
}

static int i2c_abov32_configure(const struct device *dev, uint32_t dev_config_raw)
{
	const struct i2c_abov32_config *config = dev->config;
	struct i2c_abov32_data *data = dev->data;
	uint32_t freq;
	uint16_t scll, sclh;

	if (dev_config_raw & I2C_ADDR_10_BITS) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config_raw)) {
	case I2C_SPEED_STANDARD:
		freq = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		freq = KHZ(400);
		break;
	case I2C_SPEED_FAST_PLUS:
		freq = MHZ(1);
		break;
	default:
		return -ENOTSUP;
	}

	HLL_I2C_CalcSclPeriod(freq, &scll, &sclh);

	k_sem_take(&data->lock, K_FOREVER);

	HLL_I2C_SetSclLow(config->id, scll);
	HLL_I2C_SetSclHigh(config->id, sclh);
	data->dev_config = dev_config_raw;

	k_sem_give(&data->lock);

	return 0;
}

static int i2c_abov32_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_abov32_data *data = dev->data;

	if (data->dev_config == 0) {
		return -EIO;
	}

	*dev_config = data->dev_config;

	return 0;
}

static int i2c_abov32_target_register(const struct device *dev, struct i2c_target_config *cfg)
{
	const struct i2c_abov32_config *config = dev->config;
	struct i2c_abov32_data *data = dev->data;

	if (cfg == NULL) {
		return -EINVAL;
	}

	if (cfg->flags & I2C_TARGET_FLAGS_ADDR_10_BITS) {
		return -ENOTSUP;
	}

	if (data->target_cfg != NULL) {
		return -EBUSY;
	}

	data->target_cfg = cfg;
	data->last_rx_valid = false;

	HLL_I2C_SetOwnSlaveAddr(config->id, (uint8_t)cfg->address);
	HLL_I2C_SetAck(config->id, true);
	HLL_I2C_ClearStatus(config->id);

	/*
	 * The NVIC vector is only ever connected here, not at init() time:
	 * a master-only instance's transfer() also needs CR.INTEN=1 for IIF
	 * to latch on this silicon, and if the NVIC IRQ were already wired
	 * up, that would fire i2c_abov32_isr() (which just clears status and
	 * returns for a null target_cfg) *during* transfer()'s own polling,
	 * racing it and eating the event before the poll loop observes it.
	 */
	config->irq_config_func(dev);
	HLL_I2C_SetIntrEnable(config->id, true);

	return 0;
}

static int i2c_abov32_target_unregister(const struct device *dev, struct i2c_target_config *cfg)
{
	const struct i2c_abov32_config *config = dev->config;
	struct i2c_abov32_data *data = dev->data;

	if (data->target_cfg != cfg) {
		return -EINVAL;
	}

	HLL_I2C_SetIntrEnable(config->id, false);
	data->target_cfg = NULL;

	return 0;
}

/*
 * Only reached while a target is registered (see the file-level note): the
 * address phase's ACK is already on the wire by the time these status codes
 * are visible, so write_requested()/read_requested() returning nonzero can
 * only affect the ACK of the byte that follows, not the address byte itself.
 */
static void i2c_abov32_isr(const struct device *dev)
{
	const struct i2c_abov32_config *config = dev->config;
	struct i2c_abov32_data *data = dev->data;
	I2C_ID_e id = config->id;
	struct i2c_target_config *target_cfg = data->target_cfg;
	uint32_t status;
	uint8_t byte;
	int ret;

	if (target_cfg == NULL) {
		HLL_I2C_ClearStatus(id);
		return;
	}

	status = HLL_I2C_GetStatus(id);

	switch (status) {
	case I2C_SLAVE_RX_SEL_ACK:
	case I2C_SLAVE_RX_GC_ACK:
		data->last_rx_valid = false;
		ret = target_cfg->callbacks->write_requested(target_cfg);
		HLL_I2C_SetAck(id, ret == 0);
		break;

	case I2C_RX_DATA_ACK:
		byte = HLL_I2C_ReceiveByte(id);
		/*
		 * This silicon reports two kinds of protocol noise as ordinary
		 * I2C_RX_DATA_ACK events, confirmed on real hardware:
		 *
		 * 1. Right after a STOP -- which this version never actually
		 *    reports as I2C_SLAVE_RX_DONE when another transfer()
		 *    follows immediately -- DR still holds an exact duplicate
		 *    of the last real byte received (nothing new was clocked
		 *    in; the STOP itself doesn't move DR).
		 * 2. Right before the real I2C_SLAVE_RX_SEL_ACK/TX_SEL_ACK of
		 *    the START (or repeated-START) that follows, DR holds our
		 *    own address byte, shifted, with either R/W bit -- a
		 *    one-event-early "preview" of the address-match that's
		 *    about to be properly classified.
		 *
		 * Neither is real write data; skip both without touching
		 * write_received() or last_rx_byte/last_rx_valid.
		 */
		if ((uint8_t)(byte >> 1) == (uint8_t)target_cfg->address) {
			HLL_I2C_SetAck(id, true);
			break;
		}
		if (data->last_rx_valid && byte == data->last_rx_byte) {
			HLL_I2C_SetAck(id, true);
			break;
		}
		ret = target_cfg->callbacks->write_received(target_cfg, byte);
		HLL_I2C_SetAck(id, ret == 0);
		data->last_rx_byte = byte;
		data->last_rx_valid = true;
		break;

	case I2C_SLAVE_RX_DONE:
		data->last_rx_valid = false;
		if (target_cfg->callbacks->stop != NULL) {
			target_cfg->callbacks->stop(target_cfg);
		}
		HLL_I2C_SetAck(id, true);
		break;

	case I2C_SLAVE_TX_SEL_ACK:
	case I2C_SLAVE_TX_GC_ACK:
		ret = target_cfg->callbacks->read_requested(target_cfg, &byte);
		HLL_I2C_TransmitByte(id, (ret == 0) ? byte : 0xFFU);
		break;

	case I2C_TX_DATA_ACK:
		ret = target_cfg->callbacks->read_processed(target_cfg, &byte);
		HLL_I2C_TransmitByte(id, (ret == 0) ? byte : 0xFFU);
		break;

	case I2C_SLAVE_TX_DATA_NOACK:
	case I2C_SLAVE_TX_DONE:
		if (target_cfg->callbacks->stop != NULL) {
			target_cfg->callbacks->stop(target_cfg);
		}
		break;

	default:
		break;
	}

	HLL_I2C_ClearStatus(id);
}

static DEVICE_API(i2c, i2c_abov32_api) = {
	.configure = i2c_abov32_configure,
	.get_config = i2c_abov32_get_config,
	.transfer = i2c_abov32_transfer,
	.target_register = i2c_abov32_target_register,
	.target_unregister = i2c_abov32_target_unregister,
};

static int i2c_abov32_init(const struct device *dev)
{
	const struct i2c_abov32_config *config = dev->config;
	struct i2c_abov32_data *data = dev->data;
	uint32_t bitrate_cfg;
	int ret;

	k_sem_init(&data->lock, 1, 1);

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}

	if (HLL_I2C_SetClockEnable(config->id, true) != HAL_ERR_OK) {
		return -EIO;
	}

	HLL_I2C_SetEnable(config->id, true);
	HLL_I2C_SetOwnSlaveAddr(config->id, 0);
	HLL_I2C_SetGeneralCallEnable(config->id, false);
	HLL_I2C_SetSdaHoldEnable(config->id, false);
	HLL_I2C_SetSdaHold(config->id, 0);

	bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);

	return i2c_abov32_configure(dev, I2C_MODE_CONTROLLER | bitrate_cfg);
}

#define ABOV32_I2C_INIT(n)                                                                        \
	PINCTRL_DT_INST_DEFINE(n);                                                                \
                                                                                                   \
	static void i2c_abov32_irq_config_##n(const struct device *dev)                          \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), i2c_abov32_isr,            \
			    DEVICE_DT_INST_GET(n), 0);                                            \
		irq_enable(DT_INST_IRQN(n));                                                      \
	}                                                                                          \
                                                                                                   \
	static const struct i2c_abov32_config i2c_abov32_cfg_##n = {                             \
		.id = (I2C_ID_e)DT_INST_PROP(n, i2c_id),                                          \
		.bitrate = DT_INST_PROP(n, clock_frequency),                                      \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                        \
		.irq_config_func = i2c_abov32_irq_config_##n,                                     \
	};                                                                                         \
                                                                                                   \
	static struct i2c_abov32_data i2c_abov32_data_##n;                                        \
                                                                                                   \
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_abov32_init, NULL, &i2c_abov32_data_##n,                 \
				  &i2c_abov32_cfg_##n, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,     \
				  &i2c_abov32_api);

DT_INST_FOREACH_STATUS_OKAY(ABOV32_I2C_INIT)
