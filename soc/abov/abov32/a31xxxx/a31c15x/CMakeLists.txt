# SPDX-License-Identifier: BSD-3-Clause 

zephyr_include_directories(${ZEPHYR_BASE}/drivers)
zephyr_sources(soc.c)

zephyr_sources_ifdef(CONFIG_PM power.c)
zephyr_sources_ifdef(CONFIG_POWEROFF poweroff.c)

zephyr_include_directories(.)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")

# config_a31c15x.h/board_a31c15x.h/etc. (see modules/hal/abov32/audk32/
# CMakeLists.txt) are specific to this series, not the whole A31xxxx product
# line, so they're added here rather than in a31xxxx/CMakeLists.txt.
zephyr_include_directories(
  ${ZEPHYR_ABOV32_MODULE_DIR}/audk32/ProductConfig/A31xxxx/A31C15x/Config
)

# audk32's abov_config.h picks the vendor headers for this series via these
# EXTRN_* overrides (see modules/hal/abov32/audk32/CMakeLists.txt). This file
# is only added to the build when SOC_SERIES is a31c15x, so EXTRN_SUBFAMILY_
# A31C15x is safe unconditionally here.
zephyr_compile_definitions(EXTRN_SUBFAMILY_A31C15x)

# A31C156 is the only chip wired up in this series so far. When a second one
# is added, give it its own elseif(CONFIG_SOC_...) branch here instead of
# assuming A31C156RLN.
if(CONFIG_SOC_A31C156)
  zephyr_compile_definitions(EXTRN_VARIANT_A31C156RLN)
endif()
