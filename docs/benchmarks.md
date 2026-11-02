# Benchmarks

`mbevocoder-bench` (built with `MBEVOCODER_BUILD_BENCH=ON`, run by `ctest -L bench`) times every
encoder, decoder and transcoder path on 60 s of the speech corpus (synthetic speech if no corpus
directory is given), single threaded, and writes `bench.json`.

MSVC 2026 x64 /O2, Windows 11, 2026-10-21, the default build, and for comparison the same build
with the AMBE-3000R-measured decoder (the `neo-compat` synthesis), the floating-point analysis and
the weighted D-STAR quantiser off; the faster of two runs. Encoder timings vary by about 20 % from
run to run.

| path | default: us per frame | x real time | quality options off |
|---|---|---|---|
| encode AMBE+2 3600x2450 (analysis + quantise + FEC) | 311 | 64 | 219 |
| encode AMBE+2 2450 parameters only | 300 | 67 | 216 |
| encode D-STAR 3600x2400 | 316 | 63 | 214 |
| encode IMBE 7200x4400 | 306 | 65 | 198 |
| encode IMBE 4400 parameters only | 306 | 65 | 203 |
| decode AMBE+2 3600x2450 (FEC + parameters + synthesis) | 11.9 | 1681 | 13.1 |
| decode AMBE+2 2450 parameters + synthesis | 12.5 | 1600 | 12.3 |
| decode D-STAR 3600x2400 | 12.3 | 1626 | 11.6 |
| decode IMBE 7200x4400 | 11.9 | 1681 | 11.7 |
| decode AMBE+2 3600x2450 soft bits | 24.5 | 816 | 23.7 |
| decode IMBE 7200x4400 soft bits | 49.0 | 408 | 42.7 |
| transcode AMBE+2 to D-STAR (parameters) | 7.8 | 2564 | 6.2 |
| transcode AMBE+2 to IMBE (parameters) | 14.5 | 1379 | 14.4 |
| transcode IMBE to AMBE+2 (parameters) | 5.7 | 3509 | 5.3 |
| tone frame AMBE+2 (build + FEC) | 0.1 | 200000 | 0.2 |

The soft-decision decoders score every Golay and Hamming codeword against the received bits; each
candidate costs three table lookups on its difference word (per-call cost tables per byte, the
codewords tabulated once), with the same candidates, order and tie-breaks as scoring bit by bit
(identical output on 90 000 random words), where the bit loop took 644 us per soft AMBE+2 frame and
1629 us per soft IMBE frame.

The encoder's input gain (`MBEVOCODER_ENCODER_INPUT_GAIN`) costs nothing measurable: on
and off run alternately on the same machine agree within the run-to-run spread on every encoder.

What keeps the default build fast: the measured voiced synthesis renders every
harmonic phase-continuously with a geometric amplitude ramp, computed by recurrence in double
precision rather than `powf` and `cosf` per sample (the decoder runs as fast as mbelib-neo's
synthesis); the floating-point analysis' band energies use a 2048-point FFT, and IMBE does not
compute them; the pitch correlation's interpolation weights are hoisted out of its loop.

The encoders are dominated by the fixed-point analysis (pitch estimation with its look-back /
look-ahead search, FFT, voicing); the AMBE quantiser adds under 10 us and the FEC is negligible. At
about 65x real time on one core there is no case for SIMD in the encoder for the intended
applications (a few streams per host, or one on a phone); the decoder's synthesis carries
mbelib-neo's runtime-dispatched SIMD paths (`MBEVOCODER_ENABLE_SIMD`). Timings scale with the corpus,
not the frame content.
