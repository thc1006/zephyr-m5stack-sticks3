/*
 * Copyright (c) 2026 Hsiu-Chi Tsai
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef M5STICKS3_AUDIO_H
#define M5STICKS3_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

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
 *
 * The PAGE_AUDIO entry now runs audio_loopback() instead; audio_beep() is kept
 * as the standalone, HW-006-verified playback primitive (and the eventual
 * codec-sample reference) and is exercised by tests / future callers.
 */
void audio_beep(void);

/*
 * Acoustic loopback self-test (issue #6): play the 440 Hz tone on the speaker
 * while capturing the on-board mic over I2S, printing per-block RMS/peak on
 * serial (the RMS rises during the beep vs the surrounding silence, so the mic
 * is verified to hear the speaker). Blocking, ~0.3 s; the amp is on only for the
 * beep phase.
 */
void audio_loopback(void);

/*
 * Peak per-block mic RMS (0..32767) from the last audio_loopback() run (0 before
 * the first run); audio_mic_bars() maps it to a 0..4 level for the PAGE_AUDIO
 * meter.
 */
uint16_t audio_mic_level(void);
uint8_t audio_mic_bars(void);

#endif /* CONFIG_APP_AUDIO */

#endif /* M5STICKS3_AUDIO_H */
