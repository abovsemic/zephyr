# Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
# SPDX-License-Identifier: BSD-3-Clause

# abov_config.h and system_a31xxxx.h/.c (see modules/hal/abov32/audk32/
# CMakeLists.txt) are shared by every A31xxxx subfamily (A31C15x, A31C14x,
# A31G11x, ...), not just the one wired up below, so they belong at this
# product-line level rather than in the audk32 module itself or in a single
# series' CMakeLists.txt. This file is only added to the build when the
# active product line is a31xxxx (see zephyr/soc/abov/abov32/CMakeLists.txt),
# so these paths are safe unconditionally here.
zephyr_include_directories(
  ${ZEPHYR_ABOV32_MODULE_DIR}/audk32/ProductConfig/A31xxxx/Config
  ${ZEPHYR_ABOV32_MODULE_DIR}/audk32/Framework/CMSIS/Device/ABOV/Include
)

add_subdirectory(${SOC_SERIES})
