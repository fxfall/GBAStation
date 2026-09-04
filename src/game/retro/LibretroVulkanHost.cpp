#if defined(__APPLE__) && !defined(__SWITCH__)

#include "LibretroVulkanHost.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <mach-o/dyld.h>
#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>

#ifndef GBASTATION_KOSMICKRISP_ICD_RELATIVE_PATH
#define GBASTATION_KOSMICKRISP_ICD_RELATIVE_PATH ""
#endif

namespace beiklive {

namespace {

constexpr uint32_t kMaxReadbackDimension = 8192;

// PPSSPP's bundled VMA path assumes every Vulkan 1.1/1.3 entry point is
// present when the loader reports the driver's native 1.4 version. KK
// intentionally exposes a smaller device-function set, so advertise the
// Vulkan 1.0 core ABI to this legacy libretro adapter while retaining the
// driver's actual implementation and Metal surface extensions.
PFN_vkGetInstanceProcAddr s_realInstanceProcAddr = nullptr;
PFN_vkGetPhysicalDeviceProperties s_realPhysicalDeviceProperties = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL enumerateInstanceVersionForLegacyCore(
    uint32_t* version)
{
    if (!version)
        return VK_ERROR_INITIALIZATION_FAILED;
    *version = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL getPhysicalDevicePropertiesForLegacyCore(
    VkPhysicalDevice device, VkPhysicalDeviceProperties* properties)
{
    if (!s_realPhysicalDeviceProperties || !properties)
        return;
    s_realPhysicalDeviceProperties(device, properties);
    // PPSSPP's libretro Vulkan adapter assumes every function promoted after
    // 1.0 exists when this field reports 1.4. KK intentionally exposes a
    // smaller legacy entry-point set; keep the adapter on the Vulkan 1.0 path
    // while using KK's actual device and implementation.
    properties->apiVersion = VK_API_VERSION_1_0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getInstanceProcAddrForLegacyCore(
    VkInstance instance, const char* name)
{
    if (name && std::strcmp(name, "vkEnumerateInstanceVersion") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(
            enumerateInstanceVersionForLegacyCore);
    if (name && std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
    {
        s_realPhysicalDeviceProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                s_realInstanceProcAddr
                    ? s_realInstanceProcAddr(instance, name)
                    : nullptr);
        return reinterpret_cast<PFN_vkVoidFunction>(
            getPhysicalDevicePropertiesForLegacyCore);
    }
    return s_realInstanceProcAddr
        ? s_realInstanceProcAddr(instance, name)
        : nullptr;
}

void* createMetalLayer()
{
    CAMetalLayer* layer = [[CAMetalLayer alloc] init];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!layer || !device)
    {
        [layer release];
        [device release];
        return nullptr;
    }

    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(1.0, 1.0);
    layer.frame = CGRectMake(0.0, 0.0, 1.0, 1.0);
    [device release];
    return (__bridge void*)layer;
}

void destroyMetalLayer(void* layer)
{
    if (layer)
        [(CAMetalLayer*)layer release];
}

bool containsKosmicKrisp(const char* value)
{
    if (!value)
        return false;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text.find("kosmickrisp") != std::string::npos;
}

std::filesystem::path executablePath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0)
        return {};

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    return std::filesystem::weakly_canonical(buffer.data());
}

const char* resultName(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        default: return "VK_ERROR_UNKNOWN";
    }
}

} // namespace

LibretroVulkanHost::~LibretroVulkanHost()
{
    shutdown();
}

