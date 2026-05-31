# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ir_nec_test)

# Unit-test the real app NEC logic (app/src/nec.c) as a pure function, no HW.
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src)
target_sources(app PRIVATE src/main.c ${APP_SRC}/nec.c)
target_include_directories(app PRIVATE ${APP_SRC})
