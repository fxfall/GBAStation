#pragma once

#if defined(__APPLE__) && !defined(__SWITCH__)

#include "third_party/RetroArch-1.22.2/libretro-common/include/libretro_vulkan.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace beiklive {

/// Minimal libretro Vulkan frontend for external cores on Apple Silicon.
///
/// The host keeps the existing OpenGL UI.  It gives the core a tiny
/// Metal-backed Vulkan surface without creating a window, then copies the
/// presented image to CPU memory for the existing GameRenderer.
class LibretroVulkanHost final {
public:
    LibretroVulkanHost() = default;
    ~LibretroVulkanHost();

    LibretroVulkanHost(const LibretroVulkanHost&) = delete;
    LibretroVulkanHost& operator=(const LibretroVulkanHost&) = delete;

    bool initialize(const retro_hw_render_context_negotiation_interface_vulkan& negotiation);
    void shutdown();

    bool isInitialized() const { return m_instance != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE; }
    const retro_hw_render_interface* interfacePtr() const
    {
        return reinterpret_cast<const retro_hw_render_interface*>(&m_interface);
    }

    /// Copy the latest core image to a CPU RGBA8888 frame.
    bool readbackFrame(unsigned width, unsigned height,
                       std::vector<uint32_t>& pixels);

private:
    static void setImageThunk(void* handle, const retro_vulkan_image* image,
                              uint32_t numSemaphores, const VkSemaphore* semaphores,
                              uint32_t srcQueueFamily);
    static uint32_t getSyncIndexThunk(void* handle);
    static uint32_t getSyncIndexMaskThunk(void* handle);
    static void setCommandBuffersThunk(void* handle, uint32_t numCmd,
                                       const VkCommandBuffer* commands);
    static void waitSyncIndexThunk(void* handle);
    static void lockQueueThunk(void* handle);
    static void unlockQueueThunk(void* handle);
    static void setSignalSemaphoreThunk(void* handle, VkSemaphore semaphore);

    void setImage(const retro_vulkan_image* image, uint32_t numSemaphores,
                  const VkSemaphore* semaphores, uint32_t srcQueueFamily);
    void setCommandBuffers(uint32_t numCmd, const VkCommandBuffer* commands);
    void waitSyncIndex();
    void lockQueue();
    void unlockQueue();
    void setSignalSemaphore(VkSemaphore semaphore);

    bool createInstance(const retro_hw_render_context_negotiation_interface_vulkan& negotiation);
    bool createMetalSurface();
    bool choosePhysicalDevice();
    bool createFallbackDevice();
    bool createReadbackResources(VkDeviceSize size);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    void destroyReadbackResources();
    void resetState();

    retro_hw_render_context_negotiation_interface_vulkan m_negotiation{};
    retro_vulkan_destroy_device_t m_destroyDevice = nullptr;
    retro_hw_render_interface_vulkan m_interface{};
    retro_vulkan_context m_coreContext{};

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    void* m_metalLayer = nullptr;
    VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    bool m_deviceCreatedByCore = false;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_copyCommand = VK_NULL_HANDLE;
    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;

    retro_vulkan_image m_image{};
    bool m_imageValid = false;
    std::vector<VkSemaphore> m_waitSemaphores;
    std::vector<VkCommandBuffer> m_pendingCommands;
    VkSemaphore m_signalSemaphore = VK_NULL_HANDLE;
    std::atomic<uint32_t> m_syncIndex{0};
    mutable std::mutex m_queueMutex;
};

} // namespace beiklive

#endif // defined(__APPLE__) && !defined(__SWITCH__)
