#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr uint32_t WIDTH = 64;
static constexpr uint32_t HEIGHT = 32;
static constexpr uint32_t MIP_LEVELS = 3;
static constexpr uint32_t ARRAY_LAYERS = 3;

class skip_test : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

static void check(VkResult result, const char *what) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(what) + " failed with VkResult " +
                             std::to_string(result));
}

static const char *format_name(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
    return "R8G8B8A8_UNORM";
  case VK_FORMAT_R8G8B8A8_SRGB:
    return "R8G8B8A8_SRGB";
  default:
    return "unknown";
  }
}

struct vulkan_context {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory_properties{};

  ~vulkan_context() {
    if (device)
      vkDestroyDevice(device, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
};

static std::vector<VkPhysicalDevice> enumerate_devices(VkInstance instance) {
  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
        "vkEnumeratePhysicalDevices(count)");
  if (!count)
    throw skip_test("the Vulkan loader reported no physical devices");

  std::vector<VkPhysicalDevice> devices(count);
  check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
        "vkEnumeratePhysicalDevices(list)");
  devices.resize(count);
  return devices;
}

static uint32_t find_transfer_queue(VkPhysicalDevice physical_device) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count,
                                           properties.data());

  for (uint32_t i = 0; i < count; ++i) {
    if (properties[i].queueCount &&
        (properties[i].queueFlags & VK_QUEUE_TRANSFER_BIT))
      return i;
  }
  throw skip_test("selected device has no transfer-capable queue family");
}

static void create_context(vulkan_context &context, uint32_t device_index) {
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "staged-color-vulkan";
  app_info.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &app_info;
  check(vkCreateInstance(&instance_info, nullptr, &context.instance),
        "vkCreateInstance");

  auto devices = enumerate_devices(context.instance);
  std::printf("Vulkan devices:\n");
  for (uint32_t i = 0; i < devices.size(); ++i) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices[i], &properties);
    std::printf("  [%u] %s (vendor %#x, device %#x)\n", i,
                properties.deviceName, properties.vendorID,
                properties.deviceID);
  }
  if (device_index >= devices.size())
    throw skip_test("requested Vulkan device index is out of range");

  context.physical_device = devices[device_index];
  vkGetPhysicalDeviceProperties(context.physical_device, &context.properties);
  vkGetPhysicalDeviceMemoryProperties(context.physical_device,
                                      &context.memory_properties);
  context.queue_family = find_transfer_queue(context.physical_device);

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = context.queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  check(vkCreateDevice(context.physical_device, &device_info, nullptr,
                       &context.device),
        "vkCreateDevice");
  vkGetDeviceQueue(context.device, context.queue_family, 0, &context.queue);

  std::printf("Selected [%u] %s, queue family %u\n", device_index,
              context.properties.deviceName, context.queue_family);
}

static std::optional<uint32_t>
find_memory_type(const vulkan_context &context, uint32_t bits,
                 VkMemoryPropertyFlags required,
                 VkMemoryPropertyFlags preferred) {
  std::optional<uint32_t> fallback;
  for (uint32_t i = 0; i < context.memory_properties.memoryTypeCount; ++i) {
    VkMemoryPropertyFlags flags =
        context.memory_properties.memoryTypes[i].propertyFlags;
    if (!(bits & (1u << i)) || (flags & required) != required)
      continue;
    if ((flags & preferred) == preferred)
      return i;
    if (!fallback)
      fallback = i;
  }
  return fallback;
}

struct subresource_data {
  uint32_t mip;
  uint32_t layer;
  uint32_t width;
  uint32_t height;
  VkDeviceSize offset;
  VkDeviceSize size;
};

static std::vector<subresource_data> make_subresources() {
  std::vector<subresource_data> result;
  VkDeviceSize offset = 0;
  for (uint32_t mip = 0; mip < MIP_LEVELS; ++mip) {
    uint32_t width = std::max(1u, WIDTH >> mip);
    uint32_t height = std::max(1u, HEIGHT >> mip);
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    for (uint32_t layer = 0; layer < ARRAY_LAYERS; ++layer) {
      result.push_back({mip, layer, width, height, offset, size});
      offset += size;
    }
  }
  return result;
}

