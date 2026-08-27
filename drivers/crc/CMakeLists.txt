# Copyright (c) 2025 Renesas Electronics Corporation
# Copyright 2025 NXP
# SPDX-License-Identifier: Apache-2.0

zephyr_syscall_header(${ZEPHYR_BASE}/include/zephyr/drivers/crc.h)

zephyr_library()

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_CRC_AMBIQ crc_ambiq.c)
zephyr_library_sources_ifdef(CONFIG_CRC_DRIVER_ABOV32 crc_abov32.c)
zephyr_library_sources_ifdef(CONFIG_CRC_DRIVER_NXP crc_nxp.c)
zephyr_library_sources_ifdef(CONFIG_CRC_DRIVER_NXP_LPC crc_nxp_lpc.c)
zephyr_library_sources_ifdef(CONFIG_CRC_DRIVER_RENESAS_RA crc_renesas_ra.c)
zephyr_library_sources_ifdef(CONFIG_CRC_DRIVER_SF32LB crc_sf32lb.c)
zephyr_library_sources_ifdef(CONFIG_CRC_DRIVER_SILABS_GPCRC crc_silabs_gpcrc.c)
zephyr_library_sources_ifdef(CONFIG_CRC_STM32 crc_stm32.c)
# zephyr-keep-sorted-stop

# abov_module.h (modules/hal/abov32/audk32/ProductConfig/A31xxxx/Config) only
# defines _MODULE_CRC -- which gates the HLL_CRC-backed CRC IP version
# dispatch (hal_crc_prv.h) that crc_abov32.c depends on -- when
# CONFIG_HAL_CRC == 1. That Keil RTE-style checkbox defaults to 0
# (abov_module_config.h) and, unlike USART, CRC has no other fallback that
# turns it on. zephyr_compile_definitions() is global regardless of which
# CMakeLists.txt calls it, so setting it here -- gated on the Kconfig symbol
# that already governs whether crc_abov32.c is even built -- means any app
# that enables the ABOV32 CRC driver gets it automatically, instead of every
# such app having to repeat this definition itself.
if(CONFIG_CRC_DRIVER_ABOV32)
  zephyr_compile_definitions(CONFIG_HAL_CRC=1)
endif()
