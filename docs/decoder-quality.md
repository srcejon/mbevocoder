# Decoder quality

The decoders are mbelib-neo 2.1.0's, with the corrections below, all under
`MBEVOCODER_CHIP_MEASURED` (on). The parts mbelib-neo 2.6.0 re-implemented independently from the
standard to settle their provenance (adaptive smoothing, unvoiced synthesis and noise generators,
the reset and erasure models, the tone oscillator, the limiter and 16-bit conversion) are its
2.6.2 versions (`NOTICE.md`); with them the output is the same (0.2 % of the samples
of 615 test streams differ by one 16-bit step, the band-power sums' rounding; PESQ within 0.004 on
every DVSI vector) and so is the speed. With it off (the `neo-compat` preset, plus `MBEVOCODER_FLOAT_CLIP=ON`) the
library gives mbelib-neo 2.1's output to within float rounding (at most 0.0015 on its 1/7 float
scale, a hundredth of a 16-bit step), which `test_decoder_parity` checks.

The reference is the AMBE-3000R, whose decoder is DVSI's reference decoder: on DVSI's published
test vectors its output is sample-identical to DVSI's ("DVSI's test vectors"). IMBE has no
hardware here; JMBE and DVSI's P25 vectors stand in.

Measures: PESQ (ITU-T P.862 narrowband) and ESTOI against the original speech
(`tools/quality/perceptual.py`), and log-spectral distance (LSD, 100-3800 Hz, after alignment and
gain compensation) against the AMBE-3000R's audio (`tools/quality/compare.py`). LSD separates broken
from working decoders but saturates quickly: any good decode is about 8 dB from the original, two
good decoders 7-7.5 dB from each other, and white noise at 20 dB SNR scores like a codec. Baselines
in `corpus/baselines/` (`decoder_vs_chip.json`, `decoder_lineage*.json`, `decoder_errors.json`,
`dvsi_decoders.json`).

## Status

The AMBE-3000R's frames of the 70-file corpus, through it and through ours:

| | AMBE+2 PESQ / ESTOI | LSD vs the AMBE-3000R | D-STAR PESQ / ESTOI | LSD vs the AMBE-3000R |
|---|---|---|---|---|
| the AMBE-3000R's decoder | 2.943 / 0.810 | | 2.995 / 0.805 | |
| mbevocoder | 2.972 / 0.803 | 6.97 dB | 2.943 / 0.800 | 7.03 dB |
| mbelib-neo 2.6.5 | 2.866 / 0.803 | 6.87 dB | 2.920 / 0.798 | 6.97 dB |
| mbelib 1.3 | 2.572 / 0.732 | 8.16 dB | 1.768 / 0.561 | 11.41 dB |

IMBE, our encoder's frames: mbevocoder 3.071, JMBE 3.021. Output level: within 0.2 dB of the AMBE-3000R's on every
rate ("Output level" below).

Under channel errors, the same frames damaged identically for both decoders (`hil_errors.py`, 70
files, PESQ):

| condition | AMBE+2 AMBE-3000R / ours | D-STAR AMBE-3000R / ours |
|---|---|---|
| BER 0.5 / 1 / 2 % | 2.92 / 2.94, 2.88 / 2.90, 2.82 / 2.81 | 2.95 / 2.89, 2.90 / 2.86, 2.79 / 2.80 |
| BER 3 / 5 % | 2.68 / 2.71, 2.39 / 2.42 | 2.67 / 2.72, 2.29 / 2.38 |
| 2-8 bit bursts in 2 / 5 % of frames | 2.92 / 2.96, 2.88 / 2.90 | 2.97 / 2.92, 2.94 / 2.89 |
| random frames 2 / 5 / 10 % | 2.69 / 2.74, 2.36 / 2.41, 1.99 / 2.05 | 2.58 / 2.62, 2.11 / 2.14, 1.74 / 1.80 |

AMBE+2 is level with the AMBE-3000R (within 0.01) or above it everywhere (up to +0.06 on random
frames). D-STAR is level with it or above at 2-5 % BER (+0.09 at 5 %) and on random frames (+0.03-0.06),
and 0.04-0.06 below at 0.5-1 % BER and on bursts. On random frames D-STAR plays what the AMBE-3000R
plays: every frame whose C0 word decodes ("D-STAR channel errors"), where repeating every frame with
more than three corrected errors, as before, scored 0.18-0.20 higher at 5-10 % (2.31 / 2.00) and
departed from the AMBE-3000R on frames with real errors. IMBE, our frames: BER 5 % 2.62 (JMBE 2.60), 5 %
random frames 2.79 (2.75).

## DVSI's test vectors

