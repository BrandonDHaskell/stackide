#include <stackide/render/modes.hpp>

#include <stackide/base/constants.hpp>
#include <stackide/base/log.hpp>
#include <stackide/base/scope_exit.hpp>
#include <stackide/platform/video.hpp>
#include <stackide/render/gpu.hpp>
#include <stackide/render/quad_resources.hpp>
#include <stackide/render/render_constants.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

namespace stackide
{
    namespace
    {

        bool pixel_matches(const std::uint8_t* px, std::array<std::uint8_t, 4> expected)
        {
            const auto near = [](std::uint8_t got, std::uint8_t want)
            { return std::abs(static_cast<int>(got) - static_cast<int>(want)) <= kChannelTolerance; };
            return near(px[0], expected.at(0)) && near(px[1], expected.at(1)) && near(px[2], expected.at(2)) &&
                near(px[3], expected.at(3));
        }

        bool check_pixel(const std::uint8_t* pixels, Uint32 offset, std::array<std::uint8_t, 4> expected)
        {
            const std::uint8_t* px = pixels + offset;
            if (pixel_matches(px, expected))
            {
                return true;
            }
            STACKIDE_LOG_ERROR(LogCategory::Render,
                               "pixel mismatch at byte {}: got {:02X}{:02X}{:02X}{:02X}, "
                               "expected {:02X}{:02X}{:02X}{:02X}",
                               offset, px[0], px[1], px[2], px[3], expected.at(0), expected.at(1), expected.at(2),
                               expected.at(3));
            return false;
        }

        int render_and_verify(const Options& opts)
        {
            std::optional<render::Device> device = render::Device::create(opts.gpu_debug);
            if (!device)
            {
                return render::gpu_unavailable_code(opts);
            }

            constexpr auto kFormat = kOffscreenColorFormat;
            if (!SDL_GPUTextureSupportsFormat(device->get(), kFormat, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET))
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "R8G8B8A8_UNORM_SRGB color target unsupported on this device");
                return EXIT_FAILURE;
            }

            SDL_GPUTextureCreateInfo tex_info{};
            tex_info.type = SDL_GPU_TEXTURETYPE_2D;
            tex_info.format = kFormat;
            tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            tex_info.width = kOffscreenWidth;
            tex_info.height = kOffscreenHeight;
            tex_info.layer_count_or_depth = 1;
            tex_info.num_levels = 1;
            tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            const render::Texture color = device->create_texture(tex_info, "render target");

            const std::optional<QuadResources> quad = create_quad_resources(*device, kFormat);
            if (!color || !quad)
            {
                return render::gpu_unavailable_code(opts);
            }

            // The checker-to-pixel mapping below assumes the render target and the
            // checkerboard texture are the same size, so a texel lands on exactly
            // one output pixel with no scaling.
            static_assert(kOffscreenWidth == kCheckerSize && kOffscreenHeight == kCheckerSize);

            constexpr Uint32 kPixelBytes = 4;
            constexpr Uint32 kBufferSize = kOffscreenWidth * kOffscreenHeight * kPixelBytes;

            SDL_GPUTransferBufferCreateInfo tb_info{};
            tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            tb_info.size = kBufferSize;
            const render::TransferBuffer transfer = device->create_transfer_buffer(tb_info, "readback buffer");
            if (!transfer)
            {
                return EXIT_FAILURE;
            }

            std::optional<render::CommandBuffer> commands = render::CommandBuffer::acquire(*device);
            if (!commands)
            {
                return EXIT_FAILURE;
            }
            {
                std::optional<render::RenderPass> pass = commands->begin_render_pass(color.get(), clear_color());
                if (!pass)
                {
                    return EXIT_FAILURE;
                }
                draw_quad(*commands, *pass, *quad, {1.0F, 1.0F});
            }
            {
                std::optional<render::CopyPass> copy = commands->begin_copy_pass();
                if (!copy)
                {
                    return EXIT_FAILURE;
                }

                SDL_GPUTextureRegion region{};
                region.texture = color.get();
                region.w = kOffscreenWidth;
                region.h = kOffscreenHeight;
                region.d = 1;

                SDL_GPUTextureTransferInfo destination{};
                destination.transfer_buffer = transfer.get();
                destination.offset = 0;
                destination.pixels_per_row = kOffscreenWidth;
                destination.rows_per_layer = kOffscreenHeight;

                copy->download(region, destination);
            }
            if (!commands->submit_and_wait())
            {
                return EXIT_FAILURE;
            }

            auto* pixels = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device->get(), transfer.get(), false));
            if (pixels == nullptr)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_MapGPUTransferBuffer failed: {}", SDL_GetError());
                return EXIT_FAILURE;
            }
            const ScopeExit unmap{[&] { SDL_UnmapGPUTransferBuffer(device->get(), transfer.get()); }};

            // Four checker cells, including a boundary pair straddling x=8 on the
            // same row: catches both correct sampling and an off-by-one in the
            // checker's cell math.
            constexpr Uint32 kBoundaryLightOffset = ((7 * kOffscreenWidth) + 7) * kPixelBytes;
            constexpr Uint32 kBoundaryDarkOffset = ((7 * kOffscreenWidth) + 8) * kPixelBytes;
            constexpr Uint32 kInteriorOffset = ((32 * kOffscreenWidth) + 32) * kPixelBytes;
            constexpr Uint32 kFarCornerOffset = ((0 * kOffscreenWidth) + 63) * kPixelBytes;

            const std::array<std::pair<Uint32, std::array<std::uint8_t, 4>>, 4> checks = {
                {
                    {kBoundaryLightOffset, kCheckerLight},
                    {kBoundaryDarkOffset, kCheckerDark},
                    {kInteriorOffset, kCheckerLight},
                    {kFarCornerOffset, kCheckerDark},
                },
            };

            for (const auto& [offset, expected] : checks)
            {
                if (!check_pixel(pixels, offset, expected))
                {
                    return EXIT_FAILURE;
                }
            }

            STACKIDE_LOG_INFO(LogCategory::Render, "offscreen: quad verified at {}x{} (4 UV checks)", kOffscreenWidth,
                              kOffscreenHeight);
            return EXIT_SUCCESS;
        }

    } // namespace

    int run_offscreen(const Options& opts)
    {
        // No window is created and this needs to run on a CI host with no display
        // server, but it can't use the dummy backend: dummy can't create a Vulkan surface,
        // so SDL_CreateGPUDevice fails under it regardless of hardware availability.
        const std::optional<platform::Video> video = platform::Video::start(platform::VideoBackend::Offscreen);
        if (!video)
        {
            return EXIT_FAILURE;
        }
        return render::apply_gpu_debug_result(opts, render_and_verify(opts));
    }

} // namespace stackide