bool LibretroVulkanHost::initialize(
    const retro_hw_render_context_negotiation_interface_vulkan& negotiation)
{
    shutdown();
    m_negotiation = negotiation;
    m_destroyDevice = negotiation.destroy_device;
    s_realInstanceProcAddr = vkGetInstanceProcAddr;

    // The final application is self-contained.  A caller may override this
    // with an explicit KosmicKrisp ICD for development, but another driver is
    // never accepted as a fallback.
    const char* driverFiles = std::getenv("VK_DRIVER_FILES");
    const char* legacyDriverFiles = std::getenv("VK_ICD_FILENAMES");
    if (driverFiles && *driverFiles)
    {
        if (!containsKosmicKrisp(driverFiles))
        {
            brls::Logger::error(
                "[LibretroVulkanHost] VK_DRIVER_FILES must point to KosmicKrisp");
            return false;
        }
    }
    else if (legacyDriverFiles && *legacyDriverFiles)
    {
        if (!containsKosmicKrisp(legacyDriverFiles))
        {
            brls::Logger::error(
                "[LibretroVulkanHost] VK_ICD_FILENAMES must point to KosmicKrisp");
            return false;
        }
    }
    else
    {
        const auto packagedIcd = executablePath().parent_path() /
            GBASTATION_KOSMICKRISP_ICD_RELATIVE_PATH;
        std::error_code error;
        if (!std::filesystem::is_regular_file(packagedIcd, error))
        {
            brls::Logger::error(
                "[LibretroVulkanHost] packaged KosmicKrisp ICD is missing: {}",
                packagedIcd.string());
            return false;
        }
        if (setenv("VK_DRIVER_FILES", packagedIcd.c_str(), 1) != 0)
        {
            brls::Logger::error(
                "[LibretroVulkanHost] failed to select packaged KosmicKrisp ICD");
            return false;
        }
    }

    if (!createInstance(negotiation) || !createMetalSurface() || !choosePhysicalDevice())
    {
        shutdown();
        return false;
    }

    bool deviceCreated = false;
    if (negotiation.create_device)
    {
        VkPhysicalDeviceFeatures requiredFeatures{};
        retro_vulkan_context context{};
        deviceCreated = negotiation.create_device(
            &context, m_instance, m_gpu, m_surface,
            getInstanceProcAddrForLegacyCore,
            nullptr, 0, nullptr, 0, &requiredFeatures);
        if (deviceCreated)
        {
            m_coreContext = context;
            m_device = context.device;
            m_queue = context.queue;
            m_queueFamily = context.queue_family_index;
            m_gpu = context.gpu != VK_NULL_HANDLE ? context.gpu : m_gpu;
            m_deviceCreatedByCore = true;
            if (m_device == VK_NULL_HANDLE || m_queue == VK_NULL_HANDLE)
            {
                brls::Logger::error(
                    "[LibretroVulkanHost] core returned an incomplete Vulkan context");
                shutdown();
                return false;
            }
        }
    }

    if (!deviceCreated && !createFallbackDevice())
    {
        shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] vkCreateCommandPool failed: {} ({})",
            resultName(result), static_cast<int>(result));
        shutdown();
        return false;
    }

    VkCommandBufferAllocateInfo commandInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = m_commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(m_device, &commandInfo, &m_copyCommand);
    if (result != VK_SUCCESS)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] vkAllocateCommandBuffers failed: {} ({})",
            resultName(result), static_cast<int>(result));
        shutdown();
        return false;
    }

    m_interface = {};
    m_interface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    m_interface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    m_interface.handle = this;
    m_interface.instance = m_instance;
    m_interface.gpu = m_gpu;
    m_interface.device = m_device;
    m_interface.get_device_proc_addr = vkGetDeviceProcAddr;
    m_interface.get_instance_proc_addr = getInstanceProcAddrForLegacyCore;
    m_interface.queue = m_queue;
    m_interface.queue_index = m_queueFamily;
    m_interface.set_image = setImageThunk;
    m_interface.get_sync_index = getSyncIndexThunk;
    m_interface.get_sync_index_mask = getSyncIndexMaskThunk;
    m_interface.set_command_buffers = setCommandBuffersThunk;
    m_interface.wait_sync_index = waitSyncIndexThunk;
    m_interface.lock_queue = lockQueueThunk;
    m_interface.unlock_queue = unlockQueueThunk;
    m_interface.set_signal_semaphore = setSignalSemaphoreThunk;

    brls::Logger::info(
        "[LibretroVulkanHost] KosmicKrisp Vulkan device ready: queueFamily={} "
        "coreDevice={} ",
        m_queueFamily, deviceCreated ? "yes" : "no");
    return true;
}

