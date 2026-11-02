# Encoder front end: tone detection, VAD/DTX, noise suppression, echo cancellation

The AMBE-3000R offers four features around its encoder, and the library has them too, all
in-house (no dependencies): automatic tone detection, voice activity detection with silence
frames (DTX) and matching comfort noise on the decoder side, a noise suppressor and an echo
canceller. For each: how the AMBE-3000R behaves, what the library does, and how they compare.

All four are `mbe_encoder_options` fields (`tone_detect`, `dtx`, `noise_suppress`,
`echo_cancel`); `mbe_encoderFrameFlags()` reports the last frame's `MBE_ENCODER_FLAG_TONE`
and `MBE_ENCODER_FLAG_VOICE_ACTIVE`, the AMBE-3000R's `ECMODE_OUT`. The order inside the encoder is
echo cancellation, noise suppression, then the analysis with tone detection and the VAD on
what the analysis sees. On IMBE only noise suppression and echo cancellation apply (it has no tone or silence frames).

## Tone detection (`tone_detect`, default on)

**AMBE-3000R** (`tools/quality/probe_tone_detect.py`, `ECMODE` bit 12, on at reset): the decision
is per frame with no hold-in or hold-off (a 40 ms tone gives one tone frame, 200 ms nine);
a tone is coded from about -37 dBFS up (-40 is voice, -35 tone) and down to about 17 dB of
tone-to-noise ratio (20 dB SNR white noise: 26 of 30 frames, 15 dB: none); an off-grid
frequency goes to the nearest id (with boundary flips between two ids); DTMF is accepted at
up to 10 dB of twist; the 16 KNOX pairs and the four call-progress pairs (dial 350+440, ring
440+480, busy 480+620, reorder 350+490 Hz) are recognised, a pair 26 Hz off a KNOX pair is
still coded as it; the amplitude id is `127 - round((0.2 - L) / 0.714)` with L the component
level in dBFS (the mean of the two for a dual tone); a -20 dBFS tone over speech is detected
in 59 of 60 frames; no tone frames are coded on the 70-file speech corpus. D-STAR is the same
except that KNOX tones stay voice (D-STAR has no code for them).

**Library** (`src/encoder/tonedetect.cpp`): a 256-point FFT with a Hann window over the
previous 96 samples and the frame, the two strongest peaks with parabolic interpolation, a
least-squares fit of one or two sinusoids at the measured frequencies (nominal ones for the
call-progress pairs the 31 Hz analysis cannot separate), and the AMBE-3000R's thresholds: residual
below -17 dB, level from -37 dBFS, twist up to 10.5 dB. A single tone is rejected when a
second or third harmonic is within 25 dB of it: low-pitched vowels can be as pure as a tone
for 20 ms, and that rule is what keeps the corpus at zero tone frames (the AMBE-3000R's count).
The detector sees the input with the voice frames' 189-sample delay (239 on D-STAR), so a tone frame replaces
the voice frame that would have covered the same 20 ms.

**Result**: 97.8 % frame agreement with the AMBE-3000R on the probe (3139 of 3211 frames, AMBE+2
and D-STAR), amplitude ids identical, zero tone frames on the speech corpus. The
disagreements are boundary flips on off-grid tones and the tone-over-speech section (the
AMBE-3000R 59 of 60 frames, ours 24: a -20 dBFS tone under speech fails our purity test more
often).

**Tone under maskers** (`probe_tone_masking.py`): a 1000 Hz -20 dBFS tone with
white, 500 Hz low-pass, 2 kHz high-pass, 800-1200 Hz band-pass and pink noise at 20 to 0 dB
tone-to-noise ratio, and with speech at +10 to -12 dB tone-to-speech ratio, 30 frames each.
The AMBE-3000R: white 0/30 already at 20 dB (26/30 on an earlier realisation: 20 dB is its
edge), low-pass 7/30 at 20 dB, high-pass 0, band-pass near the tone 28/30 at 20 and 7/30 at
15 dB, pink 29/30 at 20 dB and 0 at 15; speech 23/30 at +10 dB, 15/30 at +6, 0 at 0 dB, and
frame by frame it accepts a speech residual of -10 to -18 dB below the frame and rejects
-2 to -11 dB, so its threshold under speech is about 11 dB where under noise it is about
20 dB. Ours (a 17 dB residual test on the whole frame): the noise sections within a few
frames of the AMBE-3000R's, speech 5/30 and 1/30. The masker dependence (band-limited noise
near the tone and harmonic speech tolerated, broadband and low-pass noise not) is not the
whole-frame residual, nor a local prominence test, nor the peak frequency's accuracy
(refining it by the fit changed nothing); it looks like a measure taken after the analysis'
own harmonic model has explained the frame, which would need the analysis stage in the
detector. Left as is: the corpus stays at zero tone frames, and a tone 6-10 dB above speech
is the only case that differs.

