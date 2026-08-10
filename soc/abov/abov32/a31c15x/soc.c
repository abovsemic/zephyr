/*
 * Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file
 * @brief System/hardware module for A31C15x processor
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <cmsis_core.h>

#include "abov_config.h"

/**
 * @brief Perform basic hardware initialization at boot.
 *
 * This needs to be run from the very beginning.
 *
 * Deliberately calls PRV_CHIPSET_Init() (WDT disable, SCU register-write
 * enable, flash wait-state) instead of the vendor's SystemInit() from
 * system_a31xxxx.c: that function also relocates VTOR to a vendor
 * __VECTOR_TABLE that doesn't exist in a Zephyr build, since Zephyr owns the
 * vector table and VTOR itself.
 */
void soc_early_init_hook(void)
{
        PRV_CHIPSET_Init();
}