static uint8_t expected_channel(VkFormat format, uint32_t cycle,
                                const subresource_data &subresource, uint32_t x,
                                uint32_t y, uint32_t channel) {
  uint32_t format_seed = format == VK_FORMAT_R8G8B8A8_SRGB ? 0x51u : 0x17u;
  return static_cast<uint8_t>(format_seed + cycle * 29u +
                              subresource.mip * 37u + subresource.layer * 53u +
                              x * 3u + y * 5u + channel * 71u);
}

struct test_resources {
  const vulkan_context &context;
  VkImage source = VK_NULL_HANDLE;
  VkDeviceMemory source_memory = VK_NULL_HANDLE;
  VkImage destination = VK_NULL_HANDLE;
  VkDeviceMemory destination_memory = VK_NULL_HANDLE;
  VkDeviceSize destination_allocation_size = 0;
  bool destination_coherent = false;
  VkBuffer upload = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory = VK_NULL_HANDLE;
  VkDeviceSize upload_allocation_size = 0;
  bool upload_coherent = false;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  explicit test_resources(const vulkan_context &context_) : context(context_) {}

  ~test_resources() {
    if (!context.device)
      return;
    vkDeviceWaitIdle(context.device);
    if (fence)
      vkDestroyFence(context.device, fence, nullptr);
    if (command_pool)
      vkDestroyCommandPool(context.device, command_pool, nullptr);
    if (upload)
      vkDestroyBuffer(context.device, upload, nullptr);
    if (upload_memory)
      vkFreeMemory(context.device, upload_memory, nullptr);
    if (destination)
      vkDestroyImage(context.device, destination, nullptr);
    if (destination_memory)
      vkFreeMemory(context.device, destination_memory, nullptr);
    if (source)
      vkDestroyImage(context.device, source, nullptr);
    if (source_memory)
      vkFreeMemory(context.device, source_memory, nullptr);
  }
};

static VkImage create_image(const vulkan_context &context, VkFormat format,
                            VkImageTiling tiling, VkImageUsageFlags usage) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {WIDTH, HEIGHT, 1};
  info.mipLevels = MIP_LEVELS;
  info.arrayLayers = ARRAY_LAYERS;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = tiling;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  check(vkCreateImage(context.device, &info, nullptr, &image), "vkCreateImage");
  return image;
}

static VkDeviceMemory allocate_image_memory(const vulkan_context &context,
                                            VkImage image,
                                            VkMemoryPropertyFlags required,
                                            VkMemoryPropertyFlags preferred,
                                            VkDeviceSize *allocation_size,
                                            bool *coherent) {
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(context.device, image, &requirements);
  auto type = find_memory_type(context, requirements.memoryTypeBits, required,
                               preferred);
  if (!type)
    throw skip_test(
        "no compatible memory type for the linear mapped destination image");

  VkMemoryAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  info.allocationSize = requirements.size;
  info.memoryTypeIndex = *type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  check(vkAllocateMemory(context.device, &info, nullptr, &memory),
        "vkAllocateMemory(image)");
  check(vkBindImageMemory(context.device, image, memory, 0),
        "vkBindImageMemory");
  if (allocation_size)
    *allocation_size = requirements.size;
  if (coherent)
    *coherent = context.memory_properties.memoryTypes[*type].propertyFlags &
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return memory;
}

static void create_upload_buffer(test_resources &resources, VkDeviceSize size) {
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(resources.context.device, &buffer_info, nullptr,
                       &resources.upload),
        "vkCreateBuffer(upload)");

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(resources.context.device, resources.upload,
                                &requirements);
  auto type = find_memory_type(resources.context, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!type)
    throw skip_test("no host-visible upload-buffer memory type");

  VkMemoryAllocateInfo allocation_info{};
  allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation_info.allocationSize = requirements.size;
  allocation_info.memoryTypeIndex = *type;
  check(vkAllocateMemory(resources.context.device, &allocation_info, nullptr,
                         &resources.upload_memory),
        "vkAllocateMemory(upload)");
  check(vkBindBufferMemory(resources.context.device, resources.upload,
                           resources.upload_memory, 0),
        "vkBindBufferMemory(upload)");
  resources.upload_allocation_size = requirements.size;
  resources.upload_coherent =
      resources.context.memory_properties.memoryTypes[*type].propertyFlags &
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

