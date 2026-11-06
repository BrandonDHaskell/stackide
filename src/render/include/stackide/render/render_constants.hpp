#pragma once

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>

namespace stackide
{

    inline constexpr int kWindowWidth = 1280;
    inline constexpr int kWindowHeight = 800;

    // The working budget and the input-to-paint target from docs/hardware-environment.md. The
    // budget is for when there's no display to derive one from.
    inline constexpr double kFallbackFrameBudgetMs = 16.0;
    inline constexpr double kInputToPaintTargetMs = 8.0;

    inline constexpr int kOffscreenWidth = 64;
    inline constexpr int kOffscreenHeight = 64;

    // Swapchain and offscreen target both use an sRGB-encoded format (decision 3
    // in docs/buildouts/M1-textured-quad.md): the eventual glyph atlas needs
    // linear-space alpha blending for antialiased edges, so this is load-bearing
    // now rather than retrofitted once text rendering exists.
    inline constexpr SDL_GPUTextureFormat kOffscreenColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;

    // The clear color is defined in 8-bit terms so the offscreen mode can assert
    // exact byte values. Float form is derived, never the other way around.
    inline constexpr std::uint8_t kClearR = 0x1A;
    inline constexpr std::uint8_t kClearG = 0x1A;
    inline constexpr std::uint8_t kClearB = 0x1F;
    inline constexpr std::uint8_t kClearA = 0xFF;

    // Drivers differ by one ULP in unorm rounding. Tolerate it, nothing more.
    inline constexpr int kChannelTolerance = 1;

    // The sRGB electro-optical transfer function's piecewise-linear constants
    // (IEC 61966-2-1).
    inline constexpr float kSrgbLinearThreshold = 0.04045F;
    inline constexpr float kSrgbLinearSlope = 12.92F;
    inline constexpr float kSrgbGammaOffset = 0.055F;
    inline constexpr float kSrgbGammaScale = 1.055F;
    inline constexpr float kSrgbGamma = 2.4F;

    // An _SRGB texture format auto-encodes a shader's linear float output to its
    // sRGB-encoded byte storage. To keep the clear assertion's expected bytes
    // (kClearR/G/B/A) exactly as they are, the clear color fed to SDL has to be
    // the *decoded* linear value that round-trips back through that encode.
    inline float srgb_decode(std::uint8_t v) noexcept
    {
        const float c = static_cast<float>(v) / 255.0F;
        return c <= kSrgbLinearThreshold ? c / kSrgbLinearSlope
                                         : std::pow((c + kSrgbGammaOffset) / kSrgbGammaScale, kSrgbGamma);
    }

    inline SDL_FColor clear_color() noexcept
    {
        return SDL_FColor{
            .r = srgb_decode(kClearR),
            .g = srgb_decode(kClearG),
            .b = srgb_decode(kClearB),
            .a = srgb_decode(kClearA),
        };
    }

} // namespace stackide
