//> includes
#include "vk_engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include "VkBootstrap.h"
#include "vk_images.h"

#include <chrono>
#include <thread>
//< includes

//> init
constexpr bool bUseValidationLayers = false;

VulkanEngine* loadedEngine = nullptr;
VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);

    // everything went fine

    this->init_vulkan();
    this->init_swapchain();
    this->init_commands();
    this->init_sync_structures();

    _isInitialized = true;
}
//< init

//初始化顺序是 SDL Window -> Instance -> Surface -> physicalDevice -> Device -> Swapchain，所以我们销毁的顺序正好相反。
void VulkanEngine::cleanup()
{
    if (_isInitialized) {

        //cpu阻塞，确保GPU全部指令执行完毕，处于闲置（idle）状态，cpu才继续运行，执行程序退出
        vkDeviceWaitIdle(this->_logicDevice);

        for (int i = 0; i < FRAME_OVERLAP; i++) {
            //commandBuffer是从commandpool allocate，commandBuffer会随commandpool一并释放
            //销毁CommandPool会销毁所有从该命令池分配的命令缓冲区
            vkDestroyCommandPool(this->_logicDevice, this->_frames[i]._commandPool, nullptr);
            //destroy sync objects
            vkDestroyFence(this->_logicDevice, this->_frames[i]._renderFence, nullptr);
            vkDestroySemaphore(this->_logicDevice, this->_frames[i]._renderSemaphore, nullptr);
            vkDestroySemaphore(this->_logicDevice, this->_frames[i]._swapchainSemaphore, nullptr);
        }

        destroy_swapchain();

        vkDestroySurfaceKHR(this->_instance, this->_surface, nullptr);
        vkDestroyDevice(this->_logicDevice, nullptr);
        
        //VkPhysicalDevice无法销毁，因为它本身不是 Vulkan 资源，它更像是系统中 GPU 的一个句柄
        //VkQueue(_graphicsQueue)也无法被销毁，跟VkPhysicalDevice一样，更像是系统中 GPU 的一个句柄
        vkb::destroy_debug_utils_messenger(this->_instance, this->_debug_messenger);
        vkDestroyInstance(this->_instance, nullptr);
        SDL_DestroyWindow(this->_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

void VulkanEngine::draw()
{
    // wait until the gpu has finished rendering the last frame. Timeout of 1
        // second
    VK_CHECK(vkWaitForFences(this->_logicDevice, 1, &get_current_frame()._renderFence, true, 1000000000));
    VK_CHECK(vkResetFences(this->_logicDevice, 1, &get_current_frame()._renderFence));
    uint32_t swapchainImageIndex;
    //----------------------------------阻塞------------------------------------------------
    //阻塞结束拿到swapchainImageIndex后，get_current_frame()._swapchainSemaphore不一定有信号量，因为显示引擎可能正在扫描swapchainImageIndex这张图
    VK_CHECK(vkAcquireNextImageKHR(this->_logicDevice, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex));


    VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    //添加一个标志VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT。这是可选的，但如果我们能够告知驱动程序此缓冲区只会提交和执行一次，我们的命令编码速度可能会略有提升
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));


    //make the swapchain image into writeable mode before rendering
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    //make a clear-color from frame number. This will flash with a 120 frame period.
    VkClearColorValue clearValue;
    float flash = std::abs(std::sin(_frameNumber / 120.f));
    clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    //clear image
    vkCmdClearColorImage(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);

    //make the swapchain image into presentable mode
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    //finalize the command buffer (we can no longer add commands, but it can now be executed)
    VK_CHECK(vkEndCommandBuffer(cmd));
 

    //prepare the submission to the queue. 
    //we want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
    //we will signal the _renderSemaphore, to signal that rendering has finished

    //被提交等待绘制的命令缓冲区
    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);

    //等待的信号量及等待阶段
    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, get_current_frame()._swapchainSemaphore);
    //绘制完毕后触发的信号量
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, get_current_frame()._renderSemaphore);
    //打包提交信息
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);

    //submit command buffer to the queue and execute it.
    // _renderFence will now block until the graphic commands finish execution
    VK_CHECK(vkQueueSubmit2(this->_graphicsQueue, 1, &submit, get_current_frame()._renderFence));


    //prepare present
    // this will put the image we just rendered to into the visible window.
    // we want to wait on the _renderSemaphore for that, 
    // as its necessary that drawing commands have finished before the image is displayed to the user
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;

    presentInfo.pWaitSemaphores = &get_current_frame()._renderSemaphore;
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));

    //increase the number of frames drawn
    _frameNumber++;
}
//< extras

