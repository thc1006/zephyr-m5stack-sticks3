/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_AUDIO_DSP_H
#define M5STICKS3_AUDIO_DSP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure capture-DSP helpers (no Zephyr headers, native_sim-testable). The ES8311
 * ADC is mono, but the ESP32-S3 I2S controller only supports 2 channels, so a
 * captured block arrives as interleaved stereo int16 with the mic on one slot.
 * These extract the mono channel and reduce a block to a level for the acoustic
 * loopback self-test (RMS spikes during the beep) and the PAGE_AUDIO meter.
 * Mirrors the nec.c / wifi.c pure-logic split.
 */

/* Floor of the integer square root of x. */
uint32_t audio_isqrt(uint32_t x);

/*
 * Root-mean-square of n mono int16 samples, returned 0..32768. A NULL pointer or
 * n == 0 returns 0. Uses a 64-bit sum of squares so a full block of INT16_MIN
 * does not overflow.
 */
uint16_t audio_rms_i16(const int16_t *samples, size_t n);

/*
 * Peak absolute amplitude of n mono int16 samples, 0..32767 (INT16_MIN clamps to
 * 32767). NULL or n == 0 returns 0.
 */
uint16_t audio_peak_i16(const int16_t *samples, size_t n);

/*
 * Extract one I2S slot from `frames` interleaved stereo int16 frames into out.
 * `slot` is the slot INDEX (0 = first, 1 = second), NOT a channel count: do not
 * pass the channel count (2). `interleaved` must hold at least 2*frames int16
 * samples and `out` at least `frames`. A NULL in/out, frames == 0, or slot > 1 is
 * a no-op (out is left unchanged) so a stray count argument fails loud (empty
 * capture) rather than silently reading the wrong slot.
 */
void audio_deinterleave(const int16_t *interleaved, size_t frames, uint8_t slot, int16_t *out);

/*
 * Map a level in [0, full] to a bar count in [0, nbars] (clamped). full == 0 or
 * nbars == 0 returns 0. Used for the PAGE_AUDIO input-level meter.
 */
uint8_t audio_level_bars(uint16_t level, uint16_t full, uint8_t nbars);

/*
 * Expand `frames` mono int16 samples to interleaved stereo (L = R = mono[i]) in
 * `out`, the inverse of audio_deinterleave(). The ES8311 / ESP32-S3 I2S TX path
 * needs 2-channel frames, so a recorded mono clip is duplicated to both slots
 * for playback (issue #14). `out` must hold at least 2*frames int16 samples. A
 * NULL in/out or frames == 0 is a no-op. Use separate buffers (out != mono).
 */
void audio_interleave_mono(const int16_t *mono, size_t frames, int16_t *out);

/*
 * Apply a fixed-point gain to n mono int16 samples with saturation, into `out`.
 * `gain_q8` is Q8 fixed point: 256 = unity (1.0x), 512 = 2.0x, 384 = 1.5x. Each
 * result is truncated toward zero and clamped to [-32768, 32767], so a boosted
 * loud passage clips cleanly instead of wrapping. Used to make a quiet mic
 * recording audible on playback (issue #14, requirement QR-1). A NULL in/out or
 * n == 0 is a no-op. In-place safe (out may equal in).
 */
void audio_gain_clip_i16(const int16_t *in, size_t n, uint16_t gain_q8, int16_t *out);

#endif /* M5STICKS3_AUDIO_DSP_H */