## VAD and DTX (`dtx`, default off)

**AMBE-3000R** (`tools/quality/probe_dtx.py`, `ECMODE` bit 11; `PKT_CHANFMT` bit 0 makes the AMBE-3000R
put `ECMODE_OUT` in its channel packets, which the `ambe3000` tool records as a `.cmode`
file): speech with pauses at several background levels. With DTX on the AMBE-3000R still produces
a frame every 20 ms; `VOICE_ACTIVE` = 0 marks the ones it would not transmit, and those are
*silence frames*: AMBE+2 fundamental index 124 with voicing index 16, D-STAR index 120 with
voicing index 0, all bands unvoiced, gain and shape describing the background.

The decision follows the frame energy against a background estimate: pauses in white noise
at -40 and -50 dBFS become silence about three seconds after the background last stepped up,
and stay voice for that long; noise at -30 dBFS and above is always voice; voice holds for
about three frames after speech ends; a step *down* to digital silence gives one or two
frames of voice. The AMBE-3000R's estimate falls at once after digital silence, so anything
following a muted stretch is voice for three seconds.

The silence frames themselves: on AMBE+2 the gain index is constant through a pause (30 for
-40 dBFS white noise, 28 for -50, 5 for digital silence) and the shape codes vary only
slightly, while the *decoded* gain climbs geometrically (9.7, 10.6, 11.1, ... 11.55 with
mbelib's predictor) - the AMBE-3000R's silence frames are coded against a fixed state.

**AMBE-3000R decoder** (`build/probe/sil*.dvsi`, synthesised frames decoded on the AMBE-3000R): an AMBE+2
silence frame renders at a level set by `0.5 * gamma(last voice frame) + dg[b2]` - the
ordinary gain law applied once - and stays there however many silence frames follow: the
prediction state is not advanced by silence frames. The spectrum is likewise predicted from
the last voice frame's. Fitting the harmonic model against 18 combinations of preceding
frame and shape codes gives `w0 = 2 pi / 32` with 15 bands (index 124; 14 for 125) - the
model mbelib already uses for D-STAR silence frames; JMBE's `pi / 32 * 2 pi` (which mbelib
copied for AMBE+2) puts the bands 2 kHz apart and rendered the AMBE-3000R's silence frames 27 dB
too loud with the wrong spectrum. The level is a further 13.8 dB (a factor 0.2046, the
unvoiced scale constant) below the voice-frame law, sd 0.4 dB over the 18 cases; shape error
4 dB rms over 16 bands. D-STAR silence frames (index 120) are ordinary voice frames to the
AMBE-3000R's decoder: memoryless gain, the state advances, and mbelib's decoder already renders
them within 2 dB.

**Library**:

- `src/encoder/vad.cpp`: frame energy against a background estimate that follows the frame
  energy with a 20-frame time constant, rising by at most a factor 3 per update (about
  0.4 dB per frame), falling at once, and dropped to nothing by a frame below -45 dBFS (the
  AMBE-3000R re-learns the background from scratch after any quiet stretch, so the three
  seconds of voice that follow a step up also follow a near-silent pause in the speech);
  voice when the frame is 8 dB above it or above -35 dBFS; two frames of hangover. Fitted to
  the AMBE-3000R's decisions on four speakers (94.5 / 96.7 / 97.8 / 98.2 % frame agreement over
  the whole probe, `probe_dtx.py --ours --speech NAME`). Digital
  silence is never voice and leaves the estimate alone: after a muted stretch the library
  does not, as the AMBE-3000R does, class three seconds of background as voice.
- `AmbeQuantizer::quantizeSilence`: the rate's silence fundamental (index 124 / 120), all
  bands unvoiced with the AMBE-3000R's voicing index, the unvoiced amplitude estimate resampled onto
  that grid and smoothed over the run of silence frames (0.8 / 0.2: with DTX the application
  sends one silence frame in several, each standing alone). On AMBE+2 the codes are formed
  against the quantiser's state as it stands after the last voice frame and the state is not
  advanced, exactly as the AMBE-3000R's decoder will read them, with the 13.8 dB in the level target.
- `MBEVOCODER_CHIP_MEASURED` (decoder, default on; off in the `neo-compat` preset): AMBE+2 silence
  frames use `2 pi / 32`, the extra factor 0.2046, and leave the prediction state as it was
  (`mbe_parms.silenceFrame`).
- `mbe_synthesizeComfortNoiseFramef()`: for the 20 ms slots in which no frame arrives because
  the sender withheld it, the last rendered frame is repeated with every band unvoiced at its
  level (what a silence frame renders), the synthesis state moving on and the prediction
  state not.

