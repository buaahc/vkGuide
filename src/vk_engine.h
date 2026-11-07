// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.
//> intro
#pragma once

#include <vk_types.h>

class VulkanEngine {
public:

	bool _isInitialized{ false };
	int _frameNumber {0};
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };


	struct SDL_Window* _window{ nullptr };

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	//run main loop
	void run();

private:
	VkInstance _instance = VK_NULL_HANDLE;// Vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger = VK_NULL_HANDLE;// Vulkan debug output handle
	VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;// GPU chosen as the default device
	VkDevice _logicDevice = VK_NULL_HANDLE; // Vulkan device for commands
	VkSurfaceKHR _surface = VK_NULL_HANDLE;// Vulkan window surface

	VkSwapchainKHR _swapchain;
	VkFormat _swapchainImageFormat;

	std::vector<VkImage> _swapchainImages;
	std::vector<VkImageView> _swapchainImageViews;
	VkExtent2D _swapchainExtent;

private:
	void init_vulkan();
	void init_swapchain();
	void init_commands();
	void init_sync_structures();

	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();
};
//< intro

