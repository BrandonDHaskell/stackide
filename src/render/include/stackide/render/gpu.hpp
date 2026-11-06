#pragma once

// A thin layer over SDL_GPU, covering only what STACK uses: the device, swapchain,
// command buffers, passes, and resource lifetimes. Failures are logged here, so that
// callers only decide what a failure means for them.

#include <stackide/base/constants.hpp>
#include <stackide/base/options.hpp>
#include <stackide/platform/window.hpp>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

namespace stackide::render
{

    // Owns one GPU resource. Empty after a failed creation or a move.
    template <class T, void (*Release)(SDL_GPUDevice*, T*)>
    class GpuHandle
    {
    public:
        GpuHandle() = default;
        GpuHandle(SDL_GPUDevice* device, T* handle) noexcept : device_(device), handle_(handle) {}
        GpuHandle(const GpuHandle&) = delete;
        GpuHandle& operator=(const GpuHandle&) = delete;
        GpuHandle(GpuHandle&& other) noexcept : device_(other.device_), handle_(std::exchange(other.handle_, nullptr))
        {
        }
        GpuHandle& operator=(GpuHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                device_ = other.device_;
                handle_ = std::exchange(other.handle_, nullptr);
            }
            return *this;
        }
        ~GpuHandle() { reset(); }

        [[nodiscard]] T* get() const noexcept { return handle_; }
        explicit operator bool() const noexcept { return handle_ != nullptr; }

    private:
        void reset() noexcept
        {
            if (handle_ != nullptr)
            {
                Release(device_, handle_);
                handle_ = nullptr;
            }
        }

