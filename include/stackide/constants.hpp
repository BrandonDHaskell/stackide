#pragma once

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>

namespace stackide {

inline constexpr int kWindowWidth = 1280;
inline constexpr int kWindowHeight = 800;

inline constexpr int kOffscreenWidth = 64;
inline constexpr int kOffscreenHeight = 64;

// Swapchain and offscreen target both use an sRGB-encoded format (decision 3
// in docs/buildouts/M1-textured-quad.md): the eventual glyph atlas needs
// linear-space alpha blending for antialiased edges, so this is load-bearing
// now rather than retrofitted once text rendering exists.
inline constexpr SDL_GPUTextureFormat kOffscreenColorFormat =
    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;

// The clear color is defined in 8-bit terms so the offscreen mode can assert
// exact byte values. Float form is derived, never the other way around.
inline constexpr std::uint8_t kClearR = 0x1A;
inline constexpr std::uint8_t kClearG = 0x1A;
inline constexpr std::uint8_t kClearB = 0x1F;
inline constexpr std::uint8_t kClearA = 0xFF;

// Drivers differ by one ULP in unorm rounding. Tolerate it, nothing more.
inline constexpr int kChannelTolerance = 1;

// CTest convention for "skipped" (see SKIP_RETURN_CODE).
inline constexpr int kExitSkip = 77;

inline constexpr Uint64 kDefaultTimeoutMs = 10'000;

// An _SRGB texture format auto-encodes a shader's linear float output to its
// sRGB-encoded byte storage. To keep the clear assertion's expected bytes
// (kClearR/G/B/A) exactly as they are, the clear color fed to SDL has to be
// the *decoded* linear value that round-trips back through that encode.
inline float srgb_decode(std::uint8_t v) noexcept {
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline SDL_FColor clear_color() noexcept {
    return SDL_FColor{srgb_decode(kClearR), srgb_decode(kClearG), srgb_decode(kClearB),
                       srgb_decode(kClearA)};
}

} // namespace stackide
