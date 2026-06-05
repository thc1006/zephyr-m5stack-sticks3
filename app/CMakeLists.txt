cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(m5sticks3_validation)

target_sources(app PRIVATE src/main.c src/ui.c src/status.c src/font.c src/gfx.c src/battery.c)
target_sources_ifdef(CONFIG_APP_BLE app PRIVATE src/ble.c)
target_sources_ifdef(CONFIG_APP_AUDIO app PRIVATE src/audio.c src/audio_dsp.c)
target_sources_ifdef(CONFIG_APP_IR app PRIVATE src/ir.c src/nec.c)
target_sources_ifdef(CONFIG_APP_WIFI app PRIVATE src/wifi.c src/wifi_glue.c)