void LibretroVulkanHost::shutdown()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    destroyReadbackResources();

    if (m_commandPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
    m_copyCommand = VK_NULL_HANDLE;

    if (m_device != VK_NULL_HANDLE && m_deviceCreatedByCore && m_destroyDevice)
        m_destroyDevice();
    m_destroyDevice = nullptr;

    if (m_device != VK_NULL_HANDLE)
        vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_deviceCreatedByCore = false;

    if (m_instance != VK_NULL_HANDLE)
    {
        if (m_surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        vkDestroyInstance(m_instance, nullptr);
    }
    destroyMetalLayer(m_metalLayer);
    m_metalLayer = nullptr;
    m_instance = VK_NULL_HANDLE;
    s_realInstanceProcAddr = nullptr;
    s_realPhysicalDeviceProperties = nullptr;
    resetState();
}

void LibretroVulkanHost::resetState()
{
    m_negotiation = {};
    m_interface = {};
    m_coreContext = {};
    m_gpu = VK_NULL_HANDLE;
    m_queueFamily = 0;
    m_image = {};
    m_imageValid = false;
    m_waitSemaphores.clear();
    m_pendingCommands.clear();
    m_signalSemaphore = VK_NULL_HANDLE;
    m_syncIndex.store(0, std::memory_order_relaxed);
}

bool LibretroVulkanHost::createInstance(
    const retro_hw_render_context_negotiation_interface_vulkan& negotiation)
{
    // Use the loader linked by GBAStation so the host enumerates the packaged
    // KosmicKrisp ICD.  The existing Borealis window remains OpenGL.
    uint32_t extensionCount = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS)
        return false;

    const auto hasExtension = [&extensions](const char* name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [name](const VkExtensionProperties& extension) {
                               return std::strcmp(extension.extensionName, name) == 0;
                           });
    };
    if (!hasExtension(VK_KHR_SURFACE_EXTENSION_NAME) ||
        !hasExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME))
    {
        brls::Logger::error(
            "[LibretroVulkanHost] KosmicKrisp does not expose the required Metal surface extensions");
        return false;
    }
    const std::vector<const char*> enabledExtensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME};

    VkApplicationInfo defaultApplication{
        VK_STRUCTURE_TYPE_APPLICATION_INFO};
    defaultApplication.pApplicationName = "GBAStation";
    defaultApplication.applicationVersion = 1;
    defaultApplication.pEngineName = "GBAStation libretro Vulkan host";
    defaultApplication.engineVersion = 1;
    defaultApplication.apiVersion = VK_API_VERSION_1_1;
    const VkApplicationInfo* application = &defaultApplication;
    if (negotiation.get_application_info)
    {
        const VkApplicationInfo* requested = negotiation.get_application_info();
        if (requested)
            application = requested;
    }
    VkApplicationInfo applicationCopy = *application;

    VkInstanceCreateInfo instanceInfo{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &applicationCopy;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    instanceInfo.ppEnabledExtensionNames = enabledExtensions.data();
    // Do not enable VK_KHR_portability_enumeration.  KosmicKrisp is selected
    // as a real Vulkan ICD and does not require that portability-only path.
    result = vkCreateInstance(&instanceInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] vkCreateInstance failed: {} ({})",
            resultName(result), static_cast<int>(result));
        return false;
    }
    return true;
}

bool LibretroVulkanHost::createMetalSurface()
{
    m_metalLayer = createMetalLayer();
    if (!m_metalLayer)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] failed to create the macOS Metal layer for Vulkan");
        return false;
    }

    const auto createSurface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateMetalSurfaceEXT"));
    if (!createSurface)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] KosmicKrisp vkCreateMetalSurfaceEXT is unavailable");
        destroyMetalLayer(m_metalLayer);
        m_metalLayer = nullptr;
        return false;
    }

    VkMetalSurfaceCreateInfoEXT createInfo{
        VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    createInfo.pLayer = reinterpret_cast<const CAMetalLayer*>(m_metalLayer);
    const VkResult result = createSurface(m_instance, &createInfo, nullptr, &m_surface);
    if (result != VK_SUCCESS)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] vkCreateMetalSurfaceEXT failed: {} ({})",
            resultName(result), static_cast<int>(result));
        destroyMetalLayer(m_metalLayer);
        m_metalLayer = nullptr;
        return false;
    }
    return true;
}

bool LibretroVulkanHost::choosePhysicalDevice()
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0)
    {
        brls::Logger::error("[LibretroVulkanHost] no KosmicKrisp Vulkan physical device found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    if (result != VK_SUCCESS)
        return false;

    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);

        VkPhysicalDeviceDriverProperties driverProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &driverProperties;
        vkGetPhysicalDeviceProperties2(device, &properties2);
        const std::string driverName = driverProperties.driverName;
        if (!driverName.empty())
            brls::Logger::info(
                "[LibretroVulkanHost] Vulkan driver={} device={}",
                driverName, properties.deviceName);

        std::string loweredDriver = driverName;
        std::transform(loweredDriver.begin(), loweredDriver.end(), loweredDriver.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (loweredDriver.find("moltenvk") != std::string::npos)
            continue;

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, queues.data());
        for (uint32_t family = 0; family < queueCount; ++family)
        {
            VkBool32 present = VK_FALSE;
            if (m_surface != VK_NULL_HANDLE &&
                vkGetPhysicalDeviceSurfaceSupportKHR(device, family, m_surface, &present) !=
                    VK_SUCCESS)
                continue;
            const VkQueueFlags flags = queues[family].queueFlags;
            if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                continue;
            if (m_surface != VK_NULL_HANDLE && !present)
                continue;

            m_gpu = device;
            m_queueFamily = family;
            brls::Logger::info(
                "[LibretroVulkanHost] selected device={} vendor=0x{:04X}",
                properties.deviceName, properties.vendorID);
            return true;
        }
    }

    brls::Logger::error(
        "[LibretroVulkanHost] KosmicKrisp device has no graphics/compute/present queue");
    return false;
}

