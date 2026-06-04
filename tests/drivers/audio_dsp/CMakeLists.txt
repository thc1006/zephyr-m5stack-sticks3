# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_dsp_test)

# Unit-test the pure capture-DSP logic (app/src/audio_dsp.c) as pure functions,
# no hardware and no codec.
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src)
target_sources(app PRIVATE src/main.c ${APP_SRC}/audio_dsp.c)
target_include_directories(app PRIVATE ${APP_SRC})
