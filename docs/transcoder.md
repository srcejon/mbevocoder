# Parameter-domain transcoder

`mbe_transcoder` converts vocoder parameter bits between rates without synthesising and
re-analysing audio: the source frame goes through the library's own parameter decoder
(`mbe_decodeAmbe2450Parms`, `mbe_decodeAmbe2400Parms`, `mbe_decodeImbe4400Parms`), so the
model is exactly what a receiver would hear, and is requantised for the target rate with
the target decoder in the closed loop, as the speech encoder does. Any rate to any rate;
between the two 2450 rates, from D-STAR to D-STAR and between the two IMBE rates voice and
tone frames are copied unchanged (the return value still classifies them) and erasures are replaced by a repeat
frame like any other rate pair, so the output is always a usable frame.

```c
mbe_transcoder* t = mbe_transcoderCreate(MBE_RATE_AMBE3600x2450, MBE_RATE_AMBE3600x2400);
char d_in[49], d_out[49];
mbe_decodeAmbe3600x2450Frame(fr_dmr, d_in, &result);     /* FEC decode as usual */
int kind = mbe_transcoderProcess(t, d_in, d_out);         /* MBE_TRANSCODE_VOICE / TONE / REPEAT */
mbe_encodeAmbe3600x2400Frame(d_out, fr_dstar);           /* FEC encode for the target */
```

`mbe-cli transcode --from RATE --to RATE in.dvsi out.dvsi` does the same on frame files.

## How the model is carried across

- **Fundamental**: nearest target index in the log domain; the source harmonics are
  resampled (log amplitude interpolation, nearest voicing) onto the target harmonic grid.
- **Voicing**: when the source decoder produced the AMBE-3000R's measured mixed-excitation
  fractions (`mbe_parms.Vmix`, MBEVOCODER_CHIP_MEASURED), the target voicing index is the row
  whose measured rendering is nearest to them (energy weighted, all rows of the target's
  table except the AMBE+2 transient rows 20, 24, 25 and 28, which render noise bursts), and
  the chosen row's level offsets are taken out of the amplitude targets so the target
  decoder renders the source's levels. Re-quantising a decoded AMBE+2 model to AMBE+2 this
  way reproduces 87 % of the AMBE-3000R's frames index for index (74 % with binary voicing
  and no offset compensation) and its decode to 0.8 dB LSD (6.0 dB); across rates it is
  neutral within the noise (ten files, both directions: +-0.02 MOS, +-0.05 dB), see
  [encoder-quality.md](encoder-quality.md), "Mixed frames and the voicing index".
- **Amplitudes**: the decoders' `Ml` (what the synthesis consumes) is the common domain. The
  AMBE quantiser's gain works in mean(log2 Ml) + 0.5 log2 L (the decoders reconstruct
  mean(log2 Ml) = gamma - 0.5 log2 L), and unvoiced harmonics have the decoder's
  0.2046/sqrt(w0) scale removed before quantising; getting either wrong costs 12 dB.
- **IMBE target**: the Yazev quantiser's analysis amplitude is 4 Ml for both voicing classes
  (the AMBE quantiser's unvoiced offset and the decoder's unvoiced scale cancel), minus
  0.9 dB measured; `imbe_encode_model` (added to the vendored vocoder) quantises from the
  model with the same amplitude prediction state as the speech path.
- **Prediction state**: the source decoder's previous-frame state advances as its process
  function would (`mbe_moveMbeParms`), the target quantiser keeps its own decoder in the loop.
  An AMBE+2 silence frame (DTX) is rendered from the last voice model and does not become
  the source state, as in the decoder (the AMBE-3000R's freeze, [frontend.md](frontend.md));
  it transcodes as a quiet unvoiced voice frame, which the target's quantiser does advance
  on, as the target decoder will. Tone frames touch neither side's state.
- **Erasures / invalid frames** repeat the previous model (`MBE_TRANSCODE_REPEAT`, a quiet
  unvoiced frame if nothing came before). D-STAR silence frames become quiet unvoiced frames.
- **Tones** are mapped between the AMBE rates with `mbe_toneIdToDstar` /
  `mbe_toneIdFromDstar` and the amplitude conversion in [tone-frames.md](tone-frames.md)
  (the AMBE-3000R's DTMF frames transcode to its own frames on the other rate bit for bit). IMBE
  has no tone frames and D-STAR has no KNOX tones: those return `MBE_TRANSCODE_REPEAT`.

## Level between the AMBE rates

Our decoders are consistent between rates (transcoded frames decode within 0.3 dB of the
source with either decoder), and so, nearly, are the AMBE-3000R's: AMBE-3000R frames transcoded
and decoded by the AMBE-3000R come out +0.3 dB (AMBE+2 to D-STAR) and -0.45 dB (D-STAR to AMBE+2)
against the AMBE-3000R's decode of the originals (24 files). The transcoder shifts the gain by
those amounts so a bridged call keeps its loudness on real radios. The IMBE decoder is not on the AMBE-3000R, so no shift is applied for IMBE.

## Results

Simulation, AMBE-3000R-encoded AMBE+2 goldens (two files), our decoders, LSD against our decode
of the source frames:

| chain | LSD | level |
|---|---|---|
| AMBE+2 to D-STAR | 6.3 dB | -0.3 dB (by design, see above) |
| AMBE+2 to IMBE | 7.2 to 7.5 dB | +0.2 to +0.5 dB |
| AMBE+2 to D-STAR to AMBE+2 | 5.7 to 6.6 dB | +0.2 dB |
| AMBE+2 to IMBE to AMBE+2 | 6.7 to 6.8 dB | +0.6 dB |

For scale, a single codec pass scores about 8 dB against the original speech, and the
double-transcoded audio scores 8.3 dB against the original, the same as one pass.

Hardware (`tools/quality/hil_transcode.py`, 70 speech files, `corpus/baselines/transcode_hil.json`): AMBE-3000R frames transcoded by mbevocoder, decoded by the AMBE-3000R, against the
AMBE-3000R's decode of the original frames and against the original speech (PESQ and ESTOI as
in [README.md](../README.md), higher is better; LSD lower):

| direction | vs the AMBE-3000R's decode of the source: LSD / level / PESQ / ESTOI | vs the original speech: LSD / PESQ / ESTOI | AMBE-3000R's own encode + decode at the target rate, PESQ |
|---|---|---|---|
| AMBE+2 to D-STAR | 6.6 dB / -0.28 dB / 3.86 / 0.928 | 8.2 dB / 2.81 / 0.774 | 3.00 |
| D-STAR to AMBE+2 | 6.0 dB / -0.05 dB / 4.02 / 0.937 | 8.2 dB / 2.88 / 0.776 | 2.94 |

Read across: a bridged call (one AMBE-3000R encode, our transcode, one AMBE-3000R decode)
scores 2.81 / 2.88 MOS against the original where a single AMBE-3000R codec pass at the
target rate scores 3.00 / 2.94: the second vocoder pass costs about a tenth to a fifth of a MOS, and
the transcoded audio is at 3.86 / 4.02 MOS against the audio it was made from.

## Cost

About 6-8 us per frame for AMBE to AMBE and IMBE to AMBE, 15 us to IMBE (single thread,
`mbevocoder-bench`, [benchmarks.md](benchmarks.md); the transcoder's decode step uses the decoder's model, not its
synthesis), against about 0.3 ms for the speech encoder: a bridge can run thousands of channels
per core.