bool LibretroVulkanHost::createFallbackDevice()
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    const VkResult result = vkCreateDevice(m_gpu, &deviceInfo, nullptr, &m_device);
    if (result != VK_SUCCESS)
    {
        brls::Logger::error(
            "[LibretroVulkanHost] fallback vkCreateDevice failed: {} ({})",
            resultName(result), static_cast<int>(result));
        return false;
    }
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    return m_queue != VK_NULL_HANDLE;
}

uint32_t LibretroVulkanHost::findMemoryType(
    uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

bool LibretroVulkanHost::createReadbackResources(VkDeviceSize size)
{
    if (m_stagingBuffer != VK_NULL_HANDLE && m_stagingSize >= size)
        return true;

    destroyReadbackResources();

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_stagingBuffer);
    if (result != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, m_stagingBuffer, &requirements);
    const uint32_t memoryType = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX)
    {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(m_device, &allocation, nullptr, &m_stagingMemory);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
        return false;
    }
    result = vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingMemory, 0);
    if (result != VK_SUCCESS)
    {
        destroyReadbackResources();
        return false;
    }
    m_stagingSize = size;
    return true;
}

void LibretroVulkanHost::destroyReadbackResources()
{
    if (m_stagingBuffer != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
    if (m_stagingMemory != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
    m_stagingBuffer = VK_NULL_HANDLE;
    m_stagingMemory = VK_NULL_HANDLE;
    m_stagingSize = 0;
}

bool LibretroVulkanHost::readbackFrame(unsigned width, unsigned height,
                                       std::vector<uint32_t>& pixels)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (!isInitialized() || !m_imageValid ||
        m_image.create_info.image == VK_NULL_HANDLE || width == 0 || height == 0 ||
        width > kMaxReadbackDimension || height > kMaxReadbackDimension)
        return false;

    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    if (!createReadbackResources(size))
        return false;

    VkResult result = vkResetCommandBuffer(m_copyCommand, 0);
    if (result != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(m_copyCommand, &begin);
    if (result != VK_SUCCESS)
        return false;

    const VkImage image = m_image.create_info.image;
    const VkImageLayout originalLayout = m_image.image_layout;
    if (originalLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = originalLayout;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(m_copyCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &toTransfer);
    }

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = m_image.create_info.subresourceRange.baseMipLevel;
    copy.imageSubresource.baseArrayLayer = m_image.create_info.subresourceRange.baseArrayLayer;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(m_copyCommand, image,
                           originalLayout == VK_IMAGE_LAYOUT_GENERAL
                               ? VK_IMAGE_LAYOUT_GENERAL
                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &copy);

    if (originalLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VkImageMemoryBarrier fromTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        fromTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        fromTransfer.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        fromTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        fromTransfer.newLayout = originalLayout;
        fromTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fromTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fromTransfer.image = image;
        fromTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fromTransfer.subresourceRange.baseMipLevel =
            m_image.create_info.subresourceRange.baseMipLevel;
        fromTransfer.subresourceRange.levelCount = 1;
        fromTransfer.subresourceRange.baseArrayLayer =
            m_image.create_info.subresourceRange.baseArrayLayer;
        fromTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(m_copyCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &fromTransfer);
    }

    result = vkEndCommandBuffer(m_copyCommand);
    if (result != VK_SUCCESS)
        return false;

    std::vector<VkCommandBuffer> commands = m_pendingCommands;
    commands.push_back(m_copyCommand);
    std::vector<VkPipelineStageFlags> waitStages(m_waitSemaphores.size(),
                                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    // set_command_buffers and set_image are mutually exclusive for
    // synchronization purposes according to the libretro Vulkan ABI.
    if (m_pendingCommands.empty())
    {
        submit.waitSemaphoreCount = static_cast<uint32_t>(m_waitSemaphores.size());
        submit.pWaitSemaphores = m_waitSemaphores.data();
        submit.pWaitDstStageMask = waitStages.data();
    }
    submit.commandBufferCount = static_cast<uint32_t>(commands.size());
    submit.pCommandBuffers = commands.data();
    if (m_signalSemaphore != VK_NULL_HANDLE)
    {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &m_signalSemaphore;
    }

    // The cores used here submit to the negotiated queue.  Waiting before and
    // after our copy keeps this first implementation deterministic and avoids
    // touching the frontend OpenGL renderer or introducing a second queue.
    result = vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE);
    m_pendingCommands.clear();
    m_waitSemaphores.clear();
    m_signalSemaphore = VK_NULL_HANDLE;
    if (result != VK_SUCCESS || vkQueueWaitIdle(m_queue) != VK_SUCCESS)
        return false;

    void* mapped = nullptr;
    result = vkMapMemory(m_device, m_stagingMemory, 0, size, 0, &mapped);
    if (result != VK_SUCCESS || !mapped)
        return false;

    pixels.resize(static_cast<size_t>(width) * height);
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const VkFormat format = m_image.create_info.format;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM ||
                      format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        const uint8_t* pixel = bytes + i * 4;
        const uint8_t r = bgra ? pixel[2] : pixel[0];
        const uint8_t g = pixel[1];
        const uint8_t b = bgra ? pixel[0] : pixel[2];
        pixels[i] = static_cast<uint32_t>(r) |
                    (static_cast<uint32_t>(g) << 8) |
                    (static_cast<uint32_t>(b) << 16) | 0xFF000000u;
    }
    vkUnmapMemory(m_device, m_stagingMemory);
    return true;
}

void LibretroVulkanHost::setImageThunk(void* handle, const retro_vulkan_image* image,
                                       uint32_t numSemaphores,
                                       const VkSemaphore* semaphores,
                                       uint32_t srcQueueFamily)
{
    if (handle)
        static_cast<LibretroVulkanHost*>(handle)->setImage(
            image, numSemaphores, semaphores, srcQueueFamily);
}

void LibretroVulkanHost::setImage(const retro_vulkan_image* image,
                                  uint32_t numSemaphores,
                                  const VkSemaphore* semaphores,
                                  uint32_t srcQueueFamily)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_imageValid = image != nullptr;
    if (image)
        m_image = *image;
    if (semaphores && numSemaphores != 0)
        m_waitSemaphores.assign(semaphores, semaphores + numSemaphores);
    else
        m_waitSemaphores.clear();
}