static void create_command_objects(test_resources &resources) {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = resources.context.queue_family;
  check(vkCreateCommandPool(resources.context.device, &pool_info, nullptr,
                            &resources.command_pool),
        "vkCreateCommandPool");

  VkCommandBufferAllocateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_info.commandPool = resources.command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  check(vkAllocateCommandBuffers(resources.context.device, &command_info,
                                 &resources.command_buffer),
        "vkAllocateCommandBuffers");

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  check(vkCreateFence(resources.context.device, &fence_info, nullptr,
                      &resources.fence),
        "vkCreateFence");
}

static void check_format_support(const vulkan_context &context,
                                 VkFormat format) {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(context.physical_device, format,
                                      &properties);

  bool optimal_source =
      properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  bool optimal_upload =
      properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  bool linear_destination =
      properties.linearTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  bool linear_sampled =
      properties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  bool linear_color =
      properties.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  std::printf("%s features: optimal transfer-src=%s transfer-dst=%s; "
              "linear transfer-dst=%s sampled=%s color-attachment=%s\n",
              format_name(format), optimal_source ? "yes" : "no",
              optimal_upload ? "yes" : "no", linear_destination ? "yes" : "no",
              linear_sampled ? "yes" : "no", linear_color ? "yes" : "no");
  std::printf("  Linear sampled/color-attachment support is informational; "
              "this transport only requires transfer-dst.\n");

  if (!optimal_source || !optimal_upload || !linear_destination)
    throw skip_test("required optimal transfer-src/transfer-dst or linear "
                    "transfer-dst feature is absent");

  VkImageFormatProperties image_properties{};
  VkResult result = vkGetPhysicalDeviceImageFormatProperties(
      context.physical_device, format, VK_IMAGE_TYPE_2D,
      VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
      &image_properties);
  if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
    throw skip_test("optimal source image parameters are unsupported");
  check(result, "vkGetPhysicalDeviceImageFormatProperties(optimal)");
  if (image_properties.maxMipLevels < MIP_LEVELS ||
      image_properties.maxArrayLayers < ARRAY_LAYERS ||
      image_properties.maxExtent.width < WIDTH ||
      image_properties.maxExtent.height < HEIGHT)
    throw skip_test("optimal source image limits are too small for the test");

  result = vkGetPhysicalDeviceImageFormatProperties(
      context.physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image_properties);
  if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
    throw skip_test(
        "linear transfer destination image parameters are unsupported");
  check(result, "vkGetPhysicalDeviceImageFormatProperties(linear)");
  if (image_properties.maxMipLevels < MIP_LEVELS ||
      image_properties.maxArrayLayers < ARRAY_LAYERS ||
      image_properties.maxExtent.width < WIDTH ||
      image_properties.maxExtent.height < HEIGHT)
    throw skip_test(
        "linear destination image limits are too small for the test");
}

static void fill_upload(test_resources &resources, VkFormat format,
                        uint32_t cycle,
                        const std::vector<subresource_data> &subresources) {
  void *mapped = nullptr;
  check(vkMapMemory(resources.context.device, resources.upload_memory, 0,
                    resources.upload_allocation_size, 0, &mapped),
        "vkMapMemory(upload)");
  auto *bytes = static_cast<uint8_t *>(mapped);
  for (const auto &subresource : subresources) {
    for (uint32_t y = 0; y < subresource.height; ++y) {
      for (uint32_t x = 0; x < subresource.width; ++x) {
        VkDeviceSize pixel =
            subresource.offset +
            (static_cast<VkDeviceSize>(y) * subresource.width + x) * 4;
        for (uint32_t channel = 0; channel < 4; ++channel)
          bytes[pixel + channel] =
              expected_channel(format, cycle, subresource, x, y, channel);
      }
    }
  }
  if (!resources.upload_coherent) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = resources.upload_memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    check(vkFlushMappedMemoryRanges(resources.context.device, 1, &range),
          "vkFlushMappedMemoryRanges(upload)");
  }
  vkUnmapMemory(resources.context.device, resources.upload_memory);
}