DVSI publishes the AMBE-3000 test vectors (https://www.dvsinc.com/get-usb/tv-rc.zip): input
speech, DVSI's encoded bits per mode, DVSI's decode of them, and damaged bit streams (1, 2, 5 and
10 % errors, hard and 4-bit soft) with their decodes. They are fetched into the git-ignored build
tree (mbelib-neo's `tools/quality/fetch_dvsi_vectors.py`) and never committed or redistributed;
only measurements are recorded. `tools/quality/dvsi_decoders.py` scores every decoder on them. The
files read as-is: D-STAR 9 bytes LSB first, rate 33 9 bytes DMR layout MSB first, P25 18 bytes
Phase 1 layout MSB first; soft files carry a 4-bit value per bit (0 strong 0 .. 15 strong 1), two
per byte, high nibble first.

**The AMBE-3000R is DVSI's decoder**: its decode of every D-STAR and rate-33 file, clean and damaged
(31 per mode), is sample-identical to DVSI's. Its encoder (firmware
V120.E100.XXXX.C106.G514.R009.B0010411.C0020208) is not DVSI's vector encoder: it reproduces 2-7 % of
DVSI's frames whatever its options or the input offset.

The 27 speech vectors, PESQ / ESTOI against the input, paired difference against DVSI's decode ±
SE, LSD against DVSI's decode; "final 3" are the vectors mbelib-neo holds out from its tuning:

| Mode | Decoder | All 27 | vs DVSI | LSD | Final 3 |
|---|---|---|---|---|---|
| D-STAR | DVSI (= AMBE-3000R) | 2.931 / 0.766 | | | 2.925 / 0.834 |
| | mbelib 1.x | 1.810 / 0.565 | -1.120 ± 0.064 | 11.23 | 1.660 / 0.567 |
| | mbelib-neo 2.6.5 | 2.851 / 0.764 | -0.079 ± 0.008 | 7.28 | 2.805 / 0.827 |
| | **mbevocoder** | **2.879 / 0.764** | **-0.051 ± 0.008** | **7.33** | **2.841 / 0.828** |
| AMBE+2 | DVSI (= AMBE-3000R) | 2.981 / 0.769 | | | 2.986 / 0.832 |
| | mbelib 1.x | 2.629 / 0.706 | -0.352 ± 0.026 | 8.47 | 2.429 / 0.756 |
| | mbelib-neo 2.6.5 | 2.857 / 0.767 | -0.123 ± 0.009 | 7.25 | 2.805 / 0.821 |
| | **mbevocoder** | **2.891 / 0.762** | **-0.090 ± 0.011** | **7.25** | **2.813 / 0.818** |
| IMBE | DVSI | 3.026 / 0.792 | | | 3.035 / 0.851 |
| | mbelib 1.x | 2.756 / 0.749 | -0.270 ± 0.024 | 8.24 | 2.592 / 0.802 |
| | mbelib-neo 2.6.5 | 2.910 / 0.786 | -0.116 ± 0.009 | 7.46 | 2.923 / 0.846 |
| | **mbevocoder** | **2.936 / 0.787** | **-0.089 ± 0.009** | **7.46** | **2.930 / 0.846** |

mbevocoder's PESQ is the closest to DVSI's in every mode and partition; by LSD from DVSI's decode
mbelib-neo 2.6.5 is level on AMBE+2 and IMBE and 0.05 dB closer on D-STAR (its #167 amplitude law,
"Tried and not adopted" below). The remaining gap is the first item under "Future work". (mbelib 1.x and mbelib-neo are scored on their float output
unrounded; mbelib 1.x puts out about 25 dB less than DVSI, and rounded to 16 bits would lose more.)

The damaged `dam` vectors (one 25 s recording; PESQ / ESTOI; hard bits, then soft):

| Mode | BER | DVSI | mbelib 1.x | neo 2.6.5 | mbevocoder | DVSI soft | neo 2.6.5 soft | mbevocoder soft |
|---|---|---|---|---|---|---|---|---|
| D-STAR | 1 % | 3.38 / 0.85 | 2.05 / 0.58 | 3.29 / 0.85 | 3.33 / 0.85 | 3.38 / 0.85 | 3.28 / 0.85 | 3.30 / 0.85 |
| | 2 % | 3.37 / 0.84 | 2.03 / 0.57 | 3.26 / 0.83 | 3.31 / 0.84 | 3.37 / 0.84 | 3.23 / 0.83 | 3.28 / 0.84 |
| | 5 % | 2.83 / 0.77 | 1.87 / 0.61 | 2.81 / 0.77 | 2.80 / 0.77 | 3.06 / 0.80 | 2.99 / 0.80 | 3.07 / 0.80 |
| | 10 % | 1.20 / 0.05 | 1.36 / 0.31 | 1.20 / 0.06 | 1.19 / 0.06 | 1.29 / 0.28 | 1.86 / 0.62 | 1.93 / 0.63 |
| AMBE+2 | 1 % | 3.36 / 0.85 | 3.01 / 0.78 | 3.29 / 0.85 | 3.30 / 0.84 | 3.36 / 0.85 | 3.26 / 0.85 | 3.32 / 0.84 |
| | 2 % | 3.30 / 0.84 | 2.95 / 0.77 | 3.23 / 0.84 | 3.26 / 0.83 | 3.31 / 0.84 | 3.22 / 0.84 | 3.23 / 0.83 |
| | 5 % | 3.05 / 0.79 | 2.63 / 0.73 | 2.96 / 0.80 | 2.97 / 0.80 | 3.13 / 0.81 | 3.06 / 0.82 | 3.08 / 0.81 |
| | 10 % | 1.23 / 0.31 | 1.43 / 0.45 | 1.29 / 0.27 | 1.26 / 0.29 | 1.88 / 0.61 | 1.88 / 0.63 | 2.00 / 0.63 |
| IMBE | 1 % | 2.70 / 0.81 | 3.20 / 0.84 | 3.34 / 0.88 | 3.36 / 0.88 | 3.07 / 0.84 | 3.35 / 0.88 | 3.37 / 0.88 |
| | 2 % | 2.13 / 0.70 | 3.17 / 0.82 | 3.35 / 0.88 | 3.35 / 0.88 | 2.14 / 0.73 | 3.35 / 0.88 | 3.35 / 0.88 |
| | 5 % | 1.49 / 0.51 | 2.39 / 0.72 | 2.83 / 0.81 | 3.06 / 0.83 | 1.35 / 0.49 | 3.22 / 0.86 | 3.23 / 0.86 |
| | 10 % | 1.18 / 0.28 | 1.17 / 0.21 | 1.33 / 0.49 | 1.71 / 0.54 | 1.17 / 0.28 | 1.96 / 0.65 | 1.72 / 0.58 |

(Clean `dam`: DVSI 3.40 / 3.39 / 3.43.) On the AMBE rates mbevocoder tracks DVSI within 0.03-0.09 to
5 %, hard and soft (D-STAR's 5 % soft 0.01 above it). At 10 % (hard) DVSI mutes most frames, and so do we now: D-STAR 88 % of the
active frames silent on both, the same frames (100 % agreement), AMBE+2 42 % against our 48 % (92 %
agreement), and both score what DVSI's decode scores. Soft decisions do not mute there, on DVSI's
decoder or ours. Whether muting or playing damaged frames is better is a listening question. Our IMBE decoder
mutes at 10 % as TIA-102.BABA says (the error rate above 0.0875, counted through repeats too, "After
a mute"), which PESQ scores lower than playing the frames (soft 1.70 against 2.27). DVSI's P25 decoder
degrades from 1 % on without muting or slipping; it is not a reference under errors. Every decoder
reads DVSI's P25 frames as they come, which confirms the IMBE bit order ([bit-order.md](bit-order.md)).

## What differs from mbelib-neo

### D-STAR tables

mbelib's 3600x2400 decoder was reverse engineered without a specification. Probed on the AMBE-3000R
with synthesised frames (`tools/quality/probe_*.py`, `dstar_codebook.py`, `dstar_lsweep.py`), AMBE+2
run through every probe as a control:

| Parameter | The AMBE-3000R | mbelib |
|---|---|---|
| fundamental | `log2(f0) = -4.258420 - 0.0217690 b0` (cycles per sample), 0.7 cents residual | 18-74 cents low |
| harmonic count | `L = max(9, floor(0.9254 pi / w0))` with that fundamental, exact on all 120 indices | 1-4 too high on 101 indices |
| shape prediction coefficient | 0.79 (step response 0.77-0.80; the decoder fit to its audio 0.79) | 0.65, the AMBE+2 value |
| gain | no frame-to-frame memory: `gamma = 2 dg[b2]`, a one-frame excursion rendered in full and gone the next frame (AMBE+2 keeps the 0.5 recursion) | `dg[b2] + 0.5 gamma_prev` |
| codebooks (PRBA24, PRBA58, HOC) | mbelib's: correlation 0.993-0.998, at the method's noise floor | |
| voicing groups, gain table, unvoiced scale 0.2046 / sqrt(w0), spectral enhancement | as mbelib (the voicing groups' extent per band: "Mixed excitation") | |

The decoder and the encoder's closed loop use the AMBE-3000R's values (`AmbePlusLtableMeasured`,
`AMBE2400_PRED`, `AMBE2400_GAIN_PRED` in `ambe_pred.h`). D-STAR's LSD against the AMBE-3000R's audio
went from 10.7 to 7.1 dB, the AMBE+2 decoder's figure; the coefficient and the gain were the largest
steps. Our D-STAR decoder's output is on the same scale as AMBE+2's.

### Mixed excitation

mbelib's voicing tables are binary approximations of what the AMBE-3000R renders.
`tools/quality/probe_vuv_mix.py` measured every voicing index at six fundamentals (harmonic-to-noise
ratio per band, solved for the voiced energy fraction, plus a level offset), generated into
`ambe3600x2450_vuvmix.h`. AMBE+2 rows 1, 5, 8, 11, 12, 13 and 15 are partially voiced. The decoder
carries a voiced fraction per harmonic (`Vmix`, `unvcScale`, `vmixValid` in `mbe_parms`; `Vl` is the
fraction rounded) and renders a voiced component sqrt(v) M and a noise component sqrt(1 - v) M. The
encoder's bits are unaffected. The effect on speech is small; kept because it is the measured
behaviour.

D-STAR's voicing is a rule instead (`AmbePlusHarmonicVoiced`, mbelib-neo 2.6.4 #173): b1's four bits
voice the 1 kHz bands from the lowest, and each voiced band reaches 250 Hz into the band below it, a
harmonic being voiced when its band's bit or that of the band 250 Hz above is set. On the AMBE-3000R
(every b1 at six pitches, `probe_dstar_neo265.py`'s b1 sweep) the rule matches the voicing of 96 % of
the harmonics, against 92 % for the table measured on a 500 Hz grid, which cannot express the 250 Hz
reach; 250 Hz is the best overlap there too. D-STAR corpus PESQ +0.009 ± 0.007, LSD against the
AMBE-3000R -0.05; on DVSI's vectors LSD -0.009 ± 0.004. The D-STAR encoder chooses b1 by the same rule.

### Unvoiced rows 18-31: the band grid

AMBE+2 voicing rows 18-31 (all bands unvoiced) are rendered on a band grid whose L bands always span
3.58 kHz (w0' = 2.81 / L), the fundamental index setting only the harmonic count; row 17 is row 16.
Their level offsets and voiced fractions are zero. Rows 20 and 28 (onset bursts) and 24 and 25
(pitch-gated noise) are, on the AMBE-3000R, a noise sequence restarted every frame, which buzzes at
the frame rate; they get the grid rule and nothing else. The AMBE-3000R's encoder uses rows 17-31 on
about 6 % of frames. PESQ +0.013 on its frames; what is left on those frames is the distance other
unvoiced frames have.

### Amplitude interpolation

Every harmonic voiced in both frames whose pitch moves by less than half is synthesised
phase-continuously with a geometric amplitude ramp (a straight line in dB) across the frame, where
mbelib ramped linearly and kept phase continuity for the seven lowest harmonics only; the unvoiced
synthesis gets a geometric level ramp too. +0.10 PESQ (AMBE+2) / +0.08 (D-STAR) on the AMBE-3000R's
frames. The voiced ramp and quadratic phase are computed by recurrence (review P1). Linear, smoothstep
and windowed variants, sub-frame unvoiced windows, other in-frame pitch trajectories and shifted
glides all score lower or the same. Voicing transitions (`probe_vuv_transition.py`) ramp within the
frame as the AMBE-3000R's do.

### Regenerated phases

The phase-continuity correction bends each harmonic's frequency by the frame-to-frame jump of its
regenerated phase (up to 25 Hz for a jump of pi). The regenerated phases are smoothed across frames
(0.8 of the previous frame's, `MBE_EXP_PHASE_SMOOTH`): +0.02 PESQ. The AMBE-3000R's harmonic phases
follow no rule tried (`probe_phase.py`: mbelib's kernel at any scale, minimum phase, zero phase,
quadratics), so the smoothed model stays.

### Low frequencies

The AMBE-3000R's output has no energy below about 50 Hz and rolls off under 100 Hz. A second-order
Butterworth high-pass at 80 Hz on the synthesis output removes the noise generator's DC leakage and
the rumble of very low fundamentals that read as buzz in pauses (pause frames 4.7 -> 1.8 dB above
the AMBE-3000R's, periodic pause frames 18 % -> 1 %). PESQ unchanged. A higher corner, as
mbelib-neo 2.3's roll-off (a 100 Hz high-pass equivalent), moves our output further from the
AMBE-3000R's and DVSI's below 250 Hz on every rate (+0.04 to +0.18 dB at 100 Hz) for at most +0.011
PESQ.

### High-band shelf

With the same bits our synthesis rendered 2.7-3.9 kHz 1.4-2.5 dB below DVSI's decoder (the
AMBE-3000R's) relative to 200-1000 Hz, on all three rates (the long-term spectrum of DVSI's 27 speech
vectors; mbelib-neo 2.5 found the same gap). The spectral enhancement now ends with a shelf, +2 dB
from 3.1 to 3.7 kHz, ramped in from 2.7 kHz and out by 3.9 kHz, after its energy renormalisation.
The rms gap over 1-3.8 kHz fell from 1.17 / 1.51 / 1.28 dB (D-STAR / AMBE+2 / IMBE) to 0.61 / 0.49
/ 0.27 dB; PESQ against the input +0.002 / -0.001 / +0.001, against DVSI's decode -0.003 / -0.001 /
-0.003 (all within 1.5 SE). mbelib-neo's shelf (+2.5 dB, 2.5-3.8 kHz) overshoots ours by 0.5-1 dB
in 2.5-3.3 kHz. What is left is our output 0.3-0.9 dB above DVSI's at 1-2.5 kHz.

### AMBE+2: the order of b3 and b4's low bits

The AMBE-3000R takes b4's three low bits from `ambe_d[40..42]` and b3's from `ambe_d[43]`; mbelib and
JMBE take b3's from `ambe_d[40]` and b4's from `ambe_d[41..43]` ([bit-order.md](bit-order.md)).
Neighbouring codebook rows are similar, which hid it. Decoding in the AMBE-3000R's order: PESQ
2.851 -> 2.971, ESTOI 0.770 -> 0.802 on its frames; harmonic levels on steady voiced frames within
1.38 dB rms of its (2.24 before). The codebooks themselves are mbelib's, verified at nine harmonic
counts (`dstar_codebook.py --rate ambe2450`).

### AMBE+2 pitch law

The AMBE-3000R's fundamental is log2 f0 = -4.322605 - 0.0220427 b0 (cycles per sample), the law
mbelib-neo 2.5 found in DVSI's output: its harmonics in the L-sweep recording
(`dstar_lsweep.py ambe2450`, steady all-voiced frames at every code) fit it with an rms residual of
1.1e-4 in log2 f0, mbelib's table (TIA-102.BABA-1 Annex A) 9.5e-4, the table being up to 0.23 %
(4 cents) sharp at b0 27-30, 72-76 and 117-119. The harmonic count floor(0.9254 * 0.5 / f0) on the
law differs from the table at b0 17 only: 12, as the AMBE-3000R plays it. The decoder and the
encoder use the law (`AmbeW0Measured`, `AmbeLtableMeasured`).

### IMBE: the previous frame's amplitude below harmonic 1

The amplitude prediction interpolates the previous frame's log2 amplitudes at l L(-1) / L and reads
the entry below harmonic 1 when that falls below 1: TIA-102.BABA and JMBE set it to zero, mbelib
copied harmonic 1. DVSI copies it, as mbelib-neo 2.3 found: decoding DVSI's 27 P25 vectors with the
copy brings our output 0.43 ± 0.06 dB closer to DVSI's decode below 250 Hz (ESTOI +0.004 ± 0.001,
PESQ -0.006 ± 0.004), and OpenBLIP25's frames (a port of DVSI's software vocoder) score ESTOI
+0.0075 ± 0.0006 through the copy. The decoder and our IMBE encoder both use the copy. On the corpus
(our frames, our decoder) that is level with the zero on both sides (PESQ -0.004 ± 0.006, ESTOI
+0.0013 ± 0.0003); through a copying decoder, as DVSI's, our frames gain +0.051 ± 0.007 PESQ over
the zero-fitted encoder, and through JMBE, which uses the zero, +0.021 ± 0.006 PESQ and -0.003 ESTOI.
The AMBE
decoders copy too, which is what the AMBE-3000R does. JMBE's IMBE phase model in place of the
regenerated phases is 0.02 worse.

### Silence frames

The AMBE+2 silence frame (fundamental index 124/125, a DTX encoder's pause frame) is rendered with
w0 = 2 pi / 32 at a further 0.2046 down, from the last voice frame's prediction state without
advancing it, as the AMBE-3000R does (JMBE's model was 27 dB loud with the wrong spectrum);
`mbe_synthesizeComfortNoiseFramef` continues it over the frames a DTX sender withholds
([frontend.md](frontend.md)). D-STAR silence frames (index 120) were already right.

### Output level

Decoded speech plays at the AMBE-3000R's level, which is DVSI's decoder's: the synthesis output is
scaled by 0.1529 (-16.3 dB), so float x 7, the 16-bit conversion, lands on it. Measured, ours minus
the reference's active level: the AMBE-3000R's frames of the corpus -0.03 dB (AMBE+2) and -0.19 dB
(D-STAR), DVSI's vectors +0.06 / +0.09 / +0.17 dB (D-STAR / AMBE+2 / IMBE). Tone frames play at the
AMBE-3000R's level too ([tone-frames.md](tone-frames.md)), so the tone-to-speech balance is a
radio's. mbelib's synthesis, and mbelib-neo 2.1's, play speech 16.3 dB louder, where the 16-bit
conversion clips loud speech; mbelib-neo 2.3+ decodes at DVSI's level (within 0.5 dB). With
`MBEVOCODER_CHIP_MEASURED` off the level is mbelib-neo 2.1's.

### Float output and 16-bit output: rounding

The float output is not clipped (mbelib 1.x behaviour); mbelib-neo's clip at 95 % of full scale on
its 1/7 scale cut 2-3 % of the samples of normal-level speech at mbelib's level (0.4 MOS on the files
measured) and is available as `MBEVOCODER_FLOAT_CLIP=ON`. `mbe_floattoshort` rounds to the nearest step
(half away from zero, scalar, SSE2 and NEON alike); mbelib truncated towards zero, a dead zone that
distorts quiet output (DVSI's -62 / -68 dBFS AMBE+2 vectors 2.91 / 2.43 -> 3.04 / 2.73, the float
decode's score; ordinary levels unchanged).

### Bit errors

Lost frames (`mbe_processLostFramef()`, for slots the framing layer knows were not received): the
previous model repeats three times at its level, the fourth fades to silence within the frame,
silence follows, and the first decoded frame after plays at its proper level, the prediction state
kept: the AMBE-3000R's `LOST_FRAME` behaviour (`ambe3000 --lost`, `mbe-cli decode --lost`).
Feeding the last frame's bits again instead would rerun the gain prediction and climb 2-3 dB per
frame. FEC-triggered repeats mute the same way.

Golay: a C0 word with four (or six) errors fails the extended code's parity after correction, and
the AMBE-3000R repeats the frame; mbelib's count of corrected data bits never reached the repeat
threshold, so such words decoded as garbage (5-14 dB holes). A C0 word the extended code detects as
uncorrectable reports `c0_errors` 4 and is repeated. Five or seven errors miscorrect on both
decoders alike.

### IMBE: errors in the Golay parity bits

TIA-102.BABA repeats a frame when E0 >= 2 and ET >= 10 + 40 eps_R and mutes when eps_R > 0.0875,
counting every corrected bit of each Golay(23,12) word, parity included, as JMBE does. mbelib
counted only corrected data bits, so random frames stayed under the threshold and played as bursts
up to 17 dB loud. The IMBE 7200 frame decoders count the full weight (the decoded word re-encoded
against the received bits, `src/bits/imbe_frame.cpp`): 5 % random frames 2.16 -> 2.75, BER 5 %
2.23 -> 2.62 on our frames (JMBE 2.73 / 2.58). The public `mbe_golay2312` / `mbe_eccImbe7200x4400*`
keep mbelib's count; the 7100x4400 frame keeps it too (no reference).

### D-STAR channel errors

The AMBE-3000R plays every D-STAR frame whose C0 and C1 words decode, whatever the count of corrected
bits (3 + 3 included, `hil_errors.py --golay`); only a C0 word with four errors, which the extended
Golay code detects but cannot correct, plays as a lost frame, and the frame decoded from it still
becomes the prediction history: the frames after it depend on its bits (mbelib-neo 2.6.3, #159, found
the same on a DVstick 30). Ours does the same (`historyL` / `historyLog2Ml` in `mbe_parms`, the context
flag `MBE_PROCESS_FLAG_C0_UNCORRECTABLE`); it used to repeat frames with more than 3 corrected bits.
The frame decoders' D-STAR counts are full-weight, as AMBE+2's (C1's parity bit, `ambe_d[24]`,
included), as mbelib-neo 2.6.3+ reports them.

### D-STAR special frames

As the AMBE-3000R handles them (`ambe2400_measured_tone_action`): a b0 126 tone frame plays
whenever C0 decodes, whatever the corrected count and at any volume, 0 included; an uncorrectable
C0 repeats the previous frame with no reset. b0 125, every b0 127 frame (a playable tone, a dual
tone, the volume-0 dual tone 128 that encoders have sent for silence), a b0 126 frame whose index it
cannot render (outside 5..122 and the dual tones 128..147) and DVSI's all-ones DTX frame are lost
frames: the AMBE-3000R's output for runs of 1, 3 and 5 of them equals, sample for sample, its output
with the same frames flagged `LOST_FRAME`, through the run and the voice after it
(`tools/quality/probe_dstar_frames.py`); ours too, flagged `MBE_PROCESS_FLAG_ERASURE` and `_REPEAT`
(and `_TONE` for b0 126/127), as mbelib-neo's main branch found. Ours had decoded b0 125 as voice and
played b0 127 tones. mbelib played comfort noise and reset the decoder for tone frames, leaving the
next voice frames 5-15 dB low.

### D-STAR tones

On the AMBE-3000R (`probe_dstar_neo265.py`; mbelib-neo 2.6.3-2.6.4 #152, #162 and #165 found the same
on a DVstick 30):

- The first tone frame after speech plays g(n) tone + (1 - g(n)) speech, g rising linearly to 1 at
  sample 80, the speech being the last frame repeated; the first voice frame after a tone is
  crossfaded from the tone the same way. No speech sounds under a tone after a muted frame.
- A tone whose volume changes from one frame to the next moves to the new level over the same 80
  samples; a new index starts at once.
- After a dual tone (DTMF, call progress) lost frames continue the tone, the one that reaches the
  repeat limit fading it out over its first 80 samples; after a single tone the first lost frame plays
  its tail fading out and the repeats after it are silent. The prediction history stays the voice
  from before the tone.
- The volume law (0.354 dB per step) runs on above full scale: a tone's peaks reach 16-bit full scale
  at about volume 236 and the AMBE-3000R clips above it (single tone at 255: -1.0 dB RMS). A dual tone
  stops rising at about volume 238 (-2.6 dB RMS).

Ours follows all of it, to within 0.5-1.4 dB of the AMBE-3000R's frame levels over each case (3-26 dB
before), and 0.03 dB on steady tones at every volume. The 16-bit conversion limits at full scale with
`MBEVOCODER_CHIP_MEASURED`, as the AMBE-3000R's output does; mbelib's limit is 95 %.

### D-STAR muting

The AMBE-3000R mutes D-STAR on a long-memory error rate: r = 0.99 r + 0.001064 e over each frame's
full-weight error count e (the received C0 and C1 bits, C1's parity bit included, that differ from
the corrected frame re-encoded), silent while r > 0.366, playing again as soon as it falls below. On
steady random bit errors (2-12 %) and on bursts of 8-12 % between clean stretches its silent frames
and the rule's agree on 99.3 % of the active frames (the corrected data-bit counts: 94.9 %). It
starts muting at about 8 % BER, 2-5 s into the errors depending on the rate, and plays again
0.2-0.6 s after they stop. TIA-102.BABA-1's recursion (0.95, 0.096) never mutes D-STAR at
these rates; mbelib-neo's main branch mutes on its data counts at 0.042 with the 0.95 memory (70 %
agreement). Ours follows the AMBE-3000R (99.3 %; it did not mute at all before). The frame paths
count full weight (soft frames weight each differing bit by its reliability); the data path, which
has no channel bits, uses the caller's corrected counts.

### AMBE+2 erasures and unusable tone frames

The AMBE-3000R plays an erasure (fundamental index 120-123, or 126-127 without the tone header), a
tone frame (u0 starting `111111`) whose four ID copies disagree and a tone frame whose index it
cannot play (200, and the silent tone 255 too) as it plays a lost frame: the last frame played
repeats three times, the fourth fades and silence follows, and the next voice frame plays at its
level (`tools/quality/probe_ambe2450_frames.py`, which edits such words into the AMBE-3000R's own
frames). The frame repeated is the last one played: after a tone, the tone ([tone-frames.md](tone-frames.md)).
JMBE's comfort noise and W120 reset, which mbelib-neo 2.1 had, left the frames after an erasure 20-30 dB
low while the prediction recovered; an unusable tone replayed the last voice frame without end. Ours now
repeats as the AMBE-3000R does, within about 2 dB of its level on every frame of the probe, flagged
`MBE_PROCESS_FLAG_ERASURE` and `_REPEAT` (and `_TONE` for a tone frame). Of damaged tone frames (1-10
random bit errors in each of 60) ours plays the tone on exactly the frames the AMBE-3000R plays, at
its level within about 1 dB. TIA-102.BABA-1 and mbelib-neo 2.3+ repeat erasures too, but play 255 as
silence, which the AMBE-3000R does not.

### AMBE+2 error counts and muting

TIA-102.BABA-1 5.5 counts every received bit that differs from the corrected codeword, parity bits
included (C0 with its extended parity bit, 4 when C0 is detected uncorrectable; C1 after
descrambling), and mutes while the error rate eps_R = 0.95 eps_R + 0.001064 e is above 0.096; a frame
repeats only when C0 is uncorrectable. mbelib counted the corrected data bits, under which the
recursion never reaches 0.096, and repeated on JMBE's rule (C0 >= 2 and total >= 6 too). With the
full-weight counts and the standard's threshold as written, ours mutes what the AMBE-3000R mutes: on
steady random bit errors (4-12 %, `probe_ambe2450_frames.py` part 4) the two agree on 94-100 % of the
active frames (silent at 10 / 12 %: the AMBE-3000R 25 / 92 %, ours 30 / 95 %; before, ours 0 / 0 %).
mbelib-neo 2.5+ counts the same way, and `mbe_eccAmbe3600x2450C0` / `_Data` return the full-weight
counts, as mbelib-neo 2.6's do. On DVSI's 10 % `dam` vector the hard decode now scores what DVSI's
own does (PESQ 1.24 against DVSI's 1.23; 1.74 before, when it played the damaged frames), its silent
frames agreeing with DVSI's on 92 % of the active frames; soft decisions are unchanged (2.00). The
chip's Golay sweep (`hil_errors.py --golay`) shows it decoding, not repeating, frames with two or
three C0 errors and an uncorrectable C1, which JMBE's rule repeated: at 5 % the hard decode loses
0.06 against the input for it (2.97, DVSI 3.05) while staying as close to DVSI's decode as before
(PESQ against it 3.87 either way, spectra slightly closer).

### D-STAR b0 124

b0 124 (the null frame among its codes) plays on the AMBE-3000R as TIA-102.BABA-1 4.1's silence model:
w0 = 2 pi / 32, 14 bands, all unvoiced, its amplitudes decoded and kept as the prediction history as a
voice frame's (mbelib-neo 2.6.3, #153). Decoded that way, one and three such frames after voice and ten
from a fresh start come 2.2 dB from the AMBE-3000R's frame levels against 2.5 dB decoded as voice at the
pitch law's 64 Hz. Encoders rarely send it (once in DVSI's vectors).

### Extreme model levels

A D-STAR frame held at the largest gain and b3 builds its amplitudes up to 2^45; the spectral
enhancement's float sums overflowed and the output dropped out (mbelib-neo 2.6.3, #160). The weights
are now computed from Rm1 / Rm0 and M_l / sqrt(Rm0), with models beyond 2^60 or 2^-60, or whose weights
float cannot resolve, enhanced in double precision, and the adaptive smoothing's amplitude sum is in
double. Decoded speech is unchanged. The AMBE-3000R comes back to the normal level at once after such a
run where ours carries it for several frames (the prediction decays at 0.79 a frame); no real stream
reaches those levels.

### Tried and not adopted from mbelib-neo 2.6.3-2.6.5

- Voiced amplitudes interpolated on a fourth-root law (#167): it moves the in-frame curve of a gain
  step towards the AMBE-3000R's (half the change at sample 72 on rises, 104 on falls, the AMBE-3000R
  40-56 and 120-136, ours 88), but against our geometric ramp PESQ falls on every set (D-STAR
  -0.006 to -0.010, 2.5-4 SE; AMBE+2 corpus -0.018) and LSD moves by under 0.01 dB.
- The first voice frame after the repeat limit started from the prediction history at a gain (#171,
  0.3 there): at gains 0 to 0.5 none comes closer to the AMBE-3000R than our start at the frame's own
  model (1.20-1.70 dB against 1.22).

### After a mute

The first frame decoded after a mute starts steady at its own model, the synthesis' old tails
cleared (it overlapped the full-level repeated model and came out 3-8 dB above the AMBE-3000R's; now
within about 1 dB). The AMBE reset and erasure models reset the mbevocoder fields (mixed excitation,
silence flag, output high-pass). A repeat takes the previous frame's model but keeps this frame's
error state (error rate and counts, muting threshold) and synthesis state (noise generator and
overlap, tone phase, output high-pass): copying the whole previous frame, as mbelib-neo 2.1 did,
held the error-rate recursion that IMBE's mute and repeat thresholds read and replayed the last
frame's noise (mbelib-neo 2.3 changed it the same way). Measured on the corpus' damaged frames it
moves nothing (every condition and rate within 1.5 standard errors of zero).

### Soft-decision C0

On the soft path, a C0 word whose hard decisions are a detected-uncorrectable pattern is trusted
when the bits the soft search overrules are weak: their reliabilities sum to at most
`MBE_SOFT_C0_TRUST` (2) times the word's mean (scale-free), and the count is capped at 3 so the
frame decodes. Over AWGN it accepts 94-97 % of such frames at 1-2 dB Eb/N0, 99.5-100 % right;
PESQ +0.04 to +0.10 at -1 to 1 dB on both rates. On DVSI's soft vectors our soft path lands within
0.06 of DVSI's.

### IMBE soft decisions: the error count

The IMBE 7200 soft path decodes each Golay and Hamming word by exhaustive soft search, which can
land further from the hard decisions than a hard decoder corrects (3 bits, 1 bit). A word that
overrules only weak bits (`MBE_SOFT_IMBE_TRUST` 2, the C0 rule) counts as a hard decoder's worst
correctable word; otherwise its whole distance counts. Counting the whole distance fed the
repeat and mute rules so much that soft decisions did worse than hard ones at 10 % BER. Over AWGN, our
frames, twelve recordings: +0.08 to +0.40 PESQ against the full count at 4-13 % BER, never worse
(`build/err_probe/imbeab.py`). Capping regardless of the bits overruled scores 0.01-0.03 higher on
AWGN, where no wrong bit is confident; the trust test is for fades and bursts.

## Future work

- **The gap to DVSI's decoder on DVSI's vectors** (0.051 D-STAR, 0.090 AMBE+2, 0.089 IMBE). For
  AMBE+2 it is the pitch values DVSI's encoder sends in voiced frames: in hybrid streams (the
  AMBE-3000R's encoder's frames with fields swapped in from DVSI's, `build/gap`) DVSI's b0 alone
  carries the whole gap (-0.079 ± 0.020; its own frames +0.013, b1, b2 or the shape fields
  +0.016 to +0.024), and it makes the AMBE-3000R's score rise and ours fall. The loss is spread over
  voiced frames and largest on fast pitch glides, where DVSI's track runs about a frame ahead of the
  AMBE-3000R encoder's. Ruled out: the fundamental and harmonic-count tables, the coded level, every
  synthesis knob (none gains more than 0.009, `build/abl`), the voicing rows and spectral balance,
  the in-frame glide (windowed synthesis, glide timing, phase settings), and the pitch's timing
  (held back, or a frame of look-ahead in `build/gap/lead`: our timing is already the best for
  DVSI's frames). What in the AMBE-3000R's synthesis suits those pitch values is not found.
- **The AMBE-3000R's level depends on the shape indices** by up to several dB at large L
  (`probe_shape_level.py`), which our decoder does not reproduce; not located.
- **Long erasures and heavy errors**: concealment of runs of eight or more bad frames, and whether
  muting (DVSI and the AMBE-3000R at 10 %, our IMBE decoder) or playing damaged frames is better,
  need listening.
