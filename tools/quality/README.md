# Quality scripts: what they need

These scripts made the measurements in `docs/` and `corpus/baselines/`. They are research tools,
not tests: most of them expect a Windows build and inputs that a fresh clone does not have, and they
stop with a missing-file error when one is absent. Run them from the repository root with Python 3
and `pip install numpy scipy pesq pystoi`.

| needs | where the scripts look | how to get it |
|---|---|---|
| `mbe-cli`, `ambe3000` | `build/vs2026/tools/.../Release/*.exe` (`compare.py` and `hil_encoder.py` also try `build/release`, `build/debug`) | `cmake --preset vs2026` and build; on other platforms edit the path or pass `--cli` where a script has it |
| the corpus audio and the AMBE-3000R's decodes | `testdata/` | `-DMBEVOCODER_FETCH_TESTDATA=ON` (once the test-data release is published), or `tools/corpus/*.py` |
| an AMBE-3000R (ThumbDV / DV3000U) | `--port COM3` by default | the `hil_*`, `probe_*` (except `--ours`), `dvsi_encoders.py`, `encoder_errors.py` and `neo_encoders.py` scripts decode on it |
| DVSI's test vectors | `build/quality/dvsi` | mbelib-neo's `tools/quality/fetch_dvsi_vectors.py`; DVSI's, kept local, never committed or redistributed |
| mbelib-neo's decoder and encoders | `build/dvsi_harness/Release/neodec.exe`, `neoenc.exe` | configure `tools/quality/dvsi_harness` against an mbelib-neo checkout |
| mbelib 1.x | `build/vs2026/tools/mbelib1-decode/...` | `-DMBELIB1_SOURCE_DIR=<checkout>` |
| JMBE | `build/jmbe` | a JDK and a built JMBE 1.0.9 checkout (`jmbe_crosscheck.py --help`) |
| OpenBLIP25's IMBE frames | `build/audio-quality-followup/imbe` | `reference_imbe/README.md` |
| experiment builds | `build/exp` (`synth_experiment.py --build`), `build/devtools` | configure a second build tree with `-DMBEVOCODER_EXTRA_DEFINITIONS=MBE_EXP_...=value`; the `MBE_EXP_*` knobs are in `src/decoder/mbelib.c`, `src/encoder/encoder.cpp` and `src/analysis/imbe_vocoder/v_uv_det.cc` |

Working files go to `build/quality`, `build/probe*` and `build/listening*`; the baselines they
write (`corpus/baselines/*.json`) are the recorded results, so rerun a script only to replace them.