static VkImageMemoryBarrier
image_barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
              VkAccessFlags source_access, VkAccessFlags destination_access) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = destination_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = MIP_LEVELS;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = ARRAY_LAYERS;
  return barrier;
}

static void submit_copy(test_resources &resources, bool first_cycle,
                        const std::vector<subresource_data> &subresources) {
  check(vkResetCommandBuffer(resources.command_buffer, 0),
        "vkResetCommandBuffer");
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(resources.command_buffer, &begin_info),
        "vkBeginCommandBuffer");

  VkImageMemoryBarrier source_to_upload = image_barrier(
      resources.source,
      first_cycle ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      first_cycle ? 0
                  : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
      VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdPipelineBarrier(resources.command_buffer,
                       first_cycle ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                   : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &source_to_upload);

  std::vector<VkBufferImageCopy> upload_regions;
  upload_regions.reserve(subresources.size());
  for (const auto &subresource : subresources) {
    VkBufferImageCopy region{};
    region.bufferOffset = subresource.offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = subresource.mip;
    region.imageSubresource.baseArrayLayer = subresource.layer;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {subresource.width, subresource.height, 1};
    upload_regions.push_back(region);
  }
  vkCmdCopyBufferToImage(resources.command_buffer, resources.upload,
                         resources.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         upload_regions.size(), upload_regions.data());

  VkImageMemoryBarrier barriers[2] = {
      image_barrier(resources.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
      image_barrier(resources.destination,
                    first_cycle ? VK_IMAGE_LAYOUT_UNDEFINED
                                : VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    first_cycle ? 0 : VK_ACCESS_HOST_READ_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 2, barriers);

  std::vector<VkImageCopy> copy_regions;
  copy_regions.reserve(MIP_LEVELS);
  for (uint32_t mip = 0; mip < MIP_LEVELS; ++mip) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, ARRAY_LAYERS};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, ARRAY_LAYERS};
    region.extent = {std::max(1u, WIDTH >> mip), std::max(1u, HEIGHT >> mip),
                     1};
    copy_regions.push_back(region);
  }
  vkCmdCopyImage(resources.command_buffer, resources.source,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resources.destination,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_regions.size(),
                 copy_regions.data());

  barriers[0] =
      image_barrier(resources.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  barriers[1] =
      image_barrier(resources.destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_HOST_READ_BIT);
  vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
                           VK_PIPELINE_STAGE_HOST_BIT,
                       0, 0, nullptr, 0, nullptr, 2, barriers);
  check(vkEndCommandBuffer(resources.command_buffer), "vkEndCommandBuffer");

  check(vkResetFences(resources.context.device, 1, &resources.fence),
        "vkResetFences");
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &resources.command_buffer;
  check(vkQueueSubmit(resources.context.queue, 1, &submit, resources.fence),
        "vkQueueSubmit");
  check(vkWaitForFences(resources.context.device, 1, &resources.fence, VK_TRUE,
                        UINT64_MAX),
        "vkWaitForFences");
}

