/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * SPI1 (dedicated SPI peripheral, master) <-> USART11-as-SPI (peripheral
 * mode) cross-device sample: jumper SCK1(PE5)<->SCK11(PD4),
 * MOSI1(PE7)<->MOSI11(PD2), MISO1(PE6)<->MISO11(PD3) -- see
 * stk_a31c156_rln_a.dts. No CS: the slave side is always listening, it isn't
 * selected/deselected.
 *
 * The slave side runs in its own thread because spi_transceive() on a
 * peripheral-mode SPI bus blocks until an external master actually clocks
 * data through -- there is no callback-based "target" API for SPI the way
 * there is for I2C (see i2c_target_register()/the i2c sample).
 *
 * Master and slave here are two on-chip peripherals wired together on one
 * board, but they're still driven by two separate *software* threads
 * sharing this one CPU. The slave thread is given the same priority as
 * main() so k_yield() (called from both drivers' register-poll loops, see
 * spi_abov32.c/spi_abov32_usart.c) actually round-robins between them --
 * without that, main()'s non-blocking busy-wait transceive() would run to
 * completion without the CPU ever reaching the (lower-priority) slave
 * thread, and every byte but the first (pre-loaded before main() started)
 * would come back as stale/idle data.
 */

#define MASTER_NODE  DT_NODELABEL(remote_slave)
#define MASTER_OP    (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)
#define SLAVE_NODE   DT_NODELABEL(usart11_spi)
#define MASTER_LABEL "SPI1"
#define SLAVE_LABEL  "USART11-SPI"
#define XFER_LEN     8

static const struct spi_dt_spec master_spec = SPI_DT_SPEC_GET(MASTER_NODE, MASTER_OP);
static const struct device *const slave_dev = DEVICE_DT_GET(SLAVE_NODE);

static const struct spi_config slave_cfg = {
	.frequency = 0,
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.slave = 0,
};

#define SLAVE_STACK_SIZE 1024
#define SLAVE_PRIORITY   0 /* == CONFIG_MAIN_THREAD_PRIORITY, see the file comment above */

static void slave_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Slave TX data is deliberately a different range (0xA0+) than the
	 * master's (0x00+) so the two directions are visibly distinct in the
	 * log, not just round-trip echoes.
	 */
	uint32_t iter = 0;

	while (1) {
		uint8_t tx[XFER_LEN];
		uint8_t rx[XFER_LEN] = {0};
		struct spi_buf tx_buf = {.buf = tx, .len = sizeof(tx)};
		struct spi_buf rx_buf = {.buf = rx, .len = sizeof(rx)};
		struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
		struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};
		int ret;

		for (int i = 0; i < XFER_LEN; i++) {
			tx[i] = (uint8_t)(0xA0 + iter + i);
		}

		/* Blocks here until SPI1 clocks a byte in. */
		ret = spi_transceive(slave_dev, &slave_cfg, &tx_set, &rx_set);
		if (ret != 0) {
			printk("[%s] spi_transceive (slave) failed: %d\n", SLAVE_LABEL, ret);
		}

		iter++;
	}
}

K_THREAD_DEFINE(spi_slave_tid, SLAVE_STACK_SIZE, slave_thread, NULL, NULL, NULL, SLAVE_PRIORITY,
		 0, 0);

int main(void)
{
	uint32_t iter = 0;

	if (!spi_is_ready_dt(&master_spec)) {
		printk("SPI1 device is not ready\n");
		return -ENODEV;
	}

	if (!device_is_ready(slave_dev)) {
		printk("USART11-SPI device is not ready\n");
		return -ENODEV;
	}

	printk("ABOV32 %s(master) <-> %s(slave) cross-device sample -- %u Hz\n", MASTER_LABEL,
	       SLAVE_LABEL, master_spec.config.frequency);

	/* Give the slave thread a head start so it's already blocked waiting
	 * for clock before the master issues its first transfer.
	 */
	k_sleep(K_MSEC(100));

	while (1) {
		uint8_t tx[XFER_LEN];
		uint8_t rx[XFER_LEN] = {0};
		struct spi_buf tx_buf = {.buf = tx, .len = sizeof(tx)};
		struct spi_buf rx_buf = {.buf = rx, .len = sizeof(rx)};
		struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
		struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};
		int ret;

		for (int i = 0; i < XFER_LEN; i++) {
			tx[i] = (uint8_t)(iter + i);
		}

		/* A single full-duplex call captures both directions at once:
		 * tx[] is what the master sent (-> slave RX), rx[] is what
		 * the slave shifted out at the same time (-> master RX).
		 */
		ret = spi_transceive_dt(&master_spec, &tx_set, &rx_set);
		if (ret != 0) {
			printk("[%s] spi_transceive_dt (master) failed: %d\n", MASTER_LABEL, ret);
		} else {
			printk("[%s=Master TX -> %s=Slave RX] %02x %02x %02x %02x %02x %02x %02x "
			       "%02x\n",
			       MASTER_LABEL, SLAVE_LABEL, tx[0], tx[1], tx[2], tx[3], tx[4], tx[5],
			       tx[6], tx[7]);
			printk("[%s=Slave TX -> %s=Master RX] %02x %02x %02x %02x %02x %02x %02x "
			       "%02x\n",
			       SLAVE_LABEL, MASTER_LABEL, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
			       rx[6], rx[7]);
		}

		iter++;
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
