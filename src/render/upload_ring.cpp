#include <stackide/render/upload_ring.hpp>

#include <stackide/base/log.hpp>

#include <cstring>
#include <utility>

namespace stackide::render
{

    std::optional<UploadRing> UploadRing::create(const Device& device, Uint32 capacity)
    {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = capacity;
        TransferBuffer buffer = device.create_transfer_buffer(info, "upload ring");
        if (!buffer)
        {
            return std::nullopt;
        }
        return UploadRing{device.get(), std::move(buffer), capacity};
    }

    UploadRing::~UploadRing() { end(); }

    bool UploadRing::begin()
    {
        cursor_.reset();
        mapped_ = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_, buffer_.get(), true));
        if (mapped_ == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_MapGPUTransferBuffer failed: {}", SDL_GetError());
            return false;
        }
        return true;
    }

    std::optional<SDL_GPUTransferBufferLocation> UploadRing::write(std::span<const std::byte> bytes, Uint32 alignment)
    {
        const auto offset = cursor_.reserve(static_cast<Uint32>(bytes.size()), alignment);
        if (!offset)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "upload ring full: {} byte write after {} bytes used", bytes.size(),
                               cursor_.used());
            return std::nullopt;
        }
        std::memcpy(mapped_ + *offset, bytes.data(), bytes.size());
        return SDL_GPUTransferBufferLocation{.transfer_buffer = buffer_.get(), .offset = *offset};
    }

    void UploadRing::end()
    {
        if (mapped_ != nullptr)
        {
            SDL_UnmapGPUTransferBuffer(device_, buffer_.get());
            mapped_ = nullptr;
        }
    }

} // namespace stackide::render
