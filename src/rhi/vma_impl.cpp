// Single translation unit that compiles the VulkanMemoryAllocator
// implementation. Built with warnings disabled (see CMakeLists) because VMA's
// implementation is not clean under -Wall -Wextra -Wshadow; our own code that
// *uses* VMA stays strict.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
