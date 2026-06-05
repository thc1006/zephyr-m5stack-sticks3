# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(battery_test)

# Unit-test the pure battery-display logic (app/src/battery.c) as pure functions,
# no hardware and no fuel gauge. The SoC% itself comes from the upstream
# zephyr,fuel-gauge-composite OCV lookup (validated on hardware at HW-017).
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src)
target_sources(app PRIVATE src/main.c ${APP_SRC}/battery.c)
target_include_directories(app PRIVATE ${APP_SRC})
