# Independent IMBE comparison adapter

This standalone test adapter uses OpenBLIP25 `blip25-vocoder` **1.0.0**, pinned in
Cargo.toml and Cargo.lock. It is not a dependency of the mbevocoder library or CLI.
Its software output is an independent comparison, not a certified hardware reference.
Project description and provenance: <https://github.com/OpenBLIP25/blip25-vocoder>.

`reference-imbe input.s16le output.imbe4400` reads signed 16-bit little-endian mono
PCM at 8 kHz, processes 160 samples at a time, and writes eight prioritised IMBE
code vectors as 88 packed bits per frame. The final partial frame is zero padded;
four silent frames flush the encoder, matching the comparison CLI's convention.
No preprocessing or optional decoder enhancement is added by the adapter.

`reference-pack input.imbe4400 output.imbe7200` applies mbevocoder's existing FEC and
P25 layout. The comparison script checks this adapter by requiring its output on
mbevocoder's raw frames to equal mbevocoder's directly encoded P25 frames byte for byte.
This shares only channel coding/layout; the reference's analysis and quantisation
come from the independent encoder. Both decoders consume identical packed frames.

From the repository root on Windows, with Rust, MSVC, Python quality dependencies,
and the existing JMBE checkout available:

```powershell
cargo build --release --locked --manifest-path tools/quality/reference_imbe/Cargo.toml --target-dir build/audio-quality-followup/reference-imbe-build
cmake -S tools/quality/reference_imbe -B build/audio-quality-followup/pack-build -G "Visual Studio 18 2026" -A x64
cmake --build build/audio-quality-followup/pack-build --config Release
python tools/quality/imbe_reference_compare.py --jobs 4
```

The CMake adapter defaults to the existing `build/vs2026/Release/mbevocoder.lib` and
generated headers. Override `MBEVOCODER_LIBRARY` if the static library is elsewhere.
The Python runner accepts explicit encoder, packer, JMBE and output paths.
Metrics, source/frame hashes and executable hashes are written after each file.
Native perceptual scoring is serialised while independent subprocesses overlap.

See `docs/encoder-quality.md` for results and limitations.
