# Copyright (c) 2026 Hsiu-Chi Tsai
# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_ES8311 es8311.c)
zephyr_library_sources_ifdef(CONFIG_EMUL_ES8311 emul_es8311.c)
