# Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(crc)

# CONFIG_HAL_CRC=1 (needed for abov_module.h to define _MODULE_CRC) is set
# globally by drivers/crc/CMakeLists.txt whenever CONFIG_CRC_DRIVER_ABOV32
# is enabled, so this app doesn't need to set it itself.
target_sources(app PRIVATE src/main.c)
