#include <stackide/render/gpu.hpp>

#include <stackide/base/log.hpp>
#include <stackide/platform/video.hpp>

namespace stackide::render
{
    namespace
    {

        template <class Handle>
        Handle checked(SDL_GPUDevice* device, auto* raw, std::string_view what)
        {
            if (raw == nullptr)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "creating {} failed: {}", what, SDL_GetError());
            }
            return Handle{device, raw};
        }

    } // namespace

    std::optional<Device> Device::create(bool debug)
    {
        SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, nullptr);
        if (device == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_CreateGPUDevice failed: {}", SDL_GetError());
            return std::nullopt;
        }

        const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device);
        STACKIDE_LOG_INFO(LogCategory::Render, "gpu: {} ({}), driver {} {}",
                          SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown device"),
                          SDL_GetGPUDeviceDriver(device),
                          SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "unknown"),
                          SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unknown"));
        return Device{device};
    }

    Device::~Device()
    {
        if (device_ != nullptr)
        {
            SDL_DestroyGPUDevice(device_);
        }
    }

    Shader Device::create_shader(const SDL_GPUShaderCreateInfo& info, std::string_view what) const
    {
        return checked<Shader>(device_, SDL_CreateGPUShader(device_, &info), what);
    }

    Buffer Device::create_buffer(const SDL_GPUBufferCreateInfo& info, std::string_view what) const
    {
        return checked<Buffer>(device_, SDL_CreateGPUBuffer(device_, &info), what);
    }

    Texture Device::create_texture(const SDL_GPUTextureCreateInfo& info, std::string_view what) const
    {
        return checked<Texture>(device_, SDL_CreateGPUTexture(device_, &info), what);
    }

    Sampler Device::create_sampler(const SDL_GPUSamplerCreateInfo& info, std::string_view what) const
    {
        return checked<Sampler>(device_, SDL_CreateGPUSampler(device_, &info), what);
    }

    Pipeline Device::create_pipeline(const SDL_GPUGraphicsPipelineCreateInfo& info, std::string_view what) const
    {
        return checked<Pipeline>(device_, SDL_CreateGPUGraphicsPipeline(device_, &info), what);
    }

    TransferBuffer Device::create_transfer_buffer(const SDL_GPUTransferBufferCreateInfo& info,
                                                  std::string_view what) const
    {
        return checked<TransferBuffer>(device_, SDL_CreateGPUTransferBuffer(device_, &info), what);
    }

    std::optional<Swapchain> Swapchain::create(const Device& device, const platform::Window& window, PresentMode mode)
    {
        SDL_GPUDevice* gpu = device.get();
        SDL_Window* sdl_window = window.sdl_handle();
        if (!SDL_ClaimWindowForGPUDevice(gpu, sdl_window))
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
            return std::nullopt;
        }
        // Owns the claim from here on, so that the failures below release it.
        Swapchain swapchain{gpu, sdl_window};

        // Required, not a preference (decision 3 in docs/buildouts/M1-textured-quad.md):
        // never fall back to plain SDR composition.
        if (!SDL_WindowSupportsGPUSwapchainComposition(gpu, sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR))
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDR_LINEAR swapchain composition not supported on this device");
            return std::nullopt;
        }

        // Every SDL_GPU backend guarantees vsync, but neither of the others.
        SDL_GPUPresentMode present_mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (mode == PresentMode::Immediate)
        {
            present_mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            if (!SDL_WindowSupportsGPUPresentMode(gpu, sdl_window, present_mode))
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "immediate present mode not supported on this device");
                return std::nullopt;
            }
        }
        else if (mode == PresentMode::Mailbox)
        {
            if (SDL_WindowSupportsGPUPresentMode(gpu, sdl_window, SDL_GPU_PRESENTMODE_MAILBOX))
            {
                present_mode = SDL_GPU_PRESENTMODE_MAILBOX;
            }
            else
            {
                STACKIDE_LOG_WARN(LogCategory::Render, "mailbox present mode not supported, using vsync");
            }
        }

        if (!SDL_SetGPUSwapchainParameters(gpu, sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR, present_mode))
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_SetGPUSwapchainParameters failed: {}", SDL_GetError());
            return std::nullopt;
        }
        return swapchain;
    }

    Swapchain::~Swapchain()
    {
        if (window_ != nullptr)
        {
            SDL_ReleaseWindowFromGPUDevice(device_, window_);
        }
    }

    SDL_GPUTextureFormat Swapchain::format() const { return SDL_GetGPUSwapchainTextureFormat(device_, window_); }

    RenderPass::~RenderPass()
    {
        if (pass_ != nullptr)
        {
            SDL_EndGPURenderPass(pass_);
        }
    }

    void RenderPass::bind_pipeline(const Pipeline& pipeline) noexcept
    {
        SDL_BindGPUGraphicsPipeline(pass_, pipeline.get());
    }

    void RenderPass::bind_vertex_buffer(const Buffer& buffer) noexcept
    {
        const SDL_GPUBufferBinding binding{.buffer = buffer.get(), .offset = 0};
        SDL_BindGPUVertexBuffers(pass_, 0, &binding, 1);
    }

    void RenderPass::bind_index_buffer_16(const Buffer& buffer) noexcept
    {
        const SDL_GPUBufferBinding binding{.buffer = buffer.get(), .offset = 0};
        SDL_BindGPUIndexBuffer(pass_, &binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    }

    void RenderPass::bind_fragment_texture(const Texture& texture, const Sampler& sampler) noexcept
    {
        const SDL_GPUTextureSamplerBinding binding{.texture = texture.get(), .sampler = sampler.get()};
        SDL_BindGPUFragmentSamplers(pass_, 0, &binding, 1);
    }

    void RenderPass::draw_indexed(Uint32 index_count) noexcept
    {
        SDL_DrawGPUIndexedPrimitives(pass_, index_count, 1, 0, 0, 0);
    }

    CopyPass::~CopyPass()
    {
        if (pass_ != nullptr)
        {
            SDL_EndGPUCopyPass(pass_);
        }
    }

    void CopyPass::upload(const SDL_GPUTransferBufferLocation& source, const Buffer& destination, Uint32 size) noexcept
    {
        const SDL_GPUBufferRegion region{.buffer = destination.get(), .offset = 0, .size = size};
        SDL_UploadToGPUBuffer(pass_, &source, &region, false);
    }

    void CopyPass::upload(const SDL_GPUTextureTransferInfo& source, const SDL_GPUTextureRegion& destination) noexcept
    {
        SDL_UploadToGPUTexture(pass_, &source, &destination, false);
    }

    void CopyPass::download(const SDL_GPUTextureRegion& source, const SDL_GPUTextureTransferInfo& destination) noexcept
    {
        SDL_DownloadFromGPUTexture(pass_, &source, &destination);
    }

    void CopyPass::download(const Buffer& source, Uint32 size,
                            const SDL_GPUTransferBufferLocation& destination) noexcept
    {
        const SDL_GPUBufferRegion region{.buffer = source.get(), .offset = 0, .size = size};
        SDL_DownloadFromGPUBuffer(pass_, &region, &destination);
    }

    std::optional<CommandBuffer> CommandBuffer::acquire(const Device& device)
    {
        SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(device.get());
        if (command_buffer == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
            return std::nullopt;
        }
        return CommandBuffer{device.get(), command_buffer};
    }

    CommandBuffer::~CommandBuffer()
    {
        if (command_buffer_ == nullptr)
        {
            return;
        }
        if (acquired_swapchain_)
        {
            SDL_SubmitGPUCommandBuffer(command_buffer_);
        }
        else
        {
            SDL_CancelGPUCommandBuffer(command_buffer_);
        }
    }

    std::optional<SDL_GPUTexture*> CommandBuffer::wait_for_swapchain_texture(const Swapchain& swapchain)
    {
        SDL_GPUTexture* texture = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer_, swapchain.window(), &texture, nullptr, nullptr))
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_WaitAndAcquireGPUSwapchainTexture failed: {}", SDL_GetError());
            return std::nullopt;
        }
        acquired_swapchain_ = texture != nullptr;
        return texture;
    }

    std::optional<RenderPass> CommandBuffer::begin_render_pass(SDL_GPUTexture* target, SDL_FColor clear)
    {
        SDL_GPUColorTargetInfo info{};
        info.texture = target;
        info.clear_color = clear;
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer_, &info, 1, nullptr);
        if (pass == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_BeginGPURenderPass failed: {}", SDL_GetError());
            return std::nullopt;
        }
        return RenderPass{pass};
    }

    std::optional<CopyPass> CommandBuffer::begin_copy_pass()
    {
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(command_buffer_);
        if (pass == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_BeginGPUCopyPass failed: {}", SDL_GetError());
            return std::nullopt;
        }
        return CopyPass{pass};
    }

    void CommandBuffer::push_vertex_uniform(const void* data, Uint32 size) noexcept
    {
        SDL_PushGPUVertexUniformData(command_buffer_, 0, data, size);
    }

    bool CommandBuffer::submit()
    {
        const bool submitted = SDL_SubmitGPUCommandBuffer(std::exchange(command_buffer_, nullptr));
        if (!submitted)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_SubmitGPUCommandBuffer failed: {}", SDL_GetError());
        }
        return submitted;
    }

    bool CommandBuffer::submit_and_wait()
    {
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(std::exchange(command_buffer_, nullptr));
        if (fence == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_SubmitGPUCommandBufferAndAcquireFence failed: {}",
                               SDL_GetError());
            return false;
        }
        const bool waited = SDL_WaitForGPUFences(device_, true, &fence, 1);
        if (!waited)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "SDL_WaitForGPUFences failed: {}", SDL_GetError());
        }
        SDL_ReleaseGPUFence(device_, fence);
        return waited;
    }

    int apply_gpu_debug_result(const Options& options, int exit_code)
    {
        if (!options.gpu_debug)
        {
            return exit_code;
        }
        if (const auto errors = platform::gpu_error_count(); errors > 0)
        {
            STACKIDE_LOG_ERROR(LogCategory::Render, "gpu debug: {} error(s) reported", errors);
            return EXIT_FAILURE;
        }
        if (platform::gpu_validation_missing())
        {
            if (options.require_validation)
            {
                STACKIDE_LOG_ERROR(LogCategory::Render, "gpu debug: validation layers are required but not installed");
                return exit_code == EXIT_SUCCESS ? gpu_unavailable_code(options) : exit_code;
            }
            STACKIDE_LOG_WARN(LogCategory::Render, "gpu debug: validation layers not installed, nothing was validated");
        }
        return exit_code;
    }

} // namespace stackide::render
