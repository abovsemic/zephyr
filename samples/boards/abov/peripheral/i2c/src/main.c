/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * On-board I2C0<->I2C1 loopback: I2C0 acts as controller/master, I2C1 acts
 * as target/slave, both in this same image. Jumper the two buses together
 * on the board (PD0<->PA1 for SCL, PD1<->PA0 for SDA -- see i2c0_default/
 * i2c1_default in the board's pinctrl.dtsi) plus nothing else, since both
 * are already pulled up (bias-pull-up on both pinctrl groups).
 */
#define I2C_MASTER_NODE DT_NODELABEL(i2c0)
#define I2C_TARGET_NODE DT_NODELABEL(i2c1)
#define I2C_MASTER_LABEL "I2C0"
#define I2C_TARGET_LABEL "I2C1"
#define I2C_TARGET_ADDR 0x50
#define I2C_REG_COUNT 32

/*
 * Split the register map into two disjoint 16-byte halves so the sample's
 * write cycle and read cycle can never land on the same address, no matter
 * how many iterations run: writes always go to [0, I2C_WRITE_REGION), reads
 * always come from [I2C_WRITE_REGION, I2C_REG_COUNT). Each pointer is also
 * kept away from its half's last 3 bytes, so a 4-byte access never wraps
 * (via reg_ptr's modulo I2C_REG_COUNT) into the other half.
 */
#define I2C_WRITE_REGION 16
#define I2C_PTR_SPAN (I2C_WRITE_REGION - 4)

/*
 * Emulates a tiny register map, EEPROM-style: the first byte of a write
 * transaction sets the pointer, every following write byte stores at that
 * pointer (auto-incrementing), and reads return bytes starting at the
 * pointer (also auto-incrementing). This exercises i2c_abov32_isr()'s full
 * set of target_callbacks, driven by I2C1's own IRQ while I2C0's polling
 * transfer() runs from the main thread.
 */
static uint8_t regs[I2C_REG_COUNT];
static uint8_t reg_ptr;
static bool ptr_byte_pending;

static int target_write_requested(struct i2c_target_config *config)
{
	ARG_UNUSED(config);

	ptr_byte_pending = true;

	return 0;
}

static int target_write_received(struct i2c_target_config *config, uint8_t val)
{
	ARG_UNUSED(config);

	if (ptr_byte_pending) {
		reg_ptr = (uint8_t)(val % I2C_REG_COUNT);
		ptr_byte_pending = false;
	} else {
		regs[reg_ptr] = val;
		reg_ptr = (uint8_t)((reg_ptr + 1) % I2C_REG_COUNT);
	}

	return 0;
}

static int target_read_requested(struct i2c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);

	*val = regs[reg_ptr];

	return 0;
}

static int target_read_processed(struct i2c_target_config *config, uint8_t *val)
{
	ARG_UNUSED(config);

	reg_ptr = (uint8_t)((reg_ptr + 1) % I2C_REG_COUNT);
	*val = regs[reg_ptr];

	return 0;
}

static int target_stop(struct i2c_target_config *config)
{
	ARG_UNUSED(config);

	return 0;
}

static const struct i2c_target_callbacks target_callbacks = {
	.write_requested = target_write_requested,
	.write_received = target_write_received,
	.read_requested = target_read_requested,
	.read_processed = target_read_processed,
	.stop = target_stop,
};

static struct i2c_target_config target_cfg = {
	.address = I2C_TARGET_ADDR,
	.callbacks = &target_callbacks,
};

int main(void)
{
	const struct device *const i2c_master = DEVICE_DT_GET(I2C_MASTER_NODE);
	const struct device *const i2c_target = DEVICE_DT_GET(I2C_TARGET_NODE);
	uint32_t iter = 0;
	int ret;

	if (!device_is_ready(i2c_master)) {
		printk("I2C0 (master) device is not ready\n");
		return -ENODEV;
	}

	if (!device_is_ready(i2c_target)) {
		printk("I2C1 (target) device is not ready\n");
		return -ENODEV;
	}

	for (int i = 0; i < I2C_REG_COUNT; i++) {
		regs[i] = (uint8_t)(0xA0 + i);
	}

	ret = i2c_target_register(i2c_target, &target_cfg);
	if (ret != 0) {
		printk("i2c_target_register failed: %d\n", ret);
		return ret;
	}

	printk("ABOV32 I2C0<->I2C1 loopback sample -- I2C1 listening at 0x%02x\n",
	       I2C_TARGET_ADDR);

	while (1) {
		/*
		 * write_ptr stays in [0, I2C_PTR_SPAN) (the low half, minus
		 * room for a 4-byte window) and read_ptr stays in
		 * [I2C_WRITE_REGION, I2C_WRITE_REGION + I2C_PTR_SPAN) (the
		 * high half, same margin) -- see the I2C_WRITE_REGION comment
		 * above. The write side always carries a fresh 0xC0+i pattern
		 * that was never part of the original regs[] init, and the
		 * read side always comes from the untouched high half, so the
		 * two lines below can never show the same data, this
		 * iteration or any other.
		 */
		uint8_t write_ptr = (uint8_t)(iter % I2C_PTR_SPAN);
		uint8_t read_ptr = (uint8_t)(I2C_WRITE_REGION + (iter % I2C_PTR_SPAN));
		uint8_t wbuf[5];
		uint8_t rx[4];

		/* Master TX -> Slave RX: write a fresh 4-byte pattern starting at write_ptr. */
		wbuf[0] = write_ptr;
		for (int i = 0; i < 4; i++) {
			wbuf[1 + i] = (uint8_t)(0xC0 + ((write_ptr + i) % I2C_REG_COUNT));
		}

		ret = i2c_write(i2c_master, wbuf, sizeof(wbuf), I2C_TARGET_ADDR);
		if (ret != 0) {
			printk("[" I2C_MASTER_LABEL "=Master TX -> " I2C_TARGET_LABEL
			       "=Slave RX] failed (ptr=%u): %d\n",
			       write_ptr, ret);
		} else {
			printk("[" I2C_MASTER_LABEL "=Master TX -> " I2C_TARGET_LABEL
			       "=Slave RX] @0x%02x: %02x %02x %02x %02x\n",
			       write_ptr, wbuf[1], wbuf[2], wbuf[3], wbuf[4]);
		}

		/* Slave TX -> Master RX: read 4 bytes starting at read_ptr, untouched by the write above. */
		ret = i2c_write_read(i2c_master, I2C_TARGET_ADDR, &read_ptr, 1, rx, sizeof(rx));
		if (ret != 0) {
			printk("[" I2C_TARGET_LABEL "=Slave TX -> " I2C_MASTER_LABEL
			       "=Master RX] failed (ptr=%u): %d\n",
			       read_ptr, ret);
		} else {
			printk("[" I2C_TARGET_LABEL "=Slave TX -> " I2C_MASTER_LABEL
			       "=Master RX] @0x%02x: %02x %02x %02x %02x\n",
			       read_ptr, rx[0], rx[1], rx[2], rx[3]);
		}

		iter++;
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