        SDL_GPUDevice* device_ = nullptr;
        T* handle_ = nullptr;
    };

    using Shader = GpuHandle<SDL_GPUShader, SDL_ReleaseGPUShader>;
    using Buffer = GpuHandle<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>;
    using Texture = GpuHandle<SDL_GPUTexture, SDL_ReleaseGPUTexture>;
    using Sampler = GpuHandle<SDL_GPUSampler, SDL_ReleaseGPUSampler>;
    using Pipeline = GpuHandle<SDL_GPUGraphicsPipeline, SDL_ReleaseGPUGraphicsPipeline>;
    using TransferBuffer = GpuHandle<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer>;

    class Device
    {
    public:
        // Empty on failure, having logged why. debug requests validation layers, if the
        // system has them: check platform::gpu_validation_missing() to find out.
        static std::optional<Device> create(bool debug);

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&& other) noexcept : device_(std::exchange(other.device_, nullptr)) {}
        Device& operator=(Device&&) = delete;
        ~Device();

        [[nodiscard]] SDL_GPUDevice* get() const noexcept { return device_; }

        // Each is empty on failure, having logged the failure with what's description.
        [[nodiscard]] Shader create_shader(const SDL_GPUShaderCreateInfo& info, std::string_view what) const;
        [[nodiscard]] Buffer create_buffer(const SDL_GPUBufferCreateInfo& info, std::string_view what) const;
        [[nodiscard]] Texture create_texture(const SDL_GPUTextureCreateInfo& info, std::string_view what) const;
        [[nodiscard]] Sampler create_sampler(const SDL_GPUSamplerCreateInfo& info, std::string_view what) const;
        [[nodiscard]] Pipeline create_pipeline(const SDL_GPUGraphicsPipelineCreateInfo& info,
                                               std::string_view what) const;
        [[nodiscard]] TransferBuffer create_transfer_buffer(const SDL_GPUTransferBufferCreateInfo& info,
                                                            std::string_view what) const;

    private:
        explicit Device(SDL_GPUDevice* device) noexcept : device_(device) {}

        SDL_GPUDevice* device_;
    };

    // A window's claim on the device. Resizing is handled by SDL when a swapchain texture
    // is next acquired, which recreates the swapchain and nothing else.
    class Swapchain
    {
    public:
        // Empty on failure, having logged why. Mailbox falls back to vsync; immediate has
        // no fallback, because a measurement in the wrong mode would be meaningless.
        static std::optional<Swapchain> create(const Device& device, const platform::Window& window, PresentMode mode);

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;
        Swapchain(Swapchain&& other) noexcept : device_(other.device_), window_(std::exchange(other.window_, nullptr))
        {
        }
        Swapchain& operator=(Swapchain&&) = delete;
        ~Swapchain();

        [[nodiscard]] SDL_GPUTextureFormat format() const;
        [[nodiscard]] SDL_Window* window() const noexcept { return window_; }

    private:
        Swapchain(SDL_GPUDevice* device, SDL_Window* window) noexcept : device_(device), window_(window) {}

        SDL_GPUDevice* device_;
        SDL_Window* window_;
    };

    // Ends the pass on destruction, which must happen before the command buffer's submit.
    class RenderPass
    {
    public:
        explicit RenderPass(SDL_GPURenderPass* pass) noexcept : pass_(pass) {}
        RenderPass(const RenderPass&) = delete;
        RenderPass& operator=(const RenderPass&) = delete;
        RenderPass(RenderPass&& other) noexcept : pass_(std::exchange(other.pass_, nullptr)) {}
        RenderPass& operator=(RenderPass&&) = delete;
        ~RenderPass();

        void bind_pipeline(const Pipeline& pipeline) noexcept;
        void bind_vertex_buffer(const Buffer& buffer) noexcept;
        void bind_index_buffer_16(const Buffer& buffer) noexcept;
        void bind_fragment_texture(const Texture& texture, const Sampler& sampler) noexcept;
        void draw_indexed(Uint32 index_count) noexcept;
        void draw(Uint32 vertex_count) noexcept;

    private:
        SDL_GPURenderPass* pass_;
    };

    // Ends the pass on destruction, which must happen before the command buffer's submit.
    class CopyPass
    {
    public:
        explicit CopyPass(SDL_GPUCopyPass* pass) noexcept : pass_(pass) {}
        CopyPass(const CopyPass&) = delete;
        CopyPass& operator=(const CopyPass&) = delete;
        CopyPass(CopyPass&& other) noexcept : pass_(std::exchange(other.pass_, nullptr)) {}
        CopyPass& operator=(CopyPass&&) = delete;
        ~CopyPass();

        void upload(const SDL_GPUTransferBufferLocation& source, const Buffer& destination, Uint32 size) noexcept;
        void upload(const SDL_GPUTextureTransferInfo& source, const SDL_GPUTextureRegion& destination) noexcept;
        void download(const SDL_GPUTextureRegion& source, const SDL_GPUTextureTransferInfo& destination) noexcept;
        void download(const Buffer& source, Uint32 size, const SDL_GPUTransferBufferLocation& destination) noexcept;

    private:
        SDL_GPUCopyPass* pass_;
    };

    struct SwapchainTarget
    {
        SDL_GPUTexture* texture = nullptr; // nullptr if none is ready
        Uint32 width = 0;
        Uint32 height = 0;
    };

    class CommandBuffer
    {
    public:
        // Empty on failure, having logged why.
        static std::optional<CommandBuffer> acquire(const Device& device);

        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;
        CommandBuffer(CommandBuffer&& other) noexcept :
            device_(other.device_), command_buffer_(std::exchange(other.command_buffer_, nullptr)),
            acquired_swapchain_(other.acquired_swapchain_)
        {
        }
        CommandBuffer& operator=(CommandBuffer&&) = delete;
        // Cancels if never submitted. SDL forbids that once a swapchain texture has been
        // acquired, so it submits in that case instead.
        ~CommandBuffer();

        // Never waits on the GPU or the display: holds a null texture if none is ready yet, and
        // empty only on failure. A blocking wait isn't safe here, because a compositor stops
        // presenting to a hidden or locked surface, and that wait would then never end.
        [[nodiscard]] std::optional<SwapchainTarget> try_acquire_swapchain_texture(const Swapchain& swapchain);

        [[nodiscard]] std::optional<RenderPass> begin_render_pass(SDL_GPUTexture* target, SDL_FColor clear);
        [[nodiscard]] std::optional<CopyPass> begin_copy_pass();
        void push_vertex_uniform(const void* data, Uint32 size) noexcept;

        // Both return false on failure, having logged why.
        bool submit();
        bool submit_and_wait();

    private:
        CommandBuffer(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer) noexcept :
            device_(device), command_buffer_(command_buffer)
        {
        }

        SDL_GPUDevice* device_;
        SDL_GPUCommandBuffer* command_buffer_;
        bool acquired_swapchain_ = false;
    };

    // Applies what a --gpu-debug run found to its exit code. Call after the device is
    // destroyed, because destruction is when SDL and the layers report leaks.
    int apply_gpu_debug_result(const Options& options, int exit_code);

    // The exit code for a GPU that can't be used, which may be a skip.
    inline int gpu_unavailable_code(const Options& options) { return options.allow_skip ? kExitSkip : EXIT_FAILURE; }

} // namespace stackide::render
