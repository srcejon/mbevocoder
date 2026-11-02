# Tone frames

AMBE carries DTMF, call-progress, KNOX and single tones as dedicated frames instead of
voice parameters. mbelib decoded them (from JMBE's reading of the format); mbevocoder also
encodes them, and both directions are checked against the AMBE-3000R with its tone detector on
(`ambe3000 --ecmode 0x1000 encode ...`, `PKT_ECMODE` bit 12, which the tool's default configuration
clears; [frontend.md](frontend.md)). The captures are in `tests/data/tones` (manifest.json lists
tones, indices and amplitudes) and gate `test_tones_transcode`.

## API

```c
int mbe_toneFrequencies(int tone_id, float* f1, float* f2);        /* 1 if valid */
int mbe_toneIdFromFrequencies(float f1, float f2);                  /* nearest id or -1 */
int mbe_toneIdToDstar(int tone_id);  int mbe_toneIdFromDstar(int dstar_id);
int mbe_encodeAmbe2450ToneData(int tone_id, int amplitude, char ambe_d[49]);
int mbe_encodeAmbe2400ToneData(int tone_id, int amplitude, char ambe_d[49]);
int mbe_encoderTone(mbe_encoder*, int tone_id, int amplitude, mbe_layout, char* bits);
int mbe_encoderToneDvsi(mbe_encoder*, int tone_id, int amplitude, uint8_t* bytes);
```

`tone_id` always uses the AMBE+2 numbering (mbelib's table, confirmed on the AMBE-3000R for every
entry): 5 = 156.25 Hz, 6 = 187.5 Hz, 7..122 = 31.25 Hz x id, 128..143 DTMF 0-9 A-D * #
(128 = 0, 129 = 1, ..., 137 = 9, 138..141 = A..D, 142 = *, 143 = #), 144..159 KNOX,
160..163 dial / ring / busy / reorder. `amplitude` is the 7-bit AD field, 0..127; the AMBE-3000R's
encoder emits about 127 for a full-scale sine and 0.71 dB less per step (112 at -10.5 dBFS,
84 at -30.5 dBFS). `MBE_TONE_AMPLITUDE_DEFAULT` (103) is mbelib's nominal D-STAR level.
Tone frames do not touch the voice prediction state on either side, so they can be
interleaved with voice frames from the same encoder.

## AMBE+2 (3600x2450) layout

AMBE-3000R-verified for 39 tones and three amplitudes; `mbe_encodeAmbe2450ToneData` reproduces
every captured tone frame bit for bit (46 captures, 50 frames each):

| field | bits | content |
|---|---|---|
| u0 | 12 | `111111` then AD[6:1] |
| u1 | 12 | tone id, then id[7:4] |
| u2 | 11 | id[3:0], then id[7:1] |
| u3 | 14 | id[0], id, AD[0], `0000` |

The identifier is repeated four times (mbelib reads only the first copy and checks the u1
nibbles against each other). mbelib's parse of the third copy uses a bit of u2 that does not
exist; it is unused.

## D-STAR (3600x2400) layout

Also AMBE-3000R-verified. The decoder's reading of the index (b0 = 126 in d[0..5],d[48]; index
bits 7..5 through a 3-bit code in d[6..8], bit 4 in d[9], bits 3..2 in d[42],d[43], bits
1..0 in d[10],d[11]) matches the AMBE-3000R. The 8-bit volume in d[12..16],d[44],d[45],d[17]
that mbelib only parsed for debugging is real: the AMBE-3000R emits 2 AD - 17 on the AMBE+2
scale (208 at -10.5 dBFS, 235 at -0.9, 151 at -30.5) and renders 0.354 dB per step, a tone's peaks
reaching full scale at about 236.5; above that the law runs on and the tone clips (a single tone
at 255: -1.0 dB RMS), and a dual tone stops rising at about 238 (-2.6 dB RMS). d[24] is the C1 parity, filled in by the frame encoder.

D-STAR numbers dual tones differently and has fewer of them:

| D-STAR index | tone |
|---|---|
| 128..143 | DTMF, 128 + 4 x column + row over columns 1209/1336/1477/1633 Hz and rows 697/770/852/941 Hz (so DTMF 1 = 128, 0 = 135, * = 131, # = 139, A = 140, D = 143) |
| 144, 145, 146, 147 | dial, ring, busy, reorder |
| 148..163 | nothing: the AMBE-3000R plays a lost frame (decoder-quality.md "D-STAR special frames"), and its encoder codes KNOX tones as voice |

Only b0 126 carries a tone. The AMBE-3000R plays every b0 127 frame as a lost frame, whatever tone
it holds (the volume-0 dual tone 128 that some encoders send for silence included), and a b0 126
tone at volume 0 plays, silently, as a tone.

`mbe_toneIdToDstar` converts; KNOX tones return -1 and `mbe_encodeAmbe2400ToneData` returns
`MBE_STATUS_ENCODE_RANGE` for them (the transcoder repeats the last voice model instead).

## Decoder rendering (`MBEVOCODER_CHIP_MEASURED`, on)

- **Amplitude law.** The AMBE-3000R renders 0.71 dB per AD step with full scale at 127; mbelib
  scaled AD linearly (AD/127 of 1/7 full scale), up to 50 dB too loud for quiet tones and 17 dB too
  quiet for loud ones. Our decode of our tone frames is within 0.1 dB of the AMBE-3000R's for AMBE+2
  (AD 112, 84, 60 and dual tones) and within 0.5 dB for D-STAR.
- **Dual tones** carry two full-amplitude components (the AMBE-3000R's dual tone is 3 dB above a
  single tone of the same AD); mbelib halved them.
- **D-STAR volume** is used (mbelib rendered every D-STAR tone at AD 103) and **D-STAR dual
  tones** are rendered with the D-STAR numbering (mbelib produced comfort noise and reset the
  decoder for them). A D-STAR tone frame plays whenever C0 decodes, and one the decoder cannot
  render is a lost frame ([decoder-quality.md](decoder-quality.md), "D-STAR special frames").
  As on the AMBE-3000R, a tone crossfades in from the speech before it and out into the voice
  after it, and moves between two volumes, over 80 samples, and lost frames after a dual tone
  continue it to the repeat limit, after a single tone end it (decoder-quality.md, "D-STAR
  tones").
- **Which AMBE+2 tone frames play.** A frame whose u0 starts `111111` is a tone frame to the
  AMBE-3000R whatever its other bits (a silence frame never carries that header). It plays the tone
  only when all four copies of the ID agree and the ID is one it can play (Table 9's, not the
  zero-amplitude 255); any other tone frame, and one whose C0 is uncorrectable, is a lost frame. It
  does not gate on the error counts. The decoder does the same: damaged tone frames (1-10 random bit
  errors each, `probe_ambe2450_frames.py` part 3) play or not on exactly the frames the AMBE-3000R
  plays, where JMBE's check (one copy compared, total errors under 6) played 78 % of the frames with
  seven errors against its 9 %.
- **A lost frame after a tone replays the tone**: the AMBE-3000R repeats the frame last played, so
  after a tone the lost-frame schedule (three repeats at full level, the fourth faded, silence)
  plays that tone; after voice it repeats the voice model (`lastTone` in `mbe_parms`;
  [decoder-quality.md](decoder-quality.md), "AMBE+2 erasures and unusable tone frames").
  `mbe_classifyAmbe2450Frame` keeps mbelib-neo's classification (JMBE's consistency check, 255
  usable), which reports some frames the decoder plays as lost frames as tones.
- **Dual-tone frequencies** are the nominal ones. The AMBE-3000R plays a dual tone as two harmonics of
  one fundamental in a small ratio (17:12 for DTMF 0, 7:6 for ring-back), 0.1-0.5 % off the DTMF
  frequencies and up to 3.4 % off the call-progress ones; the ratio it picks follows no rule found,
  and tone detectors (+-1.5 % to +-3.5 %) prefer the nominal frequencies.

Speech decodes at the AMBE-3000R's level too, so tones and the speech around them keep a radio's
balance (mbelib-neo 2.1's decoders play speech 16 dB louder). Set
`MBEVOCODER_CHIP_MEASURED=OFF` (which also reverts every other AMBE-3000R-measured decoder change) for
mbelib's original rendering. `test_decoder_parity` skips the
waveform comparison for tone frames (flags and status are still compared).
