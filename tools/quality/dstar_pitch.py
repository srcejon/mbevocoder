#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Experimental D-STAR pitch correction for the offline quality harness.

This is the continuity-aware estimator evaluated in docs/encoder-quality.md.
It uses the existing encoder's pitch and voicing as a conservative prior. The
caller supplies a 513-sample, DC-blocked, low-pass-filtered window centred on
the existing amplitude-analysis frame. `lowpass_for_pitch` supplies the tested
filter. This module does not replace the public library's streaming encoder.
"""
import numpy as np
from scipy.signal import fftconvolve, firwin


def lowpass_for_pitch(dc_blocked_pcm):
    """Return a centred 129-tap, 1200 Hz Hamming-windowed low-pass result.

    The offline harness evaluates old samples from the encoder's existing
    history. Its 64-sample filter lookahead fits inside that history; this
    function alone is not a zero-latency streaming filter.
    """
    return fftconvolve(dc_blocked_pcm, firwin(129, 1200, fs=8000), "same")


class DstarPitchTracker:
    """Select a transmitted pitch only when periodicity evidence is stronger.

    `angular_frequencies` contains the decoder's 120 legal speech fundamentals
    in radians/sample, in b0 order. A fresh instance is required per stream.
    The existing voicing decision remains with the caller.
    """

    def __init__(self, angular_frequencies):
        self.frequencies = np.asarray(angular_frequencies, dtype=float).copy()
        if (self.frequencies.shape != (120,)
                or not np.all(np.isfinite(self.frequencies))
                or np.any(self.frequencies <= 0)):
            raise ValueError("Expected 120 finite positive D-STAR fundamentals")
        periods = 2 * np.pi / self.frequencies
        time = np.arange(-160, 161)
        a = time[None, :] - periods[:, None] / 2 + 256
        b = time[None, :] + periods[:, None] / 2 + 256
        self.ai = np.floor(a).astype(int)
        self.bi = np.floor(b).astype(int)
        if self.ai.min() < 0 or self.bi.max() + 1 >= 513:
            raise ValueError("Pitch period falls outside the analysis window")
        self.af = a - self.ai
        self.bf = b - self.bi
        self.weight = np.hanning(321)
        self.previous = None

    def reset(self):
        self.previous = None

    def periodicity(self, window):
        """Symmetric, energy-normalised correlation at every coded period."""
        window = np.asarray(window, dtype=float)
        if window.shape != (513,) or not np.all(np.isfinite(window)):
            raise ValueError("Expected 513 finite samples")
        a = window[self.ai] * (1 - self.af) + window[self.ai + 1] * self.af
        b = window[self.bi] * (1 - self.bf) + window[self.bi + 1] * self.bf
        energy = np.sum(self.weight * (a * a + b * b), axis=1)
        return 2 * np.sum(self.weight * a * b, axis=1) / np.maximum(energy, 1e-9)

    def select(self, window, original_b0, voiced):
        """Return (selected_b0, correlations); retain uncertain original pitches.

        Pitch distance is measured in octaves. The prior penalties are 0.02
        from the original pitch and 0.03 from the previous selected pitch,
        with the latter capped at one octave. A change requires correlation
        above 0.7 and an improvement greater than 0.035. These constants were
        frozen before the new-speaker validation, not fitted to its scores.
        """
        if not isinstance(original_b0, (int, np.integer)) or not 0 <= original_b0 < 120:
            raise ValueError("original_b0 must be a speech index from 0 to 119")
        previous = original_b0 if self.previous is None else self.previous
        correlation = self.periodicity(window)
        distance = np.abs(np.log2(self.frequencies / self.frequencies[original_b0]))
        continuity = np.abs(np.log2(self.frequencies / self.frequencies[previous]))
        selected = int(np.argmax(correlation - .02 * distance - .03 * np.minimum(continuity, 1)))
        if not (voiced and correlation[selected] > .7
                and correlation[selected] - correlation[original_b0] > .035):
            selected = int(original_b0)
        self.previous = selected
        return selected, correlation
