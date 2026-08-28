/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * The devicetree node identifier for the ABOV32 UART1 peripheral.
 *
 * UART0's TX/RX pins (PF0/PF1) are shared with this board's on-board SWD
 * debug port, so UART1 (PB6/PB7) is used instead -- see
 * stk_a31c156_rln_a-pinctrl.dtsi.
 */
#define UART_TEST_NODE DT_NODELABEL(uart1)

static const char test_pattern[] = "ABOV32 UART1 self-test pattern\r\n";

/*
 * This IP has no internal loopback bit (SET_UART_DCR_LBON is a no-op on the
 * version this SoC uses -- see hal_uart_v_01_00_08.h), so the only way to
 * verify RX from software alone is an external jumper between the TX and RX
 * pins (PB6/PB7 on this board -- see stk_a31c156_rln_a-pinctrl.dtsi).
 *
 * The receiver is a single register (RBR), not a FIFO: an unread byte is
 * silently overwritten by the next one to arrive. So each byte is sent and
 * read back before the next is sent, instead of writing the whole pattern
 * up front and reading it back afterwards -- the latter would only ever
 * see whatever byte happened to still be sitting in RBR once TX caught up
 * with (or outran) RX.
 *
 * With the jumper in place, this loops the test pattern back to itself and
 * reports PASS/FAIL on the console (usart10). Without it, nothing arrives
 * within the timeout and the sample falls through to interactive echo mode:
 * connect a USB-TTL adapter to PB6 (TX), PB7 (RX) and GND, open a terminal
 * at 115200 8N1, and whatever you type should be echoed back.
 */
static bool try_loopback_test(const struct device *dev)
{
	for (size_t i = 0; i < sizeof(test_pattern) - 1; i++) {
		unsigned char expected = (unsigned char)test_pattern[i];
		int64_t deadline = k_uptime_get() + 50;
		unsigned char c;

		uart_poll_out(dev, expected);

		while (uart_poll_in(dev, &c) != 0) {
			if (k_uptime_get() > deadline) {
				return false;
			}
		}

		if (c != expected) {
			printk("UART1 loopback: byte %zu mismatch (got 0x%02x, expected 0x%02x)\n",
			       i, c, expected);
			return false;
		}
	}

	return true;
}

int main(void)
{
	const struct device *const dev = DEVICE_DT_GET(UART_TEST_NODE);

	if (!device_is_ready(dev)) {
		printk("UART1 device is not ready\n");
		return -ENODEV;
	}

	printk("ABOV32 UART1 driver sample\n");

	if (try_loopback_test(dev)) {
		printk("UART1 loopback test PASSED (TX->RX jumper detected)\n");
	} else {
		printk("UART1 loopback test: pattern did not loop back correctly.\n");
		printk("This is expected without a TX(PB6)-RX(PB7) jumper.\n");
	}

	printk("Entering interactive echo mode -- connect a USB-TTL adapter to\n"
	       "PB6 (TX) / PB7 (RX) / GND at 115200 8N1 and type characters;\n"
	       "they should be echoed back.\n");

	while (1) {
		unsigned char c;

		if (uart_poll_in(dev, &c) != 0) {
			continue;
		}

		uart_poll_out(dev, c);
		if (c == '\r') {
			uart_poll_out(dev, '\n');
		}
	}

	return 0;
}
