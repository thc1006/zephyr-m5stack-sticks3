# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(es8311_codec_test)

# The emulator's test-backend header. Both the test and the emulator itself
# (drivers/audio/emul_es8311.c, compiled only for this test) include it. The path is
# relative to this test, so it resolves the same whether the tree is upstream Zephyr or an
# out-of-tree module.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

target_sources(app PRIVATE src/main.c)