uint32_t LibretroVulkanHost::getSyncIndexThunk(void* handle)
{
    return handle
        ? static_cast<LibretroVulkanHost*>(handle)->m_syncIndex.fetch_add(
              1, std::memory_order_relaxed) & 1u
        : 0;
}

uint32_t LibretroVulkanHost::getSyncIndexMaskThunk(void* handle)
{
    (void)handle;
    return 3;
}

void LibretroVulkanHost::setCommandBuffersThunk(void* handle, uint32_t numCmd,
                                                const VkCommandBuffer* commands)
{
    if (handle)
        static_cast<LibretroVulkanHost*>(handle)->setCommandBuffers(numCmd, commands);
}

void LibretroVulkanHost::setCommandBuffers(uint32_t numCmd,
                                           const VkCommandBuffer* commands)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (!commands || numCmd == 0)
    {
        m_pendingCommands.clear();
        return;
    }
    m_pendingCommands.assign(commands, commands + numCmd);
}

void LibretroVulkanHost::waitSyncIndexThunk(void* handle)
{
    if (handle)
        static_cast<LibretroVulkanHost*>(handle)->waitSyncIndex();
}

void LibretroVulkanHost::waitSyncIndex()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_queue != VK_NULL_HANDLE)
        vkQueueWaitIdle(m_queue);
}

void LibretroVulkanHost::lockQueueThunk(void* handle)
{
    if (handle)
        static_cast<LibretroVulkanHost*>(handle)->lockQueue();
}

void LibretroVulkanHost::lockQueue()
{
    m_queueMutex.lock();
}

void LibretroVulkanHost::unlockQueueThunk(void* handle)
{
    if (handle)
        static_cast<LibretroVulkanHost*>(handle)->unlockQueue();
}

void LibretroVulkanHost::unlockQueue()
{
    m_queueMutex.unlock();
}

void LibretroVulkanHost::setSignalSemaphoreThunk(void* handle, VkSemaphore semaphore)
{
    if (handle)
        static_cast<LibretroVulkanHost*>(handle)->setSignalSemaphore(semaphore);
}

void LibretroVulkanHost::setSignalSemaphore(VkSemaphore semaphore)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_signalSemaphore = semaphore;
}

} // namespace beiklive

#endif // defined(__APPLE__) && !defined(__SWITCH__)
