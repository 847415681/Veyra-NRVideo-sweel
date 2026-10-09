# FG flash pixel sampler

Diagnostic-only sources; not linked into the normal product or its CMake targets.
The generated external VideoPresenter copy records the final enhanced/generated
input and drawn swapchain buffer in the same command list, tracks the submitted
fence, and maps the bounded readback only at presenter close.

`FlashPixelProbe.h` implements the nine-point RGBA8/RGB10 sampler.
`FieldPixelProbe.h` adds frame identity, paired source/buffer events and close-time
CSV output. A black sample is evidence of black pixels, not proof of a bug.
XeSS-internal generated frames and panel scanout are not measured.

The exact tested build recipe, generated presenter and hashed input libraries
are preserved in `E:/项目/Veyra/reports/fg-source-flash-20261008` and the matching
diagnostic ZIP. Base product commit: `71723e0249a17764696796fd204a6eb69d6da276`.
Final GUI SHA256: `4c38d320585ff545dac231910fe7f64b71a2930af5e164f6c40fb32fce9804a1`.
See `docs/FG_SOURCE_FLASH_REPORT_2026-10-08.md` for tests and limitations.
