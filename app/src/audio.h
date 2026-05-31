/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef M5STICKS3_AUDIO_H
#define M5STICKS3_AUDIO_H

#include <stdbool.h>

/*
 * ES8311 audio (P5). The whole module is gated behind CONFIG_APP_AUDIO; when
 * the option is off the functions are not declared and the source is not
 * compiled, so the default build is unchanged.
 */
#ifdef CONFIG_APP_AUDIO

/*
 * Bring up the codec + I2S TX path: configure the ES8311 for 16 kHz / 16-bit
 * I2S playback at a safe low volume, configure I2S0 TX as master, and leave
 * the speaker amplifier OFF. Returns 0 on success, negative errno otherwise.
 */
int audio_init(void);

/* True once audio_init() has succeeded and a beep can be played. */
bool audio_ready(void);

/*
 * Play a short 440 Hz tone on the speaker. The amplifier is enabled only for
 * the duration of the tone (anti-pop) and disabled again afterwards.
 */
void audio_beep(void);

#endif /* CONFIG_APP_AUDIO */

#endif /* M5STICKS3_AUDIO_H */
