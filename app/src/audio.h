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

/*
 * Record -> playback demo (issue #14). A fixed-length mono clip is recorded
 * from the mic into RAM, then played back through the speaker, proving the full
 * audio-DATA round trip (mic -> ADC -> I2S RX -> RAM -> I2S TX -> DAC -> amp ->
 * speaker), beyond the level meter (#6) and the synthesised tone (#3). The
 * record/playback runs on the audio capture thread; the UI thread only issues
 * the requests and reads the state below, and NEVER touches I2S (HW-016e).
 */
enum audio_rec_state {
	AUDIO_REC_IDLE = 0,  /* nothing recorded yet (initial / empty) */
	AUDIO_REC_RECORDING, /* capturing the clip ("speak now") */
	AUDIO_REC_REVIEW,    /* a clip is held -- ready to play / re-record */
	AUDIO_REC_PLAYING,   /* playing the held clip back */
};

/*
 * Ask the audio thread to (re)record a clip of CONFIG_APP_AUDIO_REC_SECONDS.
 * Picked up when the thread is idle (not already recording/playing); a no-op
 * until audio_init() has succeeded.
 */
void audio_record_request(void);

/* Ask the audio thread to play the held clip back. No-op if nothing is held. */
void audio_play_request(void);

/* Current record/playback state, for the UI. */
enum audio_rec_state audio_rec_get_state(void);

/* Peak capture RMS (0..32768) of the last recording, for on-screen feedback. */
uint16_t audio_rec_peak(void);

/* Length of the held clip in milliseconds (0 if nothing is recorded). */
uint32_t audio_rec_len_ms(void);

#endif /* CONFIG_APP_AUDIO */

#endif /* M5STICKS3_AUDIO_H */
