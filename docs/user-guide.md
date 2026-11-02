# mbevocoder user guide

mbevocoder decodes and encodes the vocoders used by digital voice radio:

| Codec | Rate name | Protocols |
|---|---|---|
| AMBE 3600x2400 | `MBE_RATE_AMBE3600x2400` | D-STAR |
| AMBE+2 3600x2450 | `MBE_RATE_AMBE3600x2450` (with FEC), `MBE_RATE_AMBE2450` (49 parameter bits) | DMR, P25 Phase 2, NXDN half rate, YSF DN, dPMR |
| IMBE 7200x4400 | `MBE_RATE_IMBE7200x4400` (with FEC), `MBE_RATE_IMBE4400` (88 parameter bits) | P25 Phase 1, YSF VW, NXDN full rate |
| IMBE 7100x4400 | decode only | ProVoice |

The API is one C header, `<mbevocoder/mbevocoder.h>`, with every symbol prefixed `mbe_`. Two
compatibility headers carry other libraries' APIs on the same code:

| Header | What it declares | For |
|---|---|---|
| `<mbevocoder/mbevocoder.h>` | everything: decoder, encoder, transcoder, tone frames, on-air layouts, frame assembly | new code, and anything that encodes |
| `<mbelib-neo/mbelib.h>` | mbelib-neo 2.6's API (all but the TETRA decoder): the decoder, and its encoder functions on our encoders (included by `mbevocoder.h`) | code written for mbelib-neo, such as SDRangel, which builds without changes |
| `<mbelib/mbelib.h>` | the decoder with its functions in their original mbelib 1.x form (section 8) | code written against mbelib 1.x |

The API reference is generated from the headers with Doxygen (`doxygen Doxyfile`, output in
`build/doxygen/html`).

The AMBE side of the library was verified against an AMBE-3000R vocoder chip, whose decoder is
DVSI's reference decoder, and every rate against DVSI's published test vectors: the decoder's
bits, and the encoder's, transcoder's and tone frames' behaviour are measured against them (see
the documents linked at the end).

## 1. Building and linking

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets exist for `debug`, `release`, `asan-ubsan`, `tsan`, `strict-fp`, `neo-compat`, `vs2026` and
`android-arm64` / `android-armv7` / `android-x86_64` (Android needs `ANDROID_NDK_HOME`).
CMake 3.21 or newer and a C++17 compiler are required; the library itself has no
dependencies.

The result is one static library, target `mbevocoder::mbevocoder`. Consume it in any of three ways:

```cmake
add_subdirectory(mbevocoder)                   # as a subproject
target_link_libraries(app PRIVATE mbevocoder::mbevocoder)

find_package(mbevocoder CONFIG REQUIRED)       # installed
target_link_libraries(app PRIVATE mbevocoder::mbevocoder)

find_package(mbe-neo 2 CONFIG REQUIRED)        # code written for mbelib-neo
target_link_libraries(app PRIVATE mbe_neo::mbe_static)
```

The install provides the three headers and the pkg-config files `mbevocoder.pc` and
`libmbe-neo.pc` (the name mbelib-neo installs under), so a build written for mbelib-neo finds it either through
`LIBMBE_NEO_INCLUDE_DIR` / `LIBMBE_NEO_LIBRARY` or through `find_package(mbe-neo CONFIG)`
(point `CMAKE_PREFIX_PATH` at the install prefix). SDRangel rebuilt this way has its audio
confirmed working on every protocol it decodes (DMR, D-STAR, YSF, NXDN, P25). On Windows
`MBE_STATIC` is defined by the target, so nothing to set. The pkg-config files find the install
from their own location, so an installed tree can be moved; Debug builds are named
`mbevocoderd` (`libmbevocoderd.a`), so Debug and Release installs can share a prefix (the CMake
targets pick the right one). LICENSE, NOTICE.md and the third-party licence texts go to
`share/doc/mbevocoder`. As a subproject the library leaves the parent's `CMAKE_BUILD_TYPE` alone.

C-only CMake projects are supported; the installed targets include the C++ runtime
link dependencies. For a C compiler with pkg-config, link with
`cc app.c $(pkg-config --cflags --libs --static mbevocoder)` (the library is static,
so `--static` is required to include its runtime dependencies).

