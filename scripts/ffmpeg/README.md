# PS5 H.264 slice capacity repair

Base: FFmpeg n9.0.1 with the original vcpkg 9.0.1#1 patches. FFmpeg remains LGPL-2.1-or-later in the recorded configuration; this patch does not enable GPL/nonfree/external codec features.

The H.264 D3D12 backend uses `MAX_SLICES` from `libavcodec/h264dec.h` for its fixed slice-control array. PS5 1080p H.264 supplied 68 slices per picture during the regression, exceeding the original 32. `ps5-h264-slices.patch` increases capacity to 256, a power of two required by the internal ring indexing. This does not re-encode the stream or change bitrate, resolution or GPU decoding.

1. Copy the matching vcpkg-patched source tree to a new external directory; do not mix it with another vcpkg build currently in progress.
2. Apply `git apply /path/to/ps5-h264-slices.patch` from that source directory. Verify `h264dec.h` has `MAX_SLICES 256`.
3. Run `prepare-slice-build.py` with explicit `--reference-bin`, `--source`, `--build`, `--prefix`, `--msys`, `--msvc-bin`, `--nasm` and `--output` paths. Reference-bin is the existing compatible FFmpeg DLL directory. MSYS is its installation root; MSVC-bin is the VS x64 compiler directory; nasm is the assembler directory. The helper queries the existing DLL's LGPL configuration and only changes installation/archive-tool paths.
4. In a VS x64 developer environment run MSYS `bash.exe <output-script>`. The script puts MSVC first in PATH so MSYS `link.exe` cannot replace the MSVC linker. It performs configure, `make -j8`, and `make install` into the separate prefix.
5. Test the rebuilt DLLs before replacing local development dependencies. Preserve the baseline DLLs and record all five rebuilt DLL SHA-256 values, the patched header SHA-256, patch SHA-256 and unchanged configuration in `share/ffmpeg/veyra-local-build.json`. Publisher scripts include/validate this additional provenance; it is not a player runtime DLL lock.
6. Build and run `veyra_hw_import_image_tests <1080p H.264 file> <image-prefix>`, HDR color tests, and the opt-in `veyra_ps5_hw_image_tests --last-paired-ps5 <image-prefix>`. The latter briefly connects the last DPAPI-saved PS5 and compares software/hardware decoding of the **same** compressed AU; never package its images or private logs.

For a later release, pass the actual Veyra-patched source directory to `scripts/package-ffmpeg-source.py`; the stock vcpkg source directory must be rejected when the local-build record is present. Include the resulting corresponding-source archive, original licenses, original vcpkg SPDX (baseline provenance), and the extra Veyra build record. No FFmpeg DLL or copied dependency source is committed to Veyra Git.

For the 2026-10-09 compressed-subtitle repair, additionally pass `--zlib-prefix`
to `prepare-slice-build.py`. This changes only `--disable-zlib` to
`--enable-zlib`, using the existing static zlib 1.3.2 built with `/MD` and its
pkg-config metadata. Preserve dav1d's `PKG_CONFIG_PATH` in the child process.
The original MinGW pkg-config corrupts non-ASCII include paths on this machine;
an explicitly supplied E-drive junction alias is preserved by the helper. All
physical build/install/temp outputs remain under `E:/项目/Veyra/`.
Supply `--zlib-source` when packaging the corresponding source, and include
`share/zlib` notices/provenance. The baseline vcpkg FFmpeg SPDX identifies the
base port; the new `veyra-local-build.json` identifies the actual rebuilt DLLs.
