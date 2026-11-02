# Channel frame bit order

How the AMBE-3000R's channel packets, mbelib's frame arrays and the on-air layouts map onto each
other, established against an AMBE-3000R (firmware V120.E100.XXXX.C106.G514.R009.B0010411.C0020208)
with `tools/neo-decode`. For each rate exactly one of eight candidate mappings decodes every
AMBE-3000R frame with zero Golay corrections; every other mapping averages about three corrected
errors per frame, the random-bits floor. Checked on the corpus goldens: 30935 frames per rate, zero
errors, zero tone frames, no rejected frames.

| Rate | AMBE-3000R channel packet | Mapping to `ambe_fr[4][24]` |
|---|---|---|
| AMBE 3600x2400 (D-STAR, `PKT_RATEP 0130 0763 4000 0000 0000 0048`) | 72 bits, 9 bytes | Bit *i* of the D-STAR on-air voice frame is `byte[i/8] >> (i%8) & 1` (**LSB first**), then dsdcc's D-STAR tables: `ambe_fr[dW[i]][dX[i]] = bit_i`. |
| AMBE+2 3600x2450 (DMR, P25 Phase 2, YSF DN, NXDN; `PKT_RATET 33`) | 72 bits, 9 bytes | Dibit *d* of the DMR on-air voice frame is `byte[d/4] >> (6 - 2*(d%4)) & 3` (**MSB first**), then dsdcc's DMR tables: `ambe_fr[rW[d]][rX[d]] = dibit>>1`, `ambe_fr[rY[d]][rZ[d]] = dibit&1`. |
| AMBE+2 2450x2450 (`PKT_RATET 34`) | 49 bits, 7 bytes, MSB first, 7 zero pad bits at the end | `ambe_d[i]` sits at raw bit `dvsi49[i]`, the table dsdcc uses for YSF V/D type 2; `mbe_dvsiToAmbe2450Data`. The AMBE-3000R codes identical parameter bits at rates 33 and 34. |
| IMBE 7200x4400 (P25 Phase 1, DVSI's P25 test vectors) | 144 bits, 18 bytes, MSB first | The P25 Phase 1 on-air layout (`MBE_LAYOUT_P25P1`, DSD's iW/iX/iY/iZ). |

So the AMBE-3000R's "DVSI order" is each standard's on-air voice frame order; the only quirk is the
byte bit direction (D-STAR LSB first, DMR MSB first), as dsdcc's `DSDDstar::storeSymbolDV` and
`DSDDMR::storeSymbolDV` already build frames for SDRangel's hardware decoder feature.
`mbe_encoderEncodeDvsi` writes these packets, and `.dvsi` golden files feed the D-STAR and DMR frame
paths directly. P25 Phase 2 (Annex S) and NXDN use other interleaves over the same 72 bits, taken
from the specifications and checked against the DMR layout.

The AMBE-3000R's encode + decode delay is about two frames (D-STAR 34-38 ms, AMBE+2 39-46 ms by
envelope cross-correlation; the manual quotes 52 ms encoder plus 10 ms decoder); tests align by
cross-correlation. Golden captures carry four trailing silent frames (`--flush 4`).

## Frames and error counts

- **Frame reassembly is exact**: every golden frame decoded to `ambe_d[49]` and re-encoded with
  `mbe_encodeAmbe3600x2400Frame` / `...2450Frame` reproduces the AMBE-3000R's frame bit for bit
  (61870 frames).
- **D-STAR carries 48 data bits, not 49.** `ambe_d[24]` (frame cell `fr[2][10]`) is the overall
  parity bit of the 24-bit C1 word, scrambled like the rest of C1:
  `d[24] = parity(golay24(u1) ^ pn24(u0))`; mbelib's 2400 decoder never reads it, and `b8` has a zero
  LSB. OP25's D-STAR encoder agrees (Golay(24,12) on C0 and C1, 24 unprotected bits).
- **Error counts**: mbelib corrects only the data bits of a Golay word, passes the parity bits
  through, then flips the overall parity bit if the 24-bit word is odd. The mbelib-compatible entry
  points keep that convention and return the same parameter bits and counts as mbelib-neo 2.1.0 on
  all 61870 frames with 0-5 random flips; the integer API (`mbe_golay2312Decode` etc.) reports true
  corrected-bit counts. The IMBE 7200 frame decoders count every corrected Golay bit, as the
  standard does ([decoder-quality.md](decoder-quality.md), "IMBE: errors in the Golay parity bits").
- **Hamming**: both IMBE Hamming(15,11) generators (standard and ProVoice) are systematic with the
  parity in bits 3..0.
- mbelib-neo parity dumps: `testdata/neo/<rate>/<name>.neo` (`neo-decode --rate <rate> --dump`).

## AMBE+2: the order of b3 and b4's low bits

The AMBE-3000R splits the four bits `ambe_d[40..43]` between the PRBA indices differently from
mbelib and JMBE: b4's three low bits are `ambe_d[40], [41], [42]` (MSB first) and b3's lowest bit is
`ambe_d[43]`, where mbelib (and JMBE's u3 bits 5-8) put b3's in `ambe_d[40]` and b4's in
`ambe_d[41..43]`. The frames themselves are not in question (DSD's tables and JMBE's deinterleave
give the same `ambe_d`); only the split of those bits into fields differs. Codebook probes on the
AMBE-3000R at nine harmonic counts recover mbelib's PRBA58 rows exactly, under other indices
(sending low bits 0..7 plays rows 4,4,5,5,6,6,7,7 with b3's lowest bit 1), and PRBA24 rows with the
lowest bit taken from b4's. With `MBEVOCODER_CHIP_MEASURED` the decoder and
`mbe_pack/unpackAmbe2450Fields` use the AMBE-3000R's order, so both directions of every AMBE+2 stream
agree with DVSI hardware (decoder PESQ +0.12 on its frames, [decoder-quality.md](decoder-quality.md));
neo-compat keeps mbelib's. Tone frames use these bits for the tone index (their own layout, measured
bit for bit) and are unaffected; the AMBE+2 silence frame's bits are unchanged.

## The other rates, checked

- **D-STAR**: every pair of the 48 parameter bits swapped (1,128 variants) and every bit inverted, the
  AMBE-3000R's own frames of six files decoded by ours against its decode: no variant brings the two
  closer (best +0.019 dB on 2.46 dB; the same scan on AMBE+2 with mbelib's order finds exactly the
  pairs within `ambe_d[40..43]`). With the codebook probe recovering mbelib's D-STAR rows under their
  own indices, D-STAR's field order is the AMBE-3000R's.
- **IMBE**: our decoder and JMBE decode the same fundamental, harmonic count and voicing on every
  frame of six files and amplitudes within 0.08-0.15 dB rms (`jmbe_crosscheck.py --parms`); in
  OpenBLIP25's frames no swap of two of the 88 bits or inversion of one brings our decode closer to
  the speech or to JMBE's (a planted swap is found at -0.335 dB). DVSI's own P25 test vectors decode
  with our decoder as the 18-byte frames come (PESQ 2.94 against DVSI's 3.03, LSD 7.5 dB against
  DVSI's decode, [decoder-quality.md](decoder-quality.md), "DVSI's test vectors"): the order is
  DVSI's.
