// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone comparison adapter; never linked into mbelib.
// Input is headerless little-endian signed 16-bit 8 kHz mono PCM.
// Output is 11-byte IMBE parameter frames: u0..u7, MSB first.
use blip25_vocoder::vocoder::{Rate, Vocoder};
use std::{env, fs};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() != 3 { return Err("usage: reference-imbe input.s16le output.imbe4400".into()); }
    let data = fs::read(&args[1])?;
    if data.is_empty() || data.len() % 2 != 0 { return Err("PCM must contain complete i16 samples".into()); }
    let pcm: Vec<i16> = data.chunks_exact(2).map(|s| i16::from_le_bytes([s[0], s[1]])).collect();
    let mut codec = Vocoder::new(Rate::FullRate4400x4400);
    let mut output = Vec::new();
    let widths = [12usize, 12, 12, 12, 11, 11, 11, 7];
    // Pad the last partial input frame, then flush with four silent frames, as mbe-cli does.
    for n in 0..(pcm.len().div_ceil(160) + 4) {
        let mut frame = [0i16; 160];
        let start = n * 160;
        if start < pcm.len() {
            let count = (pcm.len() - start).min(160);
            frame[..count].copy_from_slice(&pcm[start..start + count]);
        }
        let words = codec.encode_info(&frame)?;
        if words.len() != 8 { return Err("expected eight full-rate vectors".into()); }
        let mut bytes = [0u8; 11];
        let mut bit = 0usize;
        for (word, width) in words.iter().zip(widths) {
            if (*word as usize) >= (1usize << width) { return Err("vector exceeds field width".into()); }
            for shift in (0..width).rev() {
                bytes[bit / 8] |= (((word >> shift) & 1) as u8) << (7 - bit % 8);
                bit += 1;
            }
        }
        output.extend_from_slice(&bytes);
    }
    fs::write(&args[2], output)?;
    Ok(())
}
