#pragma once
// DLSS Frame Generation performance mode (NVIDIA App 11.0.9 "Model C", beta).
//
// NVIDIA delivers it as DLSS-G 310.9.16 (driver OTA download, nvapp_override
// build). That runtime adds "FG Lite" candidate blending behind the
// create-time parameter DLSSG.LiteCandidateBlendingEnabled; the 310.9.1 copy
// Veyra keeps as its default ignores the parameter. Measured 2026-10-14 on an
// RTX 5070 (2K60 NV12 + NR): DLSS-G GPU time P95 4X 5.6 -> 2.8 ms, 6X 8.0 ->
// 3.3 ms; setting it per evaluate has no effect, only at create.
//
// The runtime ships beside the default one in runtime/experimental/dlssg-lite
// (user decision 2026-10-14) and is used only while the mode is on: NGX then
// searches that directory before the default runtime directory. RTX 40/30
// keep 310.9.1, whose audited multi-frame patches do not apply to 310.9.16.
#include <filesystem>
#include <system_error>

namespace veyra::ngx {

inline constexpr const char* kDlssgLiteParameter = "DLSSG.LiteCandidateBlendingEnabled";

inline std::filesystem::path dlssgLiteDirectory(const std::filesystem::path& runtimeDirectory) {
    return runtimeDirectory / L"dlssg-lite";
}

inline bool dlssgLiteRuntimePresent(const std::filesystem::path& runtimeDirectory) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dlssgLiteDirectory(runtimeDirectory) / L"nvngx_dlssg.dll", ec);
}

} // namespace veyra::ngx