static void
validate_destination(test_resources &resources, VkFormat format, uint32_t cycle,
                     const std::vector<subresource_data> &subresources) {
  void *mapped = nullptr;
  check(vkMapMemory(resources.context.device, resources.destination_memory, 0,
                    resources.destination_allocation_size, 0, &mapped),
        "vkMapMemory(destination)");
  if (!resources.destination_coherent) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = resources.destination_memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    check(vkInvalidateMappedMemoryRanges(resources.context.device, 1, &range),
          "vkInvalidateMappedMemoryRanges(destination)");
  }

  const auto *bytes = static_cast<const uint8_t *>(mapped);
  for (const auto &subresource : subresources) {
    VkImageSubresource image_subresource{VK_IMAGE_ASPECT_COLOR_BIT,
                                         subresource.mip, subresource.layer};
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(resources.context.device, resources.destination,
                                &image_subresource, &layout);
    for (uint32_t y = 0; y < subresource.height; ++y) {
      for (uint32_t x = 0; x < subresource.width; ++x) {
        VkDeviceSize pixel = layout.offset +
                             static_cast<VkDeviceSize>(y) * layout.rowPitch +
                             x * 4;
        for (uint32_t channel = 0; channel < 4; ++channel) {
          uint8_t expected =
              expected_channel(format, cycle, subresource, x, y, channel);
          if (bytes[pixel + channel] != expected) {
            vkUnmapMemory(resources.context.device,
                          resources.destination_memory);
            throw std::runtime_error(
                "readback mismatch for " + std::string(format_name(format)) +
                ", cycle " + std::to_string(cycle) + ", mip " +
                std::to_string(subresource.mip) + ", layer " +
                std::to_string(subresource.layer) + ", pixel (" +
                std::to_string(x) + "," + std::to_string(y) + "), channel " +
                std::to_string(channel));
          }
        }
      }
    }
  }
  vkUnmapMemory(resources.context.device, resources.destination_memory);
}

static void run_lifecycle(const vulkan_context &context, VkFormat format,
                          const char *name, uint32_t cycles,
                          bool verify_static_persistence) {
  test_resources resources(context);
  auto subresources = make_subresources();
  VkDeviceSize upload_size =
      subresources.back().offset + subresources.back().size;

  resources.source = create_image(context, format, VK_IMAGE_TILING_OPTIMAL,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  resources.source_memory = allocate_image_memory(
      context, resources.source, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      nullptr, nullptr);
  resources.destination = create_image(context, format, VK_IMAGE_TILING_LINEAR,
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  resources.destination_memory = allocate_image_memory(
      context, resources.destination, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      &resources.destination_allocation_size, &resources.destination_coherent);
  create_upload_buffer(resources, upload_size);
  create_command_objects(resources);

  for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
    fill_upload(resources, format, cycle, subresources);
    submit_copy(resources, cycle == 0, subresources);
    validate_destination(resources, format, cycle, subresources);
  }
  if (verify_static_persistence)
    validate_destination(resources, format, cycles - 1, subresources);

  std::printf("PASS %-16s %-9s (%u copy cycle%s, %u mips, %u layers%s)\n",
              format_name(format), name, cycles, cycles == 1 ? "" : "s",
              MIP_LEVELS, ARRAY_LAYERS,
              verify_static_persistence ? ", persistent reread" : "");
}

int main(int argc, char **argv) {
  try {
    uint32_t device_index = 0;
    if (argc > 2) {
      std::fprintf(stderr, "usage: %s [physical-device-index]\n", argv[0]);
      return 2;
    }
    if (argc == 2)
      device_index = static_cast<uint32_t>(std::stoul(argv[1]));

    vulkan_context context;
    create_context(context, device_index);
    uint32_t passed = 0;
    uint32_t skipped = 0;
    for (VkFormat format :
         {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
      try {
        check_format_support(context, format);
        run_lifecycle(context, format, "repeated", 3, false);
        run_lifecycle(context, format, "static", 1, true);
        ++passed;
      } catch (const skip_test &error) {
        ++skipped;
        std::printf("SKIP %-16s %s\n", format_name(format), error.what());
      }
    }

    if (!passed) {
      std::printf("SKIP all formats (%u skipped)\n", skipped);
      return 77;
    }
    std::printf("PASS staged-color primitive (%u formats passed, %u skipped)\n",
                passed, skipped);
    return 0;
  } catch (const skip_test &error) {
    std::printf("SKIP %s\n", error.what());
    return 77;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