Build options: `MBEVOCODER_BUILD_TOOLS`, `MBEVOCODER_BUILD_TESTS` (both on when top level),
`MBEVOCODER_INSTALL_TOOLS` (off: install `mbe-cli` and `ambe3000` with the library),
`MBEVOCODER_BUILD_BENCH`, `MBEVOCODER_FETCH_TESTDATA` (downloads the corpus and hardware PCM used by the
parity and quality tests into `MBEVOCODER_TESTDATA_DIR`, by default `testdata/`), `MBEVOCODER_ENABLE_SIMD` (SSE2/NEON in the decoder, on),
`MBEVOCODER_CHIP_MEASURED` (on: the decoder and the encoder follow the AMBE-3000R as measured: the
D-STAR parameters, tone rendering, mixed-excitation voicing tables, geometric amplitude ramps,
smoothed regenerated phases and an 80 Hz output high-pass in the synthesis, the AMBE+2
silence-frame model and the AMBE-3000R's order of b3/b4's low bits, the AMBE+2 erasures and
unusable tone frames played as repeats, the AMBE-3000R's output level, and the IMBE 7200 frame
decoder counting Golay parity-bit errors;
off restores mbelib-neo's decoder bit for bit and is a verification build only, whose AMBE+2
frames DVSI hardware decodes with neighbouring codebook rows),
`MBEVOCODER_FLOAT_CLIP` (mbelib-neo's clip of the float output, off). Encoder analysis switches, all
measured in [encoder-quality.md](encoder-quality.md):
`MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER`, `MBEVOCODER_DSTAR_PITCH_ANALYSIS`,
`MBEVOCODER_AMBE2450_PITCH_ANALYSIS`, `MBEVOCODER_IMBE_PITCH_ANALYSIS` (on: the floating-point pitch
correction and amplitudes), `MBEVOCODER_ENCODER_INPUT_GAIN` (on: a block-floating-point gain in front of
the fixed-point analysis, so quiet input keeps its pitch and voicing, "Quiet input") and
`MBEVOCODER_IMBE_VOICING_THRESHOLD` (off: IMBE voicing threshold x1.75; a blind listening check heard no
gain). Each restores the earlier encoder bit for bit when off. Development: sanitizer and strict-FP switches,
`MBEVOCODER_WARNINGS_AS_ERRORS`, and `MBEVOCODER_EXTRA_DEFINITIONS` (the decoder's `MBE_EXP_*`
experiment knobs). The `neo-compat` preset (`MBEVOCODER_CHIP_MEASURED=OFF`, `MBEVOCODER_FLOAT_CLIP=ON`) is the
verification configuration in which the decoders give mbelib-neo 2.1's output to within float rounding; it is measurably
worse on every quality metric and not a build to ship.

## 2. Conventions

- **Frames are 20 ms**: 160 samples at 8 kHz per call, in and out. There is no resampling and
  no buffering inside the library: a caller with a continuous audio stream slices it into
  160-sample blocks, one call each.
- **Bits are one per `char`**, value 0 or 1; anything else returns `MBE_STATUS_INVALID_BITS`.
  AMBE frames are `char fr[4][24]` bitplanes and IMBE frames `char fr[8][23]` (the layouts are
  described at the top of the header); parameter bits are `char ambe_d[49]` and
  `char imbe_d[88]`. Packed bytes are converted with `mbe_packBits` / `mbe_unpackBits`.
- **Return values**: 0 or a non-negative count on success; `MBE_STATUS_INVALID_ARGUMENT` (-1),
  `MBE_STATUS_INVALID_BITS` (-2) or `MBE_STATUS_ENCODE_RANGE` (-3) on failure. An argument or
  bit-validation failure leaves the stream state unchanged; `MBE_STATUS_ENCODE_RANGE` means the
  encoder consumed the input (its analysis and front-end history move on) without producing a
  frame or advancing its prediction state, so the caller repeats the previous frame for that slot. Pointer arguments are validated; pass NULL only where the
  reference says it is accepted.
- **One stream, one state**: a decoder stream is an `mbe_parms` triplet, an encoder stream an
  `mbe_encoder`, a transcoder stream an `mbe_transcoder`. Any number of streams may exist; use
  each from one thread at a time. There is no global mutable state except two thread-local
  noise generators inherited from mbelib-neo (see section 8).
- **Output scale and level**: the float decoder output is on the 16-bit range divided by 7, at
  the AMBE-3000R's level, which is DVSI's decoder's (within 0.2 dB on every rate), speech and tone
  frames alike; mbelib-neo 2.3+ decodes at the same level (the `neo-compat` build keeps mbelib-neo
  2.1's, about 16 dB louder). It is not clipped (`MBEVOCODER_FLOAT_CLIP=ON` clips it at 95 % of
  full scale, as mbelib-neo does).
  `mbe_floattoshort` multiplies by 7, clips at full scale (95 % in the `neo-compat` build, as
  mbelib) and rounds to the nearest step,
  and the `short` variants do that for you. Encoder input is on the 16-bit scale for both `short` and `float`.

## 3. Decoding

A stream is an `mbe_parms` triplet (current, previous, previous enhanced). Initialise it once,
then call one `mbe_process*` function per received frame. Each call runs the FEC, decodes the
parameters, synthesises 160 samples, advances the state and reports what happened.

```c
#include <mbevocoder/mbevocoder.h>   /* or <mbelib-neo/mbelib.h>: the decoder is all there */

mbe_parms cur, prev, enh;
mbe_initMbeParms(&cur, &prev, &enh);

/* per received DMR voice frame: 72 on-air bits (36 dibits) in transmission order */
char bits[72], fr[4][24], d[49];
float pcm[160];
mbe_process_result r;
/* ... fill bits[] from the demodulator ... */
mbe_ambeFrameFromBits(MBE_LAYOUT_DMR, bits, fr);
int errors = mbe_processAmbe3600x2450Framef(pcm, &r, fr, d, &cur, &prev, &enh);
if (errors < 0) { /* invalid input; state unchanged */ }
/* pcm[] holds 160 samples; r.total_errors, r.c0_errors, r.flags describe the frame */
```

Which functions to use per protocol:

| Protocol | Frame from the air | Decode call |
|---|---|---|
| D-STAR | `mbe_ambeFrameFromBits(MBE_LAYOUT_DSTAR, ...)` | `mbe_processAmbe3600x2400Framef` |
| DMR, NXDN half rate, dPMR, YSF V/D 1 | `mbe_ambeFrameFromBits(MBE_LAYOUT_DMR, ...)` | `mbe_processAmbe3600x2450Framef` |
| P25 Phase 2 | `mbe_ambeFrameFromBits(MBE_LAYOUT_P25P2, ...)` | `mbe_processAmbe3600x2450Framef` |
| YSF V/D 2 | `mbe_ysfVd2ToAmbe2450Data` (104 de-whitened bits) | `mbe_processAmbe2450Dataf` |
| P25 Phase 1 | `mbe_imbeFrameFromBits(MBE_LAYOUT_P25P1, ...)` | `mbe_processImbe7200x4400Framef` |
| YSF VW | `mbe_imbeFrameFromBits(MBE_LAYOUT_YSF_VW, ...)` | `mbe_processImbe7200x4400Framef` |
| NXDN full rate | own deinterleave to `imbe_fr[8][23]` | `mbe_processImbe7200x4400Framef` |
| ProVoice | own deinterleave to `imbe_fr[7][24]` | `mbe_processImbe7100x4400Framef` |
| AMBE-3000R raw 2450 bytes | `mbe_dvsiToAmbe2450Data` | `mbe_processAmbe2450Dataf` |

If your framing layer already ran the FEC, use the `*Dataf` functions with the parameter
bits. They read the `mbe_process_result` you pass *in* (its error counts and flags drive the
repeat and mute logic), so initialise it with `mbe_initProcessResult` or pass NULL if you
have nothing to report.

Soft decisions: build `mbe_soft_bit` arrays with `mbe_softBitsFromLlr` (negative LLR = 1) or
`mbe_softBitsFromHard`, convert with `mbe_ambeSoftFrameFromBits` / `mbe_imbeSoftFrameFromBits`,
and call the `*SoftFramef` variants. The frame's Golay and Hamming blocks are then decoded by
nearest codeword under the reliabilities; a word that overrules only weak bits counts as a
correctable word, so the repeat and mute rules keep what the soft search decoded.

### What the result tells you

`total_errors` and `c0_errors` are the corrected bit counts (SDRangel shows them). The flags say
how the frame was rendered: `MBE_PROCESS_FLAG_TONE` (a tone frame), `ERASURE` (an erasure or a
tone frame that cannot be played), `REPEAT` (the previous model was reused: too many errors, or an
erasure), `MUTE` (four repeats or a high error rate, output muted), `SILENCE` (an AMBE+2 silence
frame was played), and `PROVOICE` (context set by the 7100x4400 decoders). `mbe_formatProcessResult`
renders the classic `====ETRM` bar.

A lost frame (your framing layer knows a slot was missed) is handled by
`mbe_processLostFramef(pcm, &result, &cur, &prev, &enh)` in place of the decode call: the
previous model is repeated three times, the fourth missing frame fades to silence and the
mute holds until a frame decodes, which then comes out at its proper level - the AMBE-3000R's
`LOST_FRAME` behaviour, measured ([decoder-quality.md](decoder-quality.md), "Bit errors").
Do not feed the last good frame's bits again: that reruns the gain prediction on them and
the level climbs 2-3 dB per repeated frame. A slot the sender withheld on purpose (DTX: the last frame you received was a silence frame,
or your framing layer says the transmitter went quiet) is different: call
`mbe_synthesizeComfortNoiseFramef(pcm, &cur, &prev, &enh)` for each 20 ms until frames
resume, and the background the last frame described carries on at its level.

### Special frames

Tone frames (DTMF, call progress, KNOX and single tones) are recognised and rendered by the
decoders; the AMBE-3000R's amplitude law is followed (about 0.71 dB per step of the 7-bit
amplitude field, full scale at 127), which is up to 50 dB different from mbelib-neo's linear
scaling ([tone-frames.md](tone-frames.md)). D-STAR dual tones are rendered too (mbelib-neo
produced comfort noise for them). Silence frames, the frames a DTX encoder sends in pauses
(AMBE+2 fundamental index 124, D-STAR 120), decode to the background they describe at the
AMBE-3000R's level ([frontend.md](frontend.md); mbelib-neo rendered the AMBE+2 ones 27 dB too loud
with the wrong spectrum). AMBE+2 erasures (fundamental index 120-123) and tone frames the
decoder cannot play are repeated as the AMBE-3000R repeats them: three repeats, a fade, then
silence, without disturbing the prediction state ([decoder-quality.md](decoder-quality.md),
"AMBE+2 erasures and unusable tone frames"). `mbe_classifyAmbe2450Frame` tells the frame types
apart from the parameter bits alone.

