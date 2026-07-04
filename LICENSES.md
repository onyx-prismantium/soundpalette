# Third-party licenses

All dependencies are permissively licensed and statically linked. Pins and provenance in
`DEPENDENCIES.md`; FetchContent declarations in `cmake/Dependencies.cmake`; vendored sources
under `extern/`.

| Component | Version/pin | License | Role |
|---|---|---|---|
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 (vendored) | MIT-0 or public domain (dual) | Audio decode (WAV/FLAC/MP3) + playback engine |
| [stb_vorbis](https://github.com/nothings/stb) | commit `31c1ad3` (vendored) | MIT or public domain (dual) | OGG Vorbis decoding via miniaudio |
| [stb_image_write](https://github.com/nothings/stb) | commit `31c1ad3` (vendored) | MIT or public domain (dual) | PNG output for the GUI `--smoke` capture |
| [libebur128](https://github.com/jiixyj/libebur128) | v1.2.6 | MIT | EBU R128 integrated loudness + true peak |
| [KissFFT](https://github.com/mborgerding/kissfft) | 131.1.0 | BSD-3-Clause | Real FFT for STFT features |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | JSON serialization |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) | v1.92.8-docking | MIT | GUI |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | zlib | Window/input |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | v1.2.1 | zlib | Native folder/save dialogs |
| [doctest](https://github.com/doctest/doctest) | v2.4.12 | MIT | Unit tests (dev-only, not shipped) |
| [crypto-algorithms SHA-256](https://github.com/B-Con/crypto-algorithms) (Brad Conte) | commit `cfbde48` (vendored) | Public domain | Per-file SHA-256 in manifests |
