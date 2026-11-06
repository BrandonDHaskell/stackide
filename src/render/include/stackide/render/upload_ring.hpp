#pragma once

#include <stackide/base/bump_cursor.hpp>
#include <stackide/render/gpu.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace stackide::render
{

    // One persistent transfer buffer reused every frame, rather than a new one per upload:
    //
    //     ring.begin();
    //     auto location = ring.write(bytes);
    //     ring.end();                          // before any upload from it
    //     copy_pass.upload(*location, destination, size);
    //
    // begin() asks SDL to cycle the buffer, so writing never waits for a frame the GPU is
    // still reading. SDL keeps the buffers that cycling creates, so after a few frames
    // nothing new is allocated.
    class UploadRing
    {
    public:
        // Empty on failure, having logged why.
        static std::optional<UploadRing> create(const Device& device, Uint32 capacity);

        UploadRing(const UploadRing&) = delete;
        UploadRing& operator=(const UploadRing&) = delete;
        UploadRing(UploadRing&&) noexcept = default;
        UploadRing& operator=(UploadRing&&) = delete;
        ~UploadRing();

        bool begin();
        // Empty if the frame's uploads don't fit. Call between begin() and end().
        std::optional<SDL_GPUTransferBufferLocation> write(std::span<const std::byte> bytes, Uint32 alignment = 1);
        void end();

    private:
        UploadRing(SDL_GPUDevice* device, TransferBuffer buffer, Uint32 capacity) noexcept :
            device_(device), buffer_(std::move(buffer)), cursor_(capacity)
        {
        }

        SDL_GPUDevice* device_;
        TransferBuffer buffer_;
        BumpCursor cursor_;
        std::byte* mapped_ = nullptr;
    };

} // namespace stackide::render