## 4. Encoding

```c
#include <mbevocoder/mbevocoder.h>

mbe_encoder* enc = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, NULL);   /* NULL = default options */

short pcm[160];              /* 20 ms of 8 kHz audio, 16-bit scale */
char bits[72];               /* DMR on-air bits, transmission order */
/* ... fill pcm[] ... */
int ret = mbe_encoderEncode(enc, pcm, MBE_LAYOUT_DMR, bits);
if (ret == MBE_STATUS_ENCODE_RANGE) { /* out of range: no frame for this slot, repeat the previous one */ }

mbe_encoderDestroy(enc);
```

The same encoder gives every output form; each call consumes one frame:

| Function | Output |
|---|---|
| `mbe_encoderEncodeParms` / `...Parmsf` | 49 or 88 parameter bits, plus the quantiser indices if wanted |
| `mbe_encoderEncodeAmbeFrame`, `mbe_encoderEncodeImbeFrame` | FEC-protected mbelib frame |
| `mbe_encoderEncode` | on-air bits for a layout (D-STAR, DMR, P25 Phase 2, P25 Phase 1, YSF VW) |
| `mbe_encoderEncodeDvsi` | AMBE-3000R channel-packet bytes (D-STAR, AMBE+2, raw 2450) |

Timing: the encoder delays by 189 samples (`mbe_encoderDelaySamples`: 141 of analysis
look-ahead plus 48 that put our frames on the same 20 ms of audio as an AMBE-3000R's; D-STAR
239, the AMBE-3000R's D-STAR frames sitting 50 samples later), so the frame returned for
input samples n..n+159 describes the speech around n-189 (n-239); with `noise_suppress` on,
one frame more. Feed exactly one 160-sample block per 20 ms;
there is no internal buffering. `mbe_encoderReset` restores the initial state (analysis
history, prediction loop, limiter, tone detector, VAD, suppressor and canceller) for a new call.

Levels: the encoders are calibrated so that an AMBE-3000R decodes our frames at the same
loudness as its own encoding of the same speech; normal speech at about -26 dBov sits in the
middle of the codec's gain range, and the analysis tracks level linearly from full scale down to
about -60 dBFS (an internal gain keeps quiet input at full resolution).
`mbe_encoder_options` lets you shift the gain (`gain_adjust`, in log2 units) and change the
analysis amplitude floor (both AMBE rates only; IMBE ignores them), and enable a peak limiter (`limiter = 1`, -1 dBFS ceiling) for float
input that may exceed full scale. Get the defaults from `mbe_encoderDefaultOptions` before
changing fields.

`mbe_encoderAnalysis` returns the unquantised model of the last frame (fundamental,
harmonic count, voicing, amplitudes) for diagnostics.

### Front end: tone detection, DTX, noise suppression, echo cancellation

Four more option fields, all measured against or designed around the AMBE-3000R's
([frontend.md](frontend.md)); `tone_detect` and `dtx` apply to the AMBE rates only (IMBE has no
tone or silence frames), noise suppression and echo cancellation to every rate:

| Field | Default | What it does |
|---|---|---|
| `tone_detect` | 1 | A single tone, DTMF digit, KNOX or call-progress tone in the input is coded as a tone frame instead of voice, as the AMBE-3000R does by default (97.8 % frame agreement with it, none on speech). `mbe_encoderFrameFlags() & MBE_ENCODER_FLAG_TONE` says so. |
| `dtx` | 0 | Voice activity detection: pauses are coded as silence frames describing the background, and `MBE_ENCODER_FLAG_VOICE_ACTIVE` is cleared for them. A frame is still produced every 20 ms; send the first silence frame of a pause and one every half second or so, skip the rest, and have the receiver call `mbe_synthesizeComfortNoiseFramef` for the slots you skip. |
| `noise_suppress` | 0 | A spectral noise suppressor on the input (about 6 to 10 dB off stationary noise, +0.1 to +0.25 PESQ after the codec at 5 to 25 dB SNR). Adds a frame of delay: `mbe_encoderDelaySamples` returns 349 (D-STAR 399). |
| `echo_cancel` | 0 | An acoustic echo canceller (NLMS, 32 ms span, double-talk detection, residual suppression). Give it the far-end audio you played out during the same 20 ms with `mbe_encoderSetEchoReference(enc, far)` before each encode; ERLE above 20 dB on a synthetic path. |

```c
mbe_encoder_options opts;
mbe_encoderDefaultOptions(&opts);
opts.dtx = 1;
mbe_encoder* enc = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, &opts);
/* per frame */
mbe_encoderEncode(enc, pcm, MBE_LAYOUT_DMR, bits);
if (mbe_encoderFrameFlags(enc) & MBE_ENCODER_FLAG_VOICE_ACTIVE) transmit(bits);
else if (pause_just_started || half_a_second_passed) transmit(bits);   /* a silence frame */
```

IMBE: `MBE_RATE_IMBE7200x4400` with `MBE_LAYOUT_P25P1` or `MBE_LAYOUT_YSF_VW` gives the 144
on-air bits; NXDN full rate uses the same codeword with its own interleave, so take the frame
from `mbe_encoderEncodeImbeFrame` and apply it yourself. There is no IMBE tone frame.

### Tone frames

```c
char bits[72];
mbe_encoderTone(enc, 129, 112, MBE_LAYOUT_DMR, bits);   /* DTMF '1' (697 + 1209 Hz) at about -10 dBFS */
```

Tone identifiers are the AMBE+2 numbering: 5 and 6 for 156.25 and 187.5 Hz, 7..122 for
31.25 Hz multiples, 128..143 DTMF (0-9, A-D, *, #), 144..159 KNOX, 160..163 dial, ring, busy,
reorder; `mbe_toneIdFromFrequencies` finds the id for a frequency or pair. The amplitude is the
7-bit field the AMBE-3000R uses: 127 is a full-scale sine, each step down is about 0.71 dB (112 is
about -10 dBFS). D-STAR numbers dual tones differently and has no KNOX tones; the library
converts, and reports `MBE_STATUS_ENCODE_RANGE` for a tone D-STAR cannot carry. Tone frames
do not consume audio and do not disturb the voice prediction state, so they can be inserted
between voice frames from the same encoder.

## 5. Transcoding

An `mbe_transcoder` converts parameter bits from one rate to another without audio in
between: the source frame is decoded to its MBE model with the library's own parameter
decoder and requantised for the target with the target decoder in the loop, so the receiver
hears what the source receiver would have heard, less one requantisation.

```c
mbe_transcoder* t = mbe_transcoderCreate(MBE_RATE_AMBE3600x2450, MBE_RATE_AMBE3600x2400);

/* per frame: DMR bits in, D-STAR bits out */
char dmr[72], fr[4][24], d_in[49], d_out[49], dstar[72];
mbe_process_result r;
mbe_ambeFrameFromBits(MBE_LAYOUT_DMR, dmr, fr);
mbe_decodeAmbe3600x2450Frame(fr, d_in, &r);              /* FEC only, no synthesis */
int kind = mbe_transcoderProcess(t, d_in, d_out);         /* MBE_TRANSCODE_VOICE / TONE / REPEAT */
mbe_encodeAmbe3600x2400Frame(d_out, fr);
mbe_ambeFrameToBits(MBE_LAYOUT_DSTAR, fr, dstar);

mbe_transcoderDestroy(t);
```

Any rate to any rate is accepted. Tone frames are converted between the AMBE rates (a DTMF
frame from a DMR radio becomes the D-STAR DTMF frame the AMBE-3000R itself would produce); tones
the target cannot carry, erasures and invalid frames repeat the previous model
(`MBE_TRANSCODE_REPEAT`). The gain is shifted between AMBE+2 and D-STAR by the amounts
measured on the AMBE-3000R so a bridged call keeps its loudness on real radios
([transcoder.md](transcoder.md)). Transcoding costs 6-15 us per frame.

## 6. Bit-level utilities

For framing layers that need them: Golay(23,12) / (24,12) and Hamming(15,11) in both mbelib's
`char` array form and an integer-codeword form, the PN scrambler (`mbe_pnSequence`), the
interleavers for every layout (`mbe_ambeFrameFromBits` and friends work in both directions,
hard and soft), `mbe_encodeAmbe3600x2400Frame` / `...2450Frame` / `mbe_encodeImbe7200x4400Frame`
to build FEC-protected frames from parameter bits, YSF V/D 2 and AMBE-3000R raw byte
conversions, and `mbe_unpackAmbe2450Fields` / `...2400Fields` to get at the nine quantiser
indices of an AMBE parameter word. Every conversion has been checked against AMBE-3000R captures
and dsdcc's tables ([bit-order.md](bit-order.md)).

## 7. Levels, quality and speed

The headline scores (PESQ and ESTOI for the decoders and encoders, against the AMBE-3000R,
DVSI's software and mbelib-neo) are in [README.md](../README.md), "How it compares". Details and behaviour under
errors: [decoder-quality.md](decoder-quality.md), [encoder-quality.md](encoder-quality.md) and,
for the transcoder, [transcoder.md](transcoder.md).

The decoders play speech and tone frames at the AMBE-3000R's level, which is DVSI's decoder's
(within 0.2 dB); the transcoder keeps the level within 0.3 dB.

Speed on one desktop core: encoder about 300 us per frame (65x real time), decoder 12 us
(1600x; 25-49 us from soft bits), transcoder 6-15 us ([benchmarks.md](benchmarks.md)).

## 8. Threads, determinism and compatibility notes

- Streams are independent. Two threads may run two streams at once; one stream must not be
  used from two threads at the same time.
- A thread that decoded holds a small FFT plan (about 12 KB) for the unvoiced synthesis. Call
  `mbe_freeThreadResources()` on the thread before it ends (an application that starts and stops
  decoder threads otherwise leaves one plan per thread); a later decode on it allocates a new one.
- The comfort-noise and unvoiced-noise generators are thread-local (inherited from mbelib-neo
  for bit parity). Two streams interleaved on one thread therefore get slightly different
  noise fill than each would alone (95-98 % identical frames); `mbe_setThreadRngSeed` makes a
  thread's sequence reproducible.
- Given the same input, output is deterministic on one platform. Bit-exact output across
  platforms and compilers is not guaranteed for the encoder (float arithmetic, SIMD); in the
  `neo-compat` configuration the decoder matches mbelib-neo built without fast-math to float rounding.
- Differences from mbelib-neo, all switchable off at configure time (`MBEVOCODER_CHIP_MEASURED`):
  D-STAR fundamental mapping, harmonic count, prediction coefficient and memoryless gain; tone
  amplitude laws and D-STAR dual tones ([tone-frames.md](tone-frames.md)); mixed-excitation
  voicing, the AMBE+2 unvoiced band grid, the synthesis' amplitude ramps, phase smoothing and output
  high-pass, the AMBE+2 silence frames, the lost-frame, uncorrectable-C0 and soft-decision rules,
  16-bit rounding, AMBE+2 erasures and unusable tone frames as repeats, the AMBE-3000R's AMBE+2
  tone check and tone replay, its D-STAR lost-frame codes (b0 125, 127), error-rate mute,
  channel-error handling (uncorrectable C0 words, flagged `MBE_PROCESS_FLAG_C0_UNCORRECTABLE`), tone
  crossfades, tones through lost frames, voicing rule and b0 124 model, the AMBE+2 pitch law, the high-band shelf, repeats that keep the frame's error rate and
  noise state running, and the output level ([decoder-quality.md](decoder-quality.md)); the AMBE+2
  order of b3/b4's low bits ([bit-order.md](bit-order.md)); and the AMBE frame decoders and the IMBE 7200 frame
  decoders counting every corrected Golay bit, parity included, as TIA-102.BABA(-1) says (and JMBE
  for IMBE, mbelib-neo 2.6 for the AMBE rates), which changes `c0_errors` / `total_errors` and the repeat
  and mute decisions on damaged frames ([decoder-quality.md](decoder-quality.md), "AMBE+2 error
  counts and muting", "IMBE: errors in the Golay parity bits"; `mbe_golay2312` and the
  `mbe_eccImbe*` functions keep mbelib's count, `mbe_eccAmbe3600x2450*` count full weight as
  mbelib-neo 2.6's do). With the option off the
  decoders give mbelib-neo 2.1's output to within float rounding.
- Versioning: `MBEVOCODER_VERSION` (`<mbevocoder/version.h>`; `MBELIB_VERSION` in
  `<mbelib-neo/version.h>` carries the same value) and `mbe_versionString()`; 3.x keeps every mbelib-neo 2.6
  symbol and signature except the TETRA ACELP decoder (`mbe_tetra*`, not provided), so code written
  for mbelib-neo recompiles unchanged (`tests/test_neo24_api.c`;
  mbelib-neo 2.4.3's frame classifier and DV data helpers give identical results, and its D-STAR
  examples build and run on this library). mbelib-neo 2.4's encoder functions (`mbe_ambe2450EncoderAlloc`,
  `mbe_encodeAmbe2450Parms` and the D-STAR and IMBE equivalents) run this library's encoders with
  their default options; their parameters lag the audio by 189 samples (239 for D-STAR) where
  mbelib-neo's lag about 80, and mbe_encoderCreate() gives the options and the front end. It is not
  binary-compatible: `mbe_parms` gained fields (mixed excitation, high-pass state, silence
  frame, the last AMBE+2 tone played, D-STAR's mute measure, held tone and prediction history), so anything that allocates one must be compiled against this header; substituting
  the library under a binary built for mbelib-neo would overrun its `mbe_parms`.
- Code written for the original mbelib 1.x compiles unchanged against `<mbelib/mbelib.h>`,
  which gives the decoder functions their mbelib 1.x form (mbelib-neo 2.x changed 15 of
  mbelib 1.x's 49 functions; 9 more differ only in ways callers do not see, such as a `void`
  return becoming a status `int`). With it:
  - **The 14 `mbe_process*` functions** take `int* errs, int* errs2, char* err_str` and a
    trailing `int uvquality`, as in mbelib 1.x. The frame functions set `*errs` to the C0
    errors and `*errs2` to the total. The data functions read them as the FEC stage's counts.
    `err_str` gets one `=` per error and `E` / `T` / `R` / `M` for erasure / tone / repeat /
    mute; give it 64 bytes (`MBE_V1_ERR_STR_SIZE`), as DSD does.
  - **`mbe_synthesizeSpeech` / `mbe_synthesizeSpeechf`** take `uvquality`, which is accepted
    and ignored (the unvoiced synthesis is mbelib-neo's FFT method).
  - **`mbe_printVersion`** exists.
  - **Exported names:** the header maps those names onto exported `mbe1_*` wrappers with
    macros, so one translation unit uses one decoder API or the other. The audio and counts
    are identical to the mbelib-neo API's (`tests/test_mbelib1_api.c`).
  - **Not provided:** the `mbe_parms` fields `repeat` (now `repeatCount`) and `un`, and the
    bare header name `<mbelib.h>` (add `<prefix>/include/mbelib` to the include path for
    sources that include it).

  To port a 1.x caller to the mbelib-neo API instead, replace the three arguments with a result
  struct (`c0_errors` / `total_errors` are `errs` / `errs2`, `mbe_formatProcessResult()` gives
  `err_str`) and delete `uvquality`; dsdcc's `DSDCC_MBELIB_NEO` branch is a worked example.

## 9. Command-line tools

Built with `MBEVOCODER_BUILD_TOOLS`:

- `mbe-cli encode --rate ambe2450|dstar|raw2450|imbe4400|imbe7200 in.wav out.dvsi` (with
  `--no-tone-detect`, `--dtx`, `--noise-suppress`, `--echo-cancel far.wav`, `--flags f.txt`,
  `--fe-out fe.wav` for the front end),
  `mbe-cli decode --rate ... in.dvsi out.wav`, `mbe-cli transcode --from RATE --to RATE`,
  `mbe-cli tone --rate ... --id N --amp A`, plus `parms` and `frames` for inspecting and
  synthesising frames by quantiser index and `frbits` (mbelib's frame arrays as text, for
  feeding other mbelib builds; AMBE rates and `imbe7200`), with `frpack` its inverse, and `dpack` /
  `dunpack` (parameter bits as text <-> channel bytes, for probe streams). `decode` takes `--layout`,
  `--lsb-first` / `--msb-first`, `--errors` (per-frame error counts and flags), `--lost
  frames.txt` and `--gain X` (default 1, the library's level); `parms --rate imbe7200` prints the decoded model. The `.dvsi` files are the AMBE-3000R channel-packet
  format (9, 7, 11 or 18 bytes per frame).
- `ambe3000 --port COM3 --rate dstar|ambe2450|raw2450 encode|decode|loopback ...` drives an
  AMBE-3000R dongle as a reference encoder and decoder.
- `tools/quality/*.py` score the library against the AMBE-3000R and probe the AMBE-3000R's behaviour;
  `tools/corpus/*.py` assemble the test corpus.

## Further reading

[bit-order.md](bit-order.md) (frame and layout conventions), [decoder-quality.md](decoder-quality.md),
[encoder-quality.md](encoder-quality.md), [tone-frames.md](tone-frames.md), [frontend.md](frontend.md),
[transcoder.md](transcoder.md), [benchmarks.md](benchmarks.md); attributions in `NOTICE.md` at the repository root.
