# Encoder quality

The three voice encoders (AMBE+2 3600x2450 / 2450, D-STAR 3600x2400, IMBE 7200x4400 / 4400) share
one analysis stage, OP25's fixed-point implementation of TIA-102.BABA by Pavel Yazev
(`src/analysis/imbe_vocoder`), behind a block-floating-point input gain, with a floating-point
pitch correction beside it; each rate then has its own quantiser. The AMBE encoders are measured
through the AMBE-3000R's decoder, which is DVSI's reference decoder
([decoder-quality.md](decoder-quality.md), "DVSI's test vectors"); IMBE, which has no hardware
here, through our decoder and JMBE (sdrtrunk's decoder, written from the standard), with
OpenBLIP25 (a port of DVSI's reference software vocoder, used by its output only) as the
reference encoder. Scores are PESQ (narrowband) and ESTOI against the original speech.

## Status

Through the reference decoder, against each rate's reference encoder:

| | AMBE+2 | D-STAR | IMBE (our decoder / JMBE) |
|---|---|---|---|
| DVSI's 27 test vectors, ours | **3.131 / 0.778** | **3.107 / 0.783** | **3.052 / 0.778**, 2.961 / 0.753 |
| DVSI's encoder | 2.981 / 0.769 | 2.931 / 0.766 | 2.935 / 0.787, 2.828 / 0.761 |
| the AMBE-3000R's encoder | 3.033 / 0.776 | 3.023 / 0.770 | |
| 70-file corpus, ours | 3.044 / 0.806 | 3.015 / 0.810 | 3.071 / 0.816, 3.021 / 0.808 |
| the AMBE-3000R's encoder / OpenBLIP25 | 2.943 / 0.810 | 2.995 / 0.805 | 3.107 / 0.828, 2.996 / 0.814 |

Through our own decoder the order is the same on every set. Our frames degrade under
channel errors as the reference encoders' do ("Under channel errors"). The encoder's delay is
189 samples (D-STAR 239; 349 / 399 with noise suppression), its cost about 0.3 ms per frame
([benchmarks.md](benchmarks.md)).

## How the encoders work

### Analysis

OP25's analysis (DC removal, pitch estimation with look-back / look-ahead tracking, voicing per
band, spectral amplitudes), with two fixes: the pitch tracker's sub-multiple test now runs on the
look-back path too and accepts a near-zero error outright (steady voices at 150-350 Hz used to
lock onto a half, third or fifth of the pitch), and the input gain of "Quiet input". The input is
delayed so that each frame describes the same 20 ms as the AMBE-3000R's frame: 48 samples
(AMBE+2, IMBE), 98 (D-STAR, whose frames the AMBE-3000R places 50 samples later); aligned, the
per-frame gain spread against the AMBE-3000R halves and the shape difference falls by a fifth.

### Quiet input

The fixed-point window, FFT and pitch correlations lose resolution on quiet input: the dam
recording encoded at -10 / -20 / -40 dB scored AMBE+2 3.34 / 3.10 / 1.86 against 3.47 at full level
(D-STAR 3.21 / 3.10 / 2.07 against 3.41, IMBE 3.10 / 2.66 / 1.45 against 3.38), half the AMBE+2
voicing indices changing at -20 dB. `MBEVOCODER_ENCODER_INPUT_GAIN` (on) is block floating point in
front of the analysis: each frame the input is raised by 2^k, k the largest shift (up to 8) that
keeps the raw peak of everything the analysis still holds (its 621-sample history and this frame)
at or below -6 dBFS; when k changes the stored history and level-dependent state (input buffers,
filter memories, the voicing threshold's energy tracker) are rescaled with it
(`imbe_vocoder::rescale_input`), so the window holds one gain throughout, and the quantisers take k
off again in the log2 domain, where it is exact. The floating-point analysis, tone detector and VAD
see the input as it is. Nothing clips.

The lost resolution had also been gating voicing: on quiet background the analysis' band errors
were all noise, so pauses came out unvoiced, as the AMBE-3000R's and DVSI's encoders code them
(AMBE+2, the quietest third of the corpus' frames by the AMBE-3000R's gain: 1.8 % of bands voiced,
the AMBE-3000R 1.9 %; DVSI's encoder 3.6 % on its own vectors). At full resolution the tracker,
sitting at the bottom of its range on noise (66 Hz), voiced the background as a comb across every
band, 20 % of bands, a buzz in every pause. Two changes come with the gain: the voicing threshold's
frame energy and tracked maximum are kept in double precision (`set_wide_energy`; in 32 bits the
raised input saturated both), and a level gate scales the threshold by the frame's energy above
200 Hz against its own tracked maximum, from nothing at -35 dB to full at -25 dB (`set_level_gate`;
energy below 200 Hz is left out because low-frequency background carries level the coded harmonics
do not). Quiet-third voicing is then 8 % on AMBE+2 and 8.5 % on D-STAR, the rest being onsets and
offsets at speech level; per-band agreement with the AMBE-3000R 0.876 / 0.848 (0.896 / 0.862 without
the input gain, 0.848 / 0.826 without the gate). A gate on absolute level would have matched the
corpus too, but DVSI's encoder voices a whole recording at -62 dBFS as at -22 (its voicing is level
independent; the AMBE-3000R's falls with level, 0.37 to 0.21 of bands on AMBE+2), so the gate is
relative. By ear the difference is slight: on eight pause clips (`tools/quality/listening_pauses.py`,
2026-10-12; voiced bands in the pauses 1-2 % for the AMBE-3000R, 4 % gated, 17-18 % ungated) a listener
could not tell the gated, ungated and reference encoders apart, the pauses being 40-70 dB below the
speech.

Now: AMBE+2 3.58 / 3.45 / 3.22 at -10 / -20 / -40 dB (full level 3.51), D-STAR 3.45 / 3.39 / 2.95
(3.45), IMBE 3.54 / 3.46 / 2.97 (3.47), the level out within 0.5 dB of the level in down to -40 dB.
On the corpus (four framings, our decoder, against the encoder without either): AMBE+2 +0.009 ±
0.005 (development) / +0.004 ± 0.006 (new speakers), D-STAR +0.013 ± 0.007 / +0.006 ± 0.005, IMBE
+0.005 / +0.006 (JMBE +0.016 / +0.002), ESTOI +0.002-0.005; through the AMBE-3000R's decoder the
corpus is unchanged (AMBE+2 3.041 -> 3.038, D-STAR 3.004 -> 3.013).
A gain that changes within the analysis window is what to avoid: an envelope-driven version without
the rescaling cost the corpus 0.07, while a fixed +6 dB is neutral. `test_encoder` (`quietInput`)
checks a voice at -40 dB on every rate.

### Background noise at the lowest pitches

At the bottom of the pitch range a harmonic band spans about two bins of the analysis spectrum, and
the voicing test's fit leaves noise little residual, so background noise passes as voiced: 60 Hz mains
hum in the pauses of speech came out at the lowest pitch with every band voiced, a buzz through the
AMBE-3000R (mbelib-neo 2.6.5 found the same, its #149 / #175). The analysis now tracks the background
per bin (a smoothed minimum over frames 15 dB or more under the loudest so far, forgotten 50 frames
after the last) and, at a pitch under 90 Hz, leaves a band unvoiced when its 500 Hz column holds less
than twice it (`imbe_vocoder::noise_gate_bands`). With hum added to corpus recordings, share of
pause frames voiced (D-STAR / AMBE+2 / IMBE; the AMBE-3000R's encoder in brackets): hum at -45 dBFS
0.15 / 0.19 / 0.25 (0.00 / 0.01), 0.20 / 0.27 / 0.36 before; at -35 dBFS 0.29 / 0.26 / 0.51 (0.27 /
0.26), 0.45 / 0.51 / 0.73 before.

The margin is 2x because a wider one unvoices speech in noise as well. On DVSI's 27 inputs, against
the gate off (D-STAR and AMBE+2 through the AMBE-3000R's decoder, PESQ / ESTOI; IMBE through ours):

| margin | D-STAR | AMBE+2 | IMBE | IMBE, 86 corpus recordings | hum at -35 dBFS, IMBE pauses voiced |
|---|---|---|---|---|---|
| 2x (adopted) | -0.002 ± 0.003 / -0.0005 | +0.000 ± 0.001 / -0.0002 | -0.003 ± 0.003 / -0.0021 | +0.003 ± 0.002 / -0.0004 | 0.51 |
| 3x | | | -0.008 ± 0.005 / -0.0036 | | |
| 4x | -0.001 ± 0.002 / -0.0009 | -0.001 ± 0.002 / -0.0003 | -0.011 ± 0.005 / -0.0044 | +0.000 ± 0.002 / -0.0008 | 0.40 |

At 4x DVSI's `noisy` vector lost 0.11 and its car-noise vectors 0.02-0.06 through our IMBE decoder.
Gating only the frames that are themselves background (15 dB under the loudest) does not help
(IMBE -0.010 ± 0.005). Hum before the first louder sound of a stream is not tracked: treating every
frame as background until then let the speech at a stream's start into the estimate and gated less
overall.

### Floating-point pitch correction and amplitudes

`src/encoder/dstar_analysis.cpp` (`MBEVOCODER_DSTAR_PITCH_ANALYSIS`, `MBEVOCODER_AMBE2450_PITCH_ANALYSIS`,
`MBEVOCODER_IMBE_PITCH_ANALYSIS`, all on) runs in double precision beside the fixed-point stage:

- **Pitch**: DC blocker, centred 129-tap 1200 Hz low-pass, normalised correlation at every
  transmitted period; score = correlation - 0.02 x octave distance from the quantiser's pitch -
  0.03 x octave distance from the previous frame's choice; the pitch moves only on a frame with
  voiced bands, when the best candidate's correlation exceeds 0.7 and beats the quantiser's by
  0.035. On IMBE (208 periods) the vocoder then re-analyses voicing and amplitudes at the moved
  pitch from a copy taken before that stage.
- **Amplitudes** (AMBE rates): band energies of the 221-sample window in a 2048-point FFT,
  integrated over each harmonic's band on the transmitted grid, mean-matched to the quantiser's
  own target so the frame gain is unchanged. IMBE keeps the vocoder's amplitudes (band energies
  lose 0.03 there: its quantiser has the bits for the harmonic fit).

Worth, through the AMBE-3000R's decoder: D-STAR +0.11 / +0.14 (development / new speakers), AMBE+2
+0.12 / +0.15; IMBE +0.04-0.07 through our decoder and JMBE; the pitch correction is most of it. An exact octave
below the voice is not corrected (the input is as periodic at twice the period). mbelib-neo 2.4's
octave check (odd harmonics under a tenth of the even ones' energy: halve the period), tried here
on the harmonic band energies, loses on AMBE+2 over four framings: -0.001 ± 0.001 on the
development set and -0.016 ± 0.008 on new speakers at a tenth, -0.03 / -0.05 at a fifth.

### AMBE quantiser

`src/encoder/ambe_quant.cpp` (from OP25's `encode_ambe`): b0 the nearest table fundamental in the
log domain with amplitudes and voicing resampled onto its harmonic grid, b1 the voicing row, b2 the
gain, b3..b8 the shape from a block DCT of the prediction residual, the prediction loop closed with
the library's own decoder (`mbe_decodeAmbe2450Parms` / `2400Parms`), so encoder and decoder cannot
drift apart. What makes it match the AMBE-3000R:

- **Level**: calibrated from the AMBE-3000R's decoded amplitudes (voiced log2 offset -2.0, unvoiced
  by the decoder's 0.2046 / sqrt(w0)), so the AMBE-3000R decodes our frames at the loudness of its
  own encoding. D-STAR's gain has no frame-to-frame memory on the AMBE-3000R
  ([decoder-quality.md](decoder-quality.md), "D-STAR tables"), and the closed loop uses the same.
- **Voicing**: re-decided from the analysis' per-band error ratios with per-band multipliers tuned
  on the corpus separately for each rate, the upper bands relaxed when the four lowest are voiced,
  and frames below 95 Hz voicing only the lowest bands coded unvoiced (the tracker settles at the
  bottom of its range on noise). 89 % per-harmonic agreement with the AMBE-3000R on AMBE+2.
- **Unvoiced frames on a fine grid** (AMBE+2): every fully unvoiced frame is coded on at least 50
  harmonics, as the AMBE-3000R does (our tracker had put a fifth of them on 9-14 wide bands);
  +0.02-0.06. D-STAR keeps the tracker's grid (the rule costs it 0.03-0.08).
- **Silence and quiet frames**: digital silence is the AMBE-3000R's canonical frame (b0 119, b1 16,
  b2 0); an amplitude floor of 2.0 on the analysis amplitudes (`amplitude_floor`) models its noise
  floor on ordinary quiet frames.
- **D-STAR's shape search** (`MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER`, on): two coordinate-descent passes
  refine the six shape indices together against the harmonic-domain residual, the squared log
  error weighted by the square root of the target amplitude relative to its geometric mean
  (bounded 0.1-10). +0.03.
- **Bit order**: the AMBE+2 fields are packed in the AMBE-3000R's order of b3 and b4's low bits
  ([bit-order.md](bit-order.md)); with mbelib's order our frames lost 0.16 PESQ through its decoder.

### D-STAR's voicing index

D-STAR's b1 is chosen by the decoder's rule (each voiced 1 kHz band reaching 250 Hz below it,
`AmbePlusHarmonicVoiced`; [decoder-quality.md](decoder-quality.md), "Mixed excitation"), for analysis
frames and for the transcoder alike. Through the AMBE-3000R's decoder, together with the background
gate: PESQ +0.001 ± 0.005 on 29 recordings.

### Mixed frames and the voicing index

On the decoded-model path (`AmbeQuantizer::quantizeModel`, the transcoder's), the voicing row is
the one whose measured voiced fractions (`mbe_parms.Vmix`) are nearest to the source's, and the
row's level offsets are taken out of the targets: re-quantising the AMBE-3000R's own decoded frames
reproduces 87 % of them index for index (92-97 % per field from the same state) and their decode to
0.8 dB LSD. The analysis path keeps binary voicing rows (the fractions cost 0.005-0.02 there).

The AMBE-3000R's encoder also uses the unvoiced transient rows 17-31 on 17 % of its unvoiced
frames, rows 20 and 28 on onsets coded at the burst's level; ours never emits them. A rule from the
frame's energy fired five times as often, rarely where the AMBE-3000R's did, and cost 0.02-0.07: its
transient detector keys on something else. Worth at most 0.04
([decoder-quality.md](decoder-quality.md), "Unvoiced rows 18-31: the band grid").

### IMBE

The vocoder's channel codewords u0..u7 (12, 12, 12, 12, 11, 11, 11, 7 bits) are repacked MSB first
into `imbe_d[88]`, FEC-encoded and laid out for P25 Phase 1 or YSF VW; NXDN full rate carries the
same codeword. Below harmonic 1 the quantiser predicts from harmonic 1's amplitude, as DVSI's
decoder and ours do (not the standard's M_0 = 1, which JMBE uses; the vocoder kept 1/4 in its own
units): through a DVSI-like decoder our frames gain +0.051 PESQ over frames fitted to the zero,
through JMBE +0.021 PESQ and -0.003 ESTOI ([decoder-quality.md](decoder-quality.md), "IMBE: the
previous frame's amplitude below harmonic 1"). `imbe_vocoder::imbe_encode_model` quantises from a model (the transcoder's AMBE to IMBE
path). The frames decode through JMBE, which shares no code with us, 6.0 dB LSD from our decode,
the distance our AMBE decoders sit from the AMBE-3000R's: the layout, FEC and quantiser are sound.

Against OpenBLIP25: its pitch agrees with ours within 50 cents on 80 % of jointly voiced frames,
voicing on 85 %, shapes within 3.2 dB. Its remaining lead, about 0.01-0.02 ESTOI, is in its voicing
decisions taken together with its amplitudes (oracles substituting its decoded pitch, voicing, gain
or shape one at a time each gain 0.002 ESTOI or less; its pitch, shape and voicing together
+0.016-0.018). OpenBLIP25 does not reproduce DVSI's P25 vector bits either (0.1 % of frames); its
pitch is within 2 % of DVSI's on 82 % of DVSI's voiced frames, ours on 75 %.

### Voicing threshold

`MBEVOCODER_IMBE_VOICING_THRESHOLD` (off) scales the standard's voicing threshold by 1.75 on every
band (`imbe_vocoder::set_voicing_scale`, IMBE only): our analysis voices 30.5 % of band-frames
against OpenBLIP25's 36.1 %. ESTOI +0.004-0.006 through both decoders on every set (30 / 32 and
31 / 32 files better on the fresh set), PESQ +0.02 through JMBE and level through ours. A blind
listening check (ten clips) preferred the current encoder twice, x1.75 never, no preference eight
times: off by default. Scaling only some bands, lowering the unvoiced level, and level corrections
on top of it all lose; closing the rest of OpenBLIP25's lead would take a different voicing
measure.

## Under channel errors

Our frames and the reference encoder's damaged identically (BER 1 / 3 / 5 %, bursts in 5 % of
frames, 5 % random frames; `tools/quality/encoder_errors.py`, `encoder_errors.json`, 70 files),
loss from clean of ours minus the reference's:

| rate, decoder | BER 1 / 3 / 5 % | bursts / random frames |
|---|---|---|
| AMBE+2, AMBE-3000R (clean 3.044 against 2.943) | -0.018 / -0.031 / -0.062 | -0.011 / -0.059 |
| D-STAR, AMBE-3000R (3.015 against 2.995) | -0.022 / -0.001 / -0.048 | +0.001 / -0.030 |
| IMBE, our decoder (3.071 against OpenBLIP25's 3.107) | -0.005 / +0.015 / -0.035 | +0.001 / -0.020 |
| IMBE, JMBE (3.021 against 2.996) | -0.002 / -0.005 / -0.062 | +0.000 / -0.034 |

Within two standard errors except AMBE+2's 0.059-0.062 at BER 5 % and random frames (2.7-2.8 SE),
which still leaves our frames ahead at every error rate, and JMBE's 0.062 at BER 5 % (2.9 SE);
ESTOI losses are the same within 0.004. Nothing in how the encoders choose indices
makes their frames more fragile.

## DVSI's test vectors

`tools/quality/dvsi_encoders.py` encodes DVSI's 27 speech vectors with our encoders and the
AMBE-3000R's and compares them with DVSI's own encoded bits through a common decoder: for the AMBE
rates the AMBE-3000R (= DVSI's decoder), for IMBE ours and JMBE (`dvsi_encoders.json`). Paired PESQ
against DVSI's encoder, by kind of recording:

| Recordings | D-STAR | AMBE+2 | IMBE (ours / JMBE) | the AMBE-3000R's encoder (D-STAR / AMBE+2) |
|---|---|---|---|---|
| all 27 | +0.181 ± 0.030 | +0.149 ± 0.031 | +0.121 / +0.138 | +0.092 / +0.053 |
| quiet: dam20, dam40, dam80 (-42 to -68 dBFS) | +0.040 ± 0.031 | +0.079 ± 0.025 | +0.108 / +0.161 | +0.01 / +0.16 |
| noisy: dam10, damcar, noisy, three car recordings | +0.440 ± 0.045 | +0.411 ± 0.037 | +0.407 / +0.376 | +0.34 / +0.32 |
| the other 18 | +0.118 ± 0.011 | +0.073 ± 0.020 | +0.028 / +0.055 | +0.02 / -0.06 |

DVSI's vectors come from an encoder that copes worse with noise than this AMBE-3000R and ours; the
AMBE-3000R does not reproduce them bit for bit. The quiet three are where the input gain matters
("Quiet input").

## mbelib-neo's encoders

mbelib-neo has a D-STAR encoder since 2.3 and AMBE+2 and IMBE encoders since 2.4.
`tools/quality/neo_encoders.py` encodes the corpus with its 2.6.5 encoders and passes their frames
bit for bit to the AMBE-3000R (`neoenc` in `tools/quality/dvsi_harness`, `mbe-cli frpack`;
`corpus/baselines/neo_encoders.json`). PESQ / ESTOI, paired difference against ours:

| Encoder | AMBE+2, the AMBE-3000R's decoder | D-STAR, the AMBE-3000R's decoder | IMBE, our decoder / JMBE |
|---|---|---|---|
| ours | **3.044 / 0.806** | **3.015 / 0.810** | 3.071 / 0.816, 3.021 / 0.808 |
| mbelib-neo 2.6.5 | 2.906 / 0.786 (-0.138 ± 0.029) | 2.972 / 0.795 (-0.043 ± 0.032) | 3.057 / 0.817 (-0.014 ± 0.024), 3.000 / 0.802 (-0.021 ± 0.023) |
| the AMBE-3000R's / OpenBLIP25 | 2.943 / 0.810 | 2.995 / 0.805 | 3.107 / 0.828, 2.996 / 0.814 |

mbelib-neo 2.5 took the AMBE-3000R's order of AMBE+2's b3/b4 low bits ([bit-order.md](bit-order.md))
and D-STAR's memoryless gain ([decoder-quality.md](decoder-quality.md), "D-STAR tables"), which
halved its AMBE+2 loss (-0.252 in 2.4.3) and took 0.6 off its D-STAR loss (-0.990). Up to 2.6.2
most of what was left on D-STAR was b0 125 (-0.398 ± 0.035): its encoder sent it on 11.7 % of the
corpus' frames (the lowest pitches and quiet frames), and the AMBE-3000R plays b0 125 as a lost frame
([decoder-quality.md](decoder-quality.md), "D-STAR special frames"). 2.6.5 sends neither b0 124 nor
125 (none in 26017 frames), and its D-STAR encoder is now within error of ours through the
AMBE-3000R (-0.043 ± 0.032; through our decoder -0.083 ± 0.029). Its AMBE+2 loss is unchanged, and its
IMBE encoder, where the decoders agree, is level with ours.

## Front-end oracles

`tools/quality/frontend_oracle.py` replaces one part of our analysis with the AMBE-3000R's decoded
values for the same frame (`mbe-cli encode --oracle-file`, hooks for experiments only) and scores
the result. What the oracles say now:

- **D-STAR, against the AMBE-3000R's encoder on the fresh set**: on aligned frames its pitch,
  voicing, gain or shape in our frames gains nothing (-0.12 to -0.02 each, +0.011 all four
  together); what edge its own frames have is in its choice of indices.
- **IMBE, against OpenBLIP25**: no single part carries its lead ("IMBE").
- Under LSD no part of the AMBE-3000R's analysis is a better estimate of the speech than ours: its
  values move our frames towards its audio (7.4 dB) and away from the original.

## Listening

One listener, headphones, blind level-matched pairs. The decisions they set:

- D-STAR's floating-point analysis (fresh set, through the AMBE-3000R's decoder): preferred twice
  and no preference three times against the previous encoder; against the AMBE-3000R's encoder one
  each way. Adopted (rule: +0.05 PESQ on the fresh set, ESTOI not worse, no file below -0.15, the
  listener prefers it or hears no difference). AMBE+2's and IMBE's single pairs were rated for it.
- AMBE+2 fricatives (`listening_fricatives.py`, ten fricative-rich clips): encoders the
  AMBE-3000R's twice, ours once, no preference three times; decoders ours twice, no preference
  twice. ESTOI favoured the AMBE-3000R on nine clips of ten; the unvoiced ESTOI gap is not heard.
- IMBE voicing threshold ("Voicing threshold"): no gain heard; off by default.

## Method

- **Framing noise.** Shifting the input by a few samples moves single files by up to 0.5 PESQ (the
  tracker and predictor carry the difference); one file's difference between two framings has a
  standard deviation of 0.16. Comparisons average each recording over framings 0, +40, +80 and
  +120 samples (`tools/quality/dstar_ladder_inputs.py`), and report paired differences ± SE.
- **Recordings.** Development: the 70-file corpus (`corpus/SOURCES.md`), used for tuning. Held out,
  each fixed before its first scoring: four sets of eight LibriSpeech test-clean speakers (64
  recordings, "new speakers") and the fresh set (`tools/corpus/prepare_validation4.py`: ten
  LibriSpeech test-other speakers and four CMU ARCTIC accents, 32 recordings); all are now used, so
  the next decision needs another set. DVSI's test vectors are the independent check.
- **Tools.** `tools/quality/rate_ladder.py` (any rate, rows of executables, our decoder, the
  AMBE-3000R's decoder and encoder, JMBE, framings), `dstar_ladder.py`, `dvsi_encoders.py`,
  `encoder_errors.py`, `hil_encoder.py`, `listening_*.py`. The AMBE-3000R on COM3 at 460800 baud,
  firmware V120.E100.XXXX.C106.G514.R009.B0010411.C0020208.
- **Controls.** Every option reproduces its predecessor's frames bit for bit when off, and each
  rate's option changes only that rate. `test_dstar_quantizer`, `test_dstar_analysis` and
  `test_encoder` cover the search, the pitch correction, silence, tones, reset and quiet input.

## Rejected

Each against the default of its day, within the framing noise unless marked: a lower amplitude
floor; pitch refinement to FFT bin 25; local-autocorrelation and harmonic-fit pitch correction;
gain correction after the shape search; joint gain and shape search; asymmetric spectral error;
other amplitude resampling; no interpolation across voicing boundaries; evidence-weighted,
periodicity-fitted and hardware-trained voicing (failed validation); re-analysis at the
transmitted pitch; more or reversed search passes; gain smoothing with look-ahead (-0.03 to
-0.06, and 20 ms of delay); harmonic least-squares and other amplitude estimators (LPC, cepstral,
peak picking: all as far from the AMBE-3000R's shapes as ours); undoing the unvoiced scale in the
target; reconstruction-guided target selection (a frame's error can fall while the utterance gets
worse); compressed multi-window objectives (trade PESQ for ESTOI); an encoder transient rule
("Mixed frames and the voicing index"); a decaying-envelope input gain ("Quiet input"); mbelib-neo
2.4's octave-low check ("Floating-point pitch correction and amplitudes").

## Open

- **The AMBE-3000R's transient rows**: its onset detector is unidentified (at most 0.04).
- **IMBE voicing**: what is left of OpenBLIP25's lead needs a different voicing measure.
- **Other material**: real radio recordings and other languages (tonal ones for the pitch
  correction) have not been part of any comparison.