//> drawloop
void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        draw();
    }
}
//< drawloop

void VulkanEngine::init_vulkan()
{
    vkb::InstanceBuilder builder;

    //make the vulkan instance, with basic debug features
    auto inst_ret = builder.set_app_name("Example Vulkan Application")
        .request_validation_layers(bUseValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();

    vkb::Instance vkb_inst = inst_ret.value();

    //grab the instance 
    this->_instance = vkb_inst.instance;
    this->_debug_messenger = vkb_inst.debug_messenger;

    SDL_Vulkan_CreateSurface(this->_window, this->_instance, &this->_surface);

    //vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features.dynamicRendering = true;//动态渲染允许我们完全跳过渲染通道/帧缓冲区
    features.synchronization2 = true;

    //vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.bufferDeviceAddress = true;//缓冲区设备地址允许我们使用 GPU 指针而无需绑定缓冲区
    features12.descriptorIndexing = true;//描述符索引则允许我们使用无绑定纹理


    //use vkbootstrap to select a gpu. 
    //We want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_surface(this->_surface)
        .select()
        .value();

    //create the final vulkan device
    vkb::DeviceBuilder deviceBuilder{ physicalDevice };
    vkb::Device vkbDevice = deviceBuilder.build().value();

    // Get the VkDevice handle used in the rest of a vulkan application
    this->_physicalDevice = physicalDevice.physical_device;
    this->_logicDevice = vkbDevice.device;

    //use vkbootstrap to get a Graphics queue
    this->_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    this->_graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
}

void VulkanEngine::init_swapchain()
{
    this->create_swapchain(_windowExtent.width, _windowExtent.height);
}

void VulkanEngine::init_commands()
{
    //create a command pool for commands submitted to the graphics queue.
    //we also want the pool to allow for resetting of individual command buffers
    /*VkCommandPoolCreateInfo commandPoolInfo = {};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.pNext = nullptr;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = _graphicsQueueFamily;*/
    VkCommandPoolCreateInfo  commandPoolInfo = vkinit::command_pool_create_info(
        this->_graphicsQueueFamily, 
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for (int i = 0; i < FRAME_OVERLAP; i++) {

        VK_CHECK(vkCreateCommandPool(this->_logicDevice, &commandPoolInfo, nullptr, &_frames[i]._commandPool));
        // allocate the default command buffer that we will use for rendering
       /* VkCommandBufferAllocateInfo cmdAllocInfo = {};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = _frames[i]._commandPool;
        cmdAllocInfo.commandBufferCount = 1;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;*/
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool);
        VK_CHECK(vkAllocateCommandBuffers(this->_logicDevice, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }
}
void VulkanEngine::init_sync_structures()
{
    //create syncronization structures
    //one fence to control when the gpu has finished rendering the frame,
    //and 2 semaphores to syncronize rendering with swapchain
    //we want the fence to start signalled so we can wait on it on the first frame
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);//初始化时带有信号量，防止第一帧无限等待
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    for (int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateFence(this->_logicDevice, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(this->_logicDevice, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(this->_logicDevice, &semaphoreCreateInfo, nullptr, &_frames[i]._renderSemaphore));
    }
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ this->_physicalDevice,this->_logicDevice,this->_surface };

    this->_swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        //.use_default_format_selection()
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        //use vsync present mode
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    this->_swapchainExtent = vkbSwapchain.extent;
    //store swapchain and its related images
    this->_swapchain = vkbSwapchain.swapchain;
    this->_swapchainImages = vkbSwapchain.get_images().value();
    this->_swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::destroy_swapchain()
{
    //删除交换链将一并删除内部的swapChainImage
    vkDestroySwapchainKHR(this->_logicDevice, this->_swapchain, nullptr);

    // destroy swapchain resources
    for (int i = 0; i < this->_swapchainImageViews.size(); i++) {

        vkDestroyImageView(this->_logicDevice, this->_swapchainImageViews[i], nullptr);
    }
}