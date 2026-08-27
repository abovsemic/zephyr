/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* The devicetree node identifier for the "crc" */
#define CRC_NODE DT_CHOSEN(zephyr_crc)

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const uint8_t sample_data[8] = {0x0A, 0x2B, 0x4C, 0x6D, 0x8E, 0x49, 0x00, 0xC4};

struct crc_case {
	const char *name;
	enum crc_type type;
	crc_poly_t poly;
	crc_init_val_t seed;
	uint32_t reversed;
	crc_result_t expected;
};

/*
 * ABOV32 CRC0 (see zephyr/drivers/crc/crc_abov32.c) has a fixed set of
 * hardware polynomials: CRC-8, CRC-16, CRC-16/CCITT and CRC-32/IEEE. This
 * runs all of the ones this sample knows expected values for back-to-back
 * on a single boot, instead of the upstream sample's build-time
 * SAMPLE_CRC_VARIANT_* choice.
 */
static const struct crc_case crc_cases[] = {
	{
		.name = "CRC32-IEEE",
		.type = CRC32_IEEE,
		.poly = CRC32_IEEE_POLY,
		.seed = CRC32_IEEE_INIT_VAL,
		.reversed = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
		.expected = 0xCEA4A6C2U,
	},
	{
		.name = "CRC16",
		.type = CRC16,
		.poly = CRC16_POLY,
		.seed = CRC16_INIT_VAL,
		.reversed = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
		.expected = 0xD543U,
	},
	{
		.name = "CRC8",
		.type = CRC8,
		.poly = CRC8_POLY,
		.seed = CRC8_INIT_VAL,
		.reversed = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
		.expected = 0xB2U,
	},
};

int main(void)
{
	const struct device *const dev = DEVICE_DT_GET(CRC_NODE);
	int failures = 0;

	if (!device_is_ready(dev)) {
		printk("CRC device is not ready\n");
		return -ENODEV;
	}

	for (size_t i = 0; i < ARRAY_SIZE(crc_cases); i++) {
		const struct crc_case *c = &crc_cases[i];
		struct crc_ctx ctx = {
			.type = c->type,
			.polynomial = c->poly,
			.seed = c->seed,
			.reversed = c->reversed,
		};
		int ret;

		ret = crc_begin(dev, &ctx);
		if (ret != 0) {
			printk("%-11s crc_begin failed: %d\n", c->name, ret);
			failures++;
			continue;
		}

		ret = crc_update(dev, &ctx, sample_data, sizeof(sample_data));
		if (ret != 0) {
			printk("%-11s crc_update failed: %d\n", c->name, ret);
			failures++;
			continue;
		}

		ret = crc_finish(dev, &ctx);
		if (ret != 0) {
			printk("%-11s crc_finish failed: %d\n", c->name, ret);
			failures++;
			continue;
		}

		printk("%-11s result: 0x%08x", c->name, (unsigned int)ctx.result);

		ret = crc_verify(&ctx, c->expected);
		if (ret != 0) {
			printk(" -- FAILED (expected 0x%08x): %d\n", (unsigned int)c->expected,
			       ret);
			failures++;
		} else {
			printk(" -- OK (expected 0x%08x)\n", (unsigned int)c->expected);
		}
	}

	if (failures != 0) {
		printk("%d/%d CRC case(s) failed\n", failures, (int)ARRAY_SIZE(crc_cases));
		return -1;
	}

	printk("All %d CRC cases passed\n", (int)ARRAY_SIZE(crc_cases));

	return 0;
}
