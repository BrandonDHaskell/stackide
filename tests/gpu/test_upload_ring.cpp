#include <stackide/platform/video.hpp>
#include <stackide/render/gpu.hpp>
#include <stackide/render/upload_ring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <print>
#include <span>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STACKIDE_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define STACKIDE_TEST_SANITIZED 1
#endif
#endif
#ifndef STACKIDE_TEST_SANITIZED
#define STACKIDE_TEST_SANITIZED 0
#endif

using namespace stackide;

namespace
{

    constexpr Uint32 kUploadBytes = Uint32{4} << 20U;
    // SDL's cycling settles once the GPU has finished with the first buffers.
    constexpr int kWarmupFrames = 16;
    constexpr int kMeasuredFrames = 64;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    std::atomic<std::uint64_t> g_allocations{0};

    bool upload_frame(const render::Device& device, render::UploadRing& ring, const render::Buffer& destination,
                      std::span<const std::byte> bytes)
    {
        if (!ring.begin())
        {
            return false;
        }
        const auto location = ring.write(bytes);
        ring.end();
        if (!location)
        {
            return false;
        }
        auto commands = render::CommandBuffer::acquire(device);
        if (!commands)
        {
            return false;
        }
        {
            auto copy = commands->begin_copy_pass();
            if (!copy)
            {
                return false;
            }
            copy->upload(*location, destination, static_cast<Uint32>(bytes.size()));
        }
        return commands->submit_and_wait();
    }

} // namespace

#if !STACKIDE_TEST_SANITIZED
// Not inlined because GCC otherwise reports a free() of new'd memory, a false positive.
[[gnu::noinline]] void* operator new(std::size_t size)
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr)
    {
        throw std::bad_alloc{};
    }
    return memory;
}
[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#endif

TEST_CASE("the upload ring delivers 4 MB per frame without allocating")
{
    // Counts SDL's own allocations too. Has to happen before SDL's first allocation.
    REQUIRE(SDL_SetMemoryFunctions(
        [](std::size_t size)
        {
            g_allocations.fetch_add(1, std::memory_order_relaxed);
            return std::malloc(size);
        },
        [](std::size_t count, std::size_t size)
        {
            g_allocations.fetch_add(1, std::memory_order_relaxed);
            return std::calloc(count, size);
        },
        [](void* memory, std::size_t size)
        {
            g_allocations.fetch_add(1, std::memory_order_relaxed);
            return std::realloc(memory, size);
        },
        [](void* memory) { std::free(memory); }));

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

    auto ring = render::UploadRing::create(*device, kUploadBytes);
    REQUIRE(ring);

    SDL_GPUBufferCreateInfo buffer_info{};
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = kUploadBytes;
    const render::Buffer destination = device->create_buffer(buffer_info, "upload destination");
    REQUIRE(destination);

    std::vector<std::byte> bytes(kUploadBytes);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        bytes.at(i) = static_cast<std::byte>((i * 2654435761U) >> 24U);
    }

    for (int i = 0; i < kWarmupFrames; ++i)
    {
        REQUIRE(upload_frame(*device, *ring, destination, bytes));
    }

    const auto before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kMeasuredFrames; ++i)
    {
        REQUIRE(upload_frame(*device, *ring, destination, bytes));
    }
    const auto allocations = g_allocations.load(std::memory_order_relaxed) - before;
#if !STACKIDE_TEST_SANITIZED
    CHECK(allocations == 0);
#else
    static_cast<void>(allocations);
#endif

    // The bytes must really have arrived.
    SDL_GPUTransferBufferCreateInfo readback_info{};
    readback_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    readback_info.size = kUploadBytes;
    const render::TransferBuffer readback = device->create_transfer_buffer(readback_info, "readback");
    REQUIRE(readback);
    {
        auto commands = render::CommandBuffer::acquire(*device);
        REQUIRE(commands);
        {
            auto copy = commands->begin_copy_pass();
            REQUIRE(copy);
            copy->download(destination, kUploadBytes,
                           SDL_GPUTransferBufferLocation{.transfer_buffer = readback.get(), .offset = 0});
        }
        REQUIRE(commands->submit_and_wait());
    }
    const void* mapped = SDL_MapGPUTransferBuffer(device->get(), readback.get(), false);
    REQUIRE(mapped != nullptr);
    CHECK(std::memcmp(mapped, bytes.data(), bytes.size()) == 0);
    SDL_UnmapGPUTransferBuffer(device->get(), readback.get());
}

TEST_CASE("a write that doesn't fit is refused")
{
    const auto video = platform::Video::start(platform::VideoBackend::Offscreen);
    REQUIRE(video);
    const auto device = render::Device::create(false);
    if (!device)
    {
        return;
    }
    auto ring = render::UploadRing::create(*device, 16);
    REQUIRE(ring);
    REQUIRE(ring->begin());
    const std::vector<std::byte> bytes(17);
    CHECK_FALSE(ring->write(bytes));
    CHECK(ring->write(std::span{bytes}.first(16)));
    ring->end();
}