**Result** (`probe_dtx.py --ours`): voice/silence agreement 94-98 % over the whole probe on
four speakers (above), the same speech onsets and hangover; the
frames we send in a -40 dBFS white-noise pause are the AMBE-3000R's (`124 16 30 ...`) and decode on
the AMBE-3000R at -40.1 dBFS (the AMBE-3000R's own at -41.8), on our decoder at -41.2; -50 dBFS noise:
-49.7 / -52.0 / -50.0; D-STAR within 2 dB throughout. The AMBE-3000R's silence frames decode on
our decoder at -42.1 dBFS where the AMBE-3000R renders -41.8 (our decoder's level being the
AMBE-3000R's, [decoder-quality.md](decoder-quality.md), "Output level").

## Noise suppression (`noise_suppress`, default off)

**AMBE-3000R** (`tools/quality/probe_ns.py`, `ECMODE` bit 6): speech with white and pink noise at
25, 15 and 5 dB SNR, then noise alone. The suppressor takes 12 to 15 dB out of stationary
noise in pauses when it acts, but it acted only on the first section (white, 25 dB) and, after
three seconds, the last (pink, 5 dB); the other four sections came out identical to the
unsuppressed encode, and the noise-only sections after a digital-silence gap were untouched.
Its noise estimate evidently updates only in what its VAD calls silence, which above -35 dBFS
is nothing. PESQ against the clean speech after the AMBE-3000R's decoder: no gain in any section
(white 25 dB: 2.53 off, 2.33 on). It is not a usable reference, and the library's suppressor
is designed on standard lines and tuned by PESQ instead.

**Library** (`src/encoder/noisesuppress.cpp`): weighted overlap-add with a square-root Hann
window of 320 samples every 160 and a 512-point FFT; the noise power per bin is the minimum
of the smoothed periodogram (0.7) over the last two seconds (eight sub-windows of twelve
frames) times 1.2; the gain is a Wiener filter on the decision-directed a priori SNR (0.95),
floored at -18 dB and smoothed over three bins. Digital-silence frames and the smoothing ramp
out of them are kept out of the minimum. One frame of delay: `mbe_encoderDelaySamples`
returns 349 with the suppressor on. The tone detector sees the input before suppression (the
AMBE-3000R's is not applied to tone frames either, and a long tone would otherwise be "noise").

**Result** (front end output against the clean speech, then through the codec): PESQ +0.2 to
+0.5 on the front end and +0.1 to +0.25 after the codec in every condition (white 25 dB:
input 3.02, front end 3.58, decoded 2.32 -> 2.62; pink 5 dB: 2.06, 2.40, 1.85 -> 2.03); pauses
6 to 10 dB quieter, speech 0.3 to 1.4 dB quieter. `test_frontend`: a -36 dBFS noise floor
drops 12 dB under a vowel that loses 0.4 dB.

## Echo cancellation (`echo_cancel`, default off)

**AMBE-3000R**: `ECMODE` bit 13 (`EC_ENABLE`, an LMS canceller of 128 taps per the manual) and bit 9
(`ES_ENABLE`, an echo suppressor). The AMBE-3000R's canceller needs its decoder output as the far
end, which the packet interface does not give us separately; it was not characterised.

**Library** (`src/encoder/echocancel.cpp`): the application gives the far-end samples it played
out during the same 20 ms with `mbe_encoderSetEchoReference()` before each encode (NULL or no
call = silence was played). An NLMS filter of 256 taps (32 ms) with step 0.5 and
regularisation; a Geigel double-talk detector (a microphone sample above half the largest
far-end sample of the span, held for 40 ms) freezes adaptation; a residual suppressor takes a
further 12 dB when the far end is active, there is no double talk and the filter is already
removing 6 dB. The application aligns the reference to the microphone within the 32 ms span.

**Result** (`test_frontend`, a three-tap echo path at 6, 10 and 14 ms): ERLE 21 dB with the
far end alone, 24 dB after a second of double talk, near-end speech through double talk with
correlation 1.00 to the original.

## Tools

- `mbe-cli encode --tone-detect|--no-tone-detect --dtx --noise-suppress --echo-cancel far.wav
  --flags flags.txt --fe-out fe.wav`: the options, the per-frame `mbe_encoderFrameFlags()` and
  the input as the analysis saw it after the front end.
- `ambe3000 --ecmode 0x0800 --ctl 150001 encode ...` writes `out.dvsi.cmode` (the AMBE-3000R's
  `ECMODE_OUT` per frame).
- `tools/quality/probe_tone_detect.py`, `probe_dtx.py`, `probe_ns.py` (each with `--ours`).
