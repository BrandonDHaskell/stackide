#include <stackide/platform/video.hpp>
#include <stackide/render/frame_graph.hpp>
#include <stackide/render/gpu.hpp>

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <print>

using namespace stackide;

namespace
{

    constexpr Uint32 kWidth = 400;
    constexpr Uint32 kHeight = 200;
    constexpr render::FrameGraphLayout kLayout{};

    struct Color
    {
        int r;
        int g;
        int b;
    };

    enum class Hue : std::uint8_t
    {
        Background,
        Green,
        Blue,
        Red,
        White,
    };

    Hue classify(const std::uint8_t* pixel)
    {
        const int r = pixel[0];
        const int g = pixel[1];
        const int b = pixel[2];
        constexpr int kBright = 200;
        constexpr int kDim = 150;
        if (r < kDim / 3 && g < kDim / 3 && b < kDim / 3)
        {
            return Hue::Background;
        }
        if (r > kBright && g > kBright && b > kBright)
        {
            return Hue::White;
        }
        if (r > g && r > b)
        {
            return Hue::Red;
        }
        return g > b ? Hue::Green : Hue::Blue;
    }

    // Reads the rendered result, with y measured up from the bottom as the graph lays it out.
    class Readback
    {
    public:
        explicit Readback(const std::uint8_t* pixels) : pixels_(pixels) {}

        [[nodiscard]] Hue at(float x_px, float y_px) const
        {
            const auto x = static_cast<Uint32>(x_px);
            const auto y = kHeight - 1 - static_cast<Uint32>(y_px);
            return classify(pixels_ + ((y * kWidth + x) * 4));
        }

    private:
        const std::uint8_t* pixels_;
    };

    float bar_center_x(std::size_t bar)
    {
        return kLayout.margin_px + ((static_cast<float>(bar) + 0.5F) * kLayout.bar_width_px) - 0.5F;
    }

    float strip_y(int strip, float fraction)
    {
        const float bottom =
            kLayout.margin_px + (static_cast<float>(strip) * (kLayout.strip_height_px + kLayout.strip_gap_px));
        return bottom + (fraction * kLayout.strip_height_px);
    }

} // namespace

TEST_CASE("the frame graph draws what it's given")
{
    const auto video = platform::Video::start(platform::VideoBackend::Offscreen);
    REQUIRE(video);
    const auto device = render::Device::create(false);
    if (!device)
    {
        std::println("skipped: no GPU device");
        if (std::getenv("STACKIDE_REQUIRE_GPU") != nullptr)
        {
            FAIL("no GPU device, and STACKIDE_REQUIRE_GPU is set");
        }
        return;
    }

    constexpr auto kFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = kFormat;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tex_info.width = kWidth;
    tex_info.height = kHeight;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    const render::Texture target = device->create_texture(tex_info, "graph target");
    REQUIRE(target);

    const auto graph = render::FrameGraph::create(*device, kFormat);
    REQUIRE(graph);

    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = kWidth * kHeight * 4;
    const render::TransferBuffer transfer = device->create_transfer_buffer(tb_info, "graph readback");
    REQUIRE(transfer);

    std::array<float, kFrameHistorySize> frames{};
    std::array<float, kFrameHistorySize> inputs{};
    frames.fill(-1.0F);
    inputs.fill(-1.0F);
    frames.at(10) = 0.5F; // half the limit
    frames.at(20) = 1.5F; // over the limit
    frames.at(30) = 50.0F; // far over, which has to clip to the strip
    inputs.at(40) = 0.5F;
    inputs.at(50) = 1.5F;

    auto commands = render::CommandBuffer::acquire(*device);
    REQUIRE(commands);
    {
        const SDL_FColor clear{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        auto pass = commands->begin_render_pass(target.get(), clear);
        REQUIRE(pass);
        graph->draw(*commands, *pass, kWidth, kHeight, frames, inputs);
    }
    {
        auto copy = commands->begin_copy_pass();
        REQUIRE(copy);
        SDL_GPUTextureRegion region{};
        region.texture = target.get();
        region.w = kWidth;
        region.h = kHeight;
        region.d = 1;
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = transfer.get();
        destination.pixels_per_row = kWidth;
        destination.rows_per_layer = kHeight;
        copy->download(region, destination);
    }
    REQUIRE(commands->submit_and_wait());

    const auto* pixels =
        static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device->get(), transfer.get(), false));
    REQUIRE(pixels != nullptr);
    const Readback image{pixels};

    // The limit line sits at the strip's limit position.
    const float limit = kLayout.limit_position;

    SECTION("a bar under the limit is green, stops below the line, and the rest stay empty")
    {
        CHECK(image.at(bar_center_x(10), strip_y(0, 0.1F)) == Hue::Green);
        CHECK(image.at(bar_center_x(10), strip_y(0, limit * 0.4F)) == Hue::Green);
        CHECK(image.at(bar_center_x(10), strip_y(0, limit * 0.6F)) == Hue::Background);
        CHECK(image.at(bar_center_x(11), strip_y(0, 0.1F)) == Hue::Background);
        CHECK(image.at(bar_center_x(119), strip_y(0, 0.1F)) == Hue::Background);
    }

    SECTION("a bar over the limit is red and rises above the line")
    {
        CHECK(image.at(bar_center_x(20), strip_y(0, 0.1F)) == Hue::Red);
        CHECK(image.at(bar_center_x(20), strip_y(0, limit * 1.3F)) == Hue::Red);
        CHECK(image.at(bar_center_x(20), strip_y(0, limit * 1.7F)) == Hue::Background);
    }

    SECTION("a huge value clips to its strip")
    {
        CHECK(image.at(bar_center_x(30), strip_y(0, 0.95F)) == Hue::Red);
        CHECK(image.at(bar_center_x(30), strip_y(0, 1.0F) + (kLayout.strip_gap_px / 2.0F)) == Hue::Background);
    }

    SECTION("input to paint has its own strip, in its own color")
    {
        CHECK(image.at(bar_center_x(40), strip_y(1, 0.1F)) == Hue::Blue);
        CHECK(image.at(bar_center_x(40), strip_y(0, 0.1F)) == Hue::Background);
        CHECK(image.at(bar_center_x(50), strip_y(1, limit * 1.3F)) == Hue::Red);
    }

    SECTION("each strip has a limit line across its full width")
    {
        CHECK(image.at(bar_center_x(0), strip_y(0, limit)) == Hue::White);
        CHECK(image.at(bar_center_x(119), strip_y(0, limit)) == Hue::White);
        CHECK(image.at(bar_center_x(0), strip_y(1, limit)) == Hue::White);
        CHECK(image.at(bar_center_x(119), strip_y(1, limit)) == Hue::White);
    }

    SDL_UnmapGPUTransferBuffer(device->get(), transfer.get());
}
