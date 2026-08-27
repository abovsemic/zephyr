/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT abov_abov32_crc

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/kernel.h>

#include <hll_crc.h>
#include <type/crc_type.h>

/*
 * The ABOV32 CRC IP has a fixed set of hardware polynomials selected by the
 * CR.POLY field (see hll_crc.h / HLL_CRC_SetPoly()) -- there is no
 * user-programmable polynomial register. crc_abov32_begin() rejects any
 * (type, polynomial) pair other than the four fixed variants below, and
 * only CRC32_IEEE gets the hardware output-invert bit set: that inversion
 * (equivalent to a final XOR with 0xFFFFFFFF) is part of the CRC32/IEEE
 * definition itself, not something the zephyr,crc API exposes per call.
 */
struct crc_abov32_config {
	CRC_ID_e id;
};

static int crc_abov32_poly(enum crc_type type, crc_poly_t polynomial, CRC_POLY_e *poly)
{
	switch (type) {
	case CRC8:
		if (polynomial != CRC8_POLY) {
			return -EINVAL;
		}
		*poly = CRC_POLY_8;
		break;
	case CRC16:
		if (polynomial != CRC16_POLY) {
			return -EINVAL;
		}
		*poly = CRC_POLY_16;
		break;
	case CRC16_CCITT:
		if (polynomial != CRC16_CCITT_POLY) {
			return -EINVAL;
		}
		*poly = CRC_POLY_16_CCITT;
		break;
	case CRC32_IEEE:
		if (polynomial != CRC32_IEEE_POLY) {
			return -EINVAL;
		}
		*poly = CRC_POLY_32;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int crc_abov32_begin(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_abov32_config *config = dev->config;
	CRC_POLY_e poly;
	CRC_INP_e first_in;
	CRC_OUTP_e first_out;
	CRC_OUTP_INV_e inv;
	int ret;

	if (ctx == NULL) {
		return -EINVAL;
	}

	ret = crc_abov32_poly(ctx->type, ctx->polynomial, &poly);
	if (ret != 0) {
		return ret;
	}

	first_in = (ctx->reversed & CRC_FLAG_REVERSE_INPUT) ? CRC_INP_LSB : CRC_INP_MSB;
	/*
	 * Empirically, CRC_OUTP_MSB (not _LSB) is what makes this IP reflect
	 * the output: the OUTP field's LSB/MSB naming does not follow the
	 * same "logical direction" convention as CR.IN_REV.
	 */
	first_out = (ctx->reversed & CRC_FLAG_REVERSE_OUTPUT) ? CRC_OUTP_MSB : CRC_OUTP_LSB;
	inv = (ctx->type == CRC32_IEEE) ? CRC_OUTP_INV_ON : CRC_OUTP_INV_OFF;

	HLL_CRC_SetMode(config->id, CRC_MODE_CRC);
	HLL_CRC_SetPoly(config->id, poly);
	HLL_CRC_SetFirstIn(config->id, first_in);
	HLL_CRC_SetOutputConfig(config->id, first_out, inv);
	HLL_CRC_SetOpMode(config->id, false);
	HLL_CRC_ClearOutput(config->id);
	HLL_CRC_SetInitValue(config->id, ctx->seed);
	HLL_CRC_SetEnable(config->id, true);

	ctx->state = CRC_STATE_IN_PROGRESS;
	ctx->result = ctx->seed;

	return 0;
}

static int crc_abov32_update(const struct device *dev, struct crc_ctx *ctx, const void *buffer,
			      size_t bufsize)
{
	const struct crc_abov32_config *config = dev->config;

	if (ctx == NULL || (buffer == NULL && bufsize != 0)) {
		return -EINVAL;
	}

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	if (bufsize != 0) {
		HLL_CRC_SetData(config->id, (uint8_t *)buffer, (uint32_t)bufsize);
		ctx->result = HLL_CRC_GetResult(config->id);
	}

	return 0;
}

static int crc_abov32_finish(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_abov32_config *config = dev->config;

	if (ctx == NULL) {
		return -EINVAL;
	}

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	ctx->result = HLL_CRC_GetResult(config->id);
	HLL_CRC_SetEnable(config->id, false);
	ctx->state = CRC_STATE_IDLE;

	return 0;
}

static int crc_abov32_init(const struct device *dev)
{
	const struct crc_abov32_config *config = dev->config;

	if (HLL_CRC_SetClockEnable(config->id, true) != HAL_ERR_OK) {
		return -EIO;
	}

	return 0;
}

static DEVICE_API(crc, crc_abov32_api) = {
	.begin = crc_abov32_begin,
	.update = crc_abov32_update,
	.finish = crc_abov32_finish,
};

#define CRC_ABOV32_INIT(n)                                                                        \
	static const struct crc_abov32_config crc_abov32_cfg_##n = {                             \
		.id = (CRC_ID_e)DT_INST_PROP(n, crc_id),                                          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, crc_abov32_init, NULL, NULL, &crc_abov32_cfg_##n, POST_KERNEL,   \
			      CONFIG_CRC_DRIVER_INIT_PRIORITY, &crc_abov32_api);

DT_INST_FOREACH_STATUS_OKAY(CRC_ABOV32_INIT)
