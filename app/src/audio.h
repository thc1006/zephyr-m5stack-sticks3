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
 * the duration of the tone (anti-pop) and disabled again afterwards. The
 * PAGE_AUDIO entry plays one beep as a speaker self-check; HW-006-verified.
 */
void audio_beep(void);

/*
 * Acoustic loopback self-test (issue #6, bring-up primitive; not auto-run by the
 * UI): play the 440 Hz tone while capturing the on-board mic over I2S full-duplex,
 * printing per-block RMS/peak on serial. NOTE: at the codec's low playback volume
 * the tiny on-board speaker is too weak to couple acoustically to the adjacent
 * mic, so the beep phase reads ~0 - this is the weak-stimulus confound that
 * HW-016d resolved, NOT a mic fault (a loud external sound drives the same path
 * to full scale; see the live mic capture thread, audio_capture_set()). Blocking,
 * ~0.3 s; amp on only for the beep phase.
 */
void audio_loopback(void);

/*
 * Enable/disable the live mic-capture thread (issue #6). Set true while the
 * PAGE_AUDIO page is up: a dedicated thread then runs ONE continuous full-duplex
 * capture session and updates the level reported by audio_mic_level()/_bars()
 * ~8x/sec, so the meter tracks sound live. Gated to the page (one start on enter,
 * one stop on leave) so the I2S/codec only run while in use. The UI thread itself
 * never touches I2S -- it only reads the level -- which is what keeps the SPI
 * display stable (a per-render capture in the UI thread corrupted it; HW-016e).
 */
void audio_capture_set(bool on);

/*
 * The latest mic RMS (0..32767) from the capture thread (0 when the page is not
 * up / capture is off). audio_mic_bars() maps a GIVEN level to a 0..4 bar -- pass
 * one audio_mic_level() read to both so the bar and the printed RMS always agree
 * (the capture thread updates the level asynchronously).
 */
uint16_t audio_mic_level(void);
uint8_t audio_mic_bars(uint16_t level);

#endif /* CONFIG_APP_AUDIO */

#endif /* M5STICKS3_AUDIO_H */
