# Copyright (c) 2026 ABOV Semiconductor Co., Ltd.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(button_interrupt)

target_sources(app PRIVATE src/main.c)
