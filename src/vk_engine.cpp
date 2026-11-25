//> includes
#include <chrono>
#include <thread>
#include <iostream>
#include <string>
#include <windows.h>
#include <filesystem>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include "VkBootstrap.h"
#include "vk_images.h"
#include "vk_engine.h"
#include "vk_pipelines.h"

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

//< includes

//> init
constexpr bool bUseValidationLayers = false;

std::string getExeDirectory() {
    char buffer[MAX_PATH];
    // 获取 exe 的完整绝对路径，例如: C:\MyProject\build\Debug\App.exe
    GetModuleFileNameA(NULL, buffer, MAX_PATH);

    // 使用 std::filesystem 提取所在目录
    std::filesystem::path exePath(buffer);

    // parent_path() 会去掉 App.exe，只保留 C:\MyProject\build\Debug
    return exePath.parent_path().string();
}

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
    this->init_descriptors();
    this->init_pipelines();
    this->init_imgui();
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

            //清空帧对象
            this->_frames[i]._deletionQueue.flush();
        }

        //清空引擎对象
        this->_mainDeletionQueue.flush();

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
    // wait until the gpu has finished rendering the last frame. Timeout of 1 second
    VK_CHECK(vkWaitForFences(this->_logicDevice, 1, &get_current_frame()._renderFence, true, 1000000000));
    //清空帧对象
    get_current_frame()._deletionQueue.flush();
    VK_CHECK(vkResetFences(this->_logicDevice, 1, &get_current_frame()._renderFence));
    uint32_t swapchainImageIndex;
    //----------------------------------阻塞------------------------------------------------
    //阻塞结束拿到swapchainImageIndex后，get_current_frame()._swapchainSemaphore不一定有信号量，因为显示引擎可能正在扫描swapchainImageIndex这张图
    VK_CHECK(vkAcquireNextImageKHR(this->_logicDevice, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex));


    this->_drawExtent.width = this->_drawImage.imageExtent.width;
    this->_drawExtent.height = this->_drawImage.imageExtent.height;

    VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    //this->recordComputeCommandBuffer(cmd, swapchainImageIndex);
    this->recordGeomtryCommandBuffer(cmd, swapchainImageIndex);
    
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
            //send SDL event to imgui for handling
            ImGui_ImplSDL2_ProcessEvent(&e);
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        //some imgui UI to test
        this->drawImgui();
 
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


    // initialize the memory allocator
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = this->_physicalDevice;
    allocatorInfo.device = this->_logicDevice;
    allocatorInfo.instance = this->_instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &this->_allocator);

    _mainDeletionQueue.push_function([&]() {
        vmaDestroyAllocator(this->_allocator);
        });
}

void VulkanEngine::init_swapchain()
{
    this->create_swapchain(this->_windowExtent.width, this->_windowExtent.height);
    this->create_drawImage();
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

        VK_CHECK(vkCreateCommandPool(this->_logicDevice, &commandPoolInfo, nullptr, &this->_frames[i]._commandPool));
        // allocate the default command buffer that we will use for rendering
       /* VkCommandBufferAllocateInfo cmdAllocInfo = {};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = _frames[i]._commandPool;
        cmdAllocInfo.commandBufferCount = 1;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;*/
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(this->_frames[i]._commandPool);
        VK_CHECK(vkAllocateCommandBuffers(this->_logicDevice, &cmdAllocInfo, &this->_frames[i]._mainCommandBuffer));
    }


    VK_CHECK(vkCreateCommandPool(this->_logicDevice, &commandPoolInfo, nullptr, &this->_immCommandPool));

    // allocate the command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(this->_immCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(this->_logicDevice, &cmdAllocInfo, &this->_immCommandBuffer));

    this->_mainDeletionQueue.push_function([=]() {
        vkDestroyCommandPool(this->_logicDevice, this->_immCommandPool, nullptr);
        });
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
        VK_CHECK(vkCreateFence(this->_logicDevice, &fenceCreateInfo, nullptr, &this->_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(this->_logicDevice, &semaphoreCreateInfo, nullptr, &this->_frames[i]._swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(this->_logicDevice, &semaphoreCreateInfo, nullptr, &this->_frames[i]._renderSemaphore));
    }

    VK_CHECK(vkCreateFence(this->_logicDevice, &fenceCreateInfo, nullptr, &this->_immFence));
    this->_mainDeletionQueue.push_function([=]() { vkDestroyFence(this->_logicDevice, this->_immFence, nullptr); });
}

void VulkanEngine::init_descriptors()
{
    //create a descriptor pool that will hold 10 sets with 1 image each
    std::vector<DescriptorAllocator::PoolSizeRatio> sizes =
    {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }
    };

    this->_globalDescriptorAllocator.init_pool(this->_logicDevice, 10, sizes);

    //make the descriptor set layout for our compute draw
    uint32_t bindingNo = 0;
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(bindingNo, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        //VK_SHADER_STAGE_COMPUTE_BIT阶段使用
        this->_drawImageDescriptorLayout = builder.build(this->_logicDevice, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    //allocate a descriptor set for our draw image
    this->_drawImageDescriptorSet = this->_globalDescriptorAllocator.allocate(this->_logicDevice, this->_drawImageDescriptorLayout);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView = this->_drawImage.imageView;

    VkWriteDescriptorSet drawImageWrite = {};
    drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext = nullptr;
    drawImageWrite.dstBinding = bindingNo;
    drawImageWrite.dstSet = this->_drawImageDescriptorSet;
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(this->_logicDevice, 1, &drawImageWrite, 0, nullptr);

    //make sure both the descriptor allocator and the new layout get cleaned up properly
    this->_mainDeletionQueue.push_function([&]() {
        //描述符集_drawImageDescriptorSet是从描述符池VkDescriptorPool中Allocator，会跟随描述符池一起销毁；
        this->_globalDescriptorAllocator.destroy_pool(this->_logicDevice);
        vkDestroyDescriptorSetLayout(this->_logicDevice, this->_drawImageDescriptorLayout, nullptr);
        });
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

void VulkanEngine::recordGeomtryCommandBuffer(VkCommandBuffer cmd, uint32_t swapchainImageIndex)
{
    //添加一个标志VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT。这是可选的，但如果我们能够告知驱动程序此缓冲区只会提交和执行一次，我们的命令编码速度可能会略有提升
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    // transition our main draw image into general layout so we can write into it
    // we will overwrite it all so we dont care about what was the older layout
    vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    //glClear()
    this->draw_background(cmd);

    vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    this->draw_geometry(cmd);

    //transtion the draw image and the swapchain image into their correct transfer layouts
    vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmd, this->_drawImage.image, this->_swapchainImages[swapchainImageIndex], this->_drawExtent, this->_swapchainExtent);

    // set swapchain image layout to Attachment Optimal so we can draw it
    vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    //draw imgui into the swapchain image
    this->draw_imgui(cmd, this->_swapchainImageViews[swapchainImageIndex]);

    // set swapchain image layout to Present so we can draw it
    vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // set swapchain image layout to Present so we can show it on the screen

    //finalize the command buffer (we can no longer add commands, but it can now be executed)
    VK_CHECK(vkEndCommandBuffer(cmd));
}

void VulkanEngine::recordComputeCommandBuffer(VkCommandBuffer cmd, uint32_t swapchainImageIndex)
{
    //添加一个标志VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT。这是可选的，但如果我们能够告知驱动程序此缓冲区只会提交和执行一次，我们的命令编码速度可能会略有提升
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    //make the drawImage.image into writeable mode before rendering
    //处于 VK_IMAGE_LAYOUT_GENERAL 布局的图像可以在不转换布局的情况下切换不同用途
    vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

#if 0 //clear swapchainImage
    //make the swapchain image into writeable mode before rendering
    //处于 VK_IMAGE_LAYOUT_GENERAL 布局的图像可以在不转换布局的情况下切换不同用途
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
#endif // 0

    {
        //计算着色指令不需要renderpass
        ComputeEffect& effect = this->_backgroundEffects[this->_currentBackgroundEffect];

        // bind the background compute pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect._pipeline);

        // bind the descriptor set containing the draw image for the compute pipeline
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect._layout, 0, 1, &this->_drawImageDescriptorSet, 0, nullptr);

        vkCmdPushConstants(cmd, effect._layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect._data);
        // execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
        vkCmdDispatch(cmd, std::ceil(this->_drawExtent.width / 16.0), std::ceil(this->_drawExtent.height / 16.0), 1);
    }


    //transition the draw image and the swapchain image into their correct transfer layouts
    vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmd, this->_drawImage.image, this->_swapchainImages[swapchainImageIndex], this->_drawExtent, this->_swapchainExtent);

    // set swapchain image layout to Attachment Optimal so we can draw it
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    //draw imgui into the swapchain image
    this->draw_imgui(cmd, _swapchainImageViews[swapchainImageIndex]);

    // set swapchain image layout to Present so we can draw it
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // set swapchain image layout to Present so we can show it on the screen

    //vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    //finalize the command buffer (we can no longer add commands, but it can now be executed)
    VK_CHECK(vkEndCommandBuffer(cmd));

}

void VulkanEngine::create_drawImage()
{
    //draw image size will match the window
    VkExtent3D drawImageExtent = {
       this->_windowExtent.width,
       this->_windowExtent.height,
        1
    };

    //hardcoding the draw format to 32 bit float
    this->_drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    this->_drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    //for the draw image, we want to allocate it from gpu local memory
    VmaAllocationCreateInfo rimg_allocinfo = {};
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    //allocate and create the image
    vmaCreateImage(this->_allocator, &rimg_info, &rimg_allocinfo, &this->_drawImage.image, &this->_drawImage.allocation, nullptr);

    //build a image-view for the draw image to use for rendering
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(
        this->_drawImage.imageFormat,
        this->_drawImage.image,
        VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(this->_logicDevice, &rview_info, nullptr, &this->_drawImage.imageView));

    //add to deletion queues
    this->_mainDeletionQueue.push_function([=]() {
        vkDestroyImageView(this->_logicDevice, this->_drawImage.imageView, nullptr);
        vmaDestroyImage(this->_allocator, this->_drawImage.image, this->_drawImage.allocation);
        });
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
    //make a clear-color from frame number. This will flash with a 120 frame period.
    VkClearColorValue clearValue;
    float flash = std::abs(std::sin(this->_frameNumber / 120.f));
    clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    //clear image
    vkCmdClearColorImage(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmd)
{
    //begin a render pass  connected to our draw image
    //帧缓存附件--创建颜色附件-绑定imageView
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(this->_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(this->_drawExtent, &colorAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_trianglePipeline);

    //set dynamic viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = this->_drawExtent.width;
    viewport.height = this->_drawExtent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = this->_drawExtent.width;
    scissor.extent.height = this->_drawExtent.height;

    vkCmdSetScissor(cmd, 0, 1, &scissor);

    //launch a draw command to draw 3 vertices
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void VulkanEngine::init_pipelines()
{
    this->init_background_pipelines();
    this->init_triangle_pipeline();
}

void VulkanEngine::init_background_pipelines()
{
    //uniform变量-不是ubo
    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    
    computeLayout.setLayoutCount = 1;
    computeLayout.pSetLayouts = &this->_drawImageDescriptorLayout;

    computeLayout.pushConstantRangeCount = 1;
    computeLayout.pPushConstantRanges = &pushConstant;

    VK_CHECK(vkCreatePipelineLayout(this->_logicDevice, &computeLayout, nullptr, &this->_gradientPipelineLayout));

    ////////////////////////////
    {
        std::string exePath = getExeDirectory();

        std::string gradientShaderPath = exePath + "/shaders/gradient_color.comp.spv";
        std::string skyShaderPath = exePath + "/shaders/sky.comp.spv";

        VkShaderModule gradientShader;
        if (!vkutil::load_shader_module(gradientShaderPath.c_str(), this->_logicDevice, &gradientShader)) {
            fmt::print("Error when building the compute shader \n");
        }

        VkShaderModule skyShader;
        if (!vkutil::load_shader_module(skyShaderPath.c_str(), this->_logicDevice, &skyShader)) {
            fmt::print("Error when building the compute shader \n");
        }

        VkPipelineShaderStageCreateInfo stageinfo{};
        stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageinfo.pNext = nullptr;
        stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageinfo.module = gradientShader;
        stageinfo.pName = "main";

        VkComputePipelineCreateInfo computePipelineCreateInfo{};
        computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        computePipelineCreateInfo.pNext = nullptr;
        computePipelineCreateInfo.layout = this->_gradientPipelineLayout;
        computePipelineCreateInfo.stage = stageinfo;

        ComputeEffect gradient;
        gradient._layout = this->_gradientPipelineLayout;
        gradient._name = "gradient";
        gradient._data = {};

        //default colors
        gradient._data.data1 = glm::vec4(1, 0, 0, 1);
        gradient._data.data2 = glm::vec4(0, 0, 1, 1);

        VK_CHECK(vkCreateComputePipelines(this->_logicDevice, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient._pipeline));

        //change the shader module only to create the sky shader
        computePipelineCreateInfo.stage.module = skyShader;

        ComputeEffect sky;
        sky._layout = this->_gradientPipelineLayout;
        sky._name = "sky";
        sky._data = {};
        //default sky parameters
        sky._data.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);


        VK_CHECK(vkCreateComputePipelines(this->_logicDevice, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky._pipeline));

        //add the 2 background effects into the array
        this->_backgroundEffects.push_back(gradient);
        this->_backgroundEffects.push_back(sky);

        //destroy structures properly
        vkDestroyShaderModule(this->_logicDevice, gradientShader, nullptr);
        vkDestroyShaderModule(this->_logicDevice, skyShader, nullptr);
        this->_mainDeletionQueue.push_function([=]() {
            vkDestroyPipelineLayout(this->_logicDevice, this->_gradientPipelineLayout, nullptr);
            vkDestroyPipeline(this->_logicDevice, sky._pipeline, nullptr);
            vkDestroyPipeline(this->_logicDevice, gradient._pipeline, nullptr);
            });
    }
    //////////////////////////


#if 0

    std::string exePath = getExeDirectory();
    std::string shaderPath = exePath + "/shaders/gradient_color.comp.spv";
    //layout code
    VkShaderModule computeDrawShader;
    if (!vkutil::load_shader_module(shaderPath.c_str(), this->_logicDevice, &computeDrawShader))
    {
        fmt::print("Error when building the compute shader \n");
    }

    VkPipelineShaderStageCreateInfo stageinfo{};
    stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageinfo.pNext = nullptr;
    stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageinfo.module = computeDrawShader;
    stageinfo.pName = "main";

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.stage = stageinfo;
    computePipelineCreateInfo.layout = this->_gradientPipelineLayout;

    VK_CHECK(vkCreateComputePipelines(this->_logicDevice, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &this->_gradientPipeline));

#endif // 0
}

void VulkanEngine::init_triangle_pipeline()
{
    std::string exePath = getExeDirectory();
    std::string triangleVs = exePath + "/shaders/colored_triangle.frag.spv";
    VkShaderModule triangleFragShader;
    if (!vkutil::load_shader_module(triangleVs.c_str(), this->_logicDevice, &triangleFragShader)) {
        fmt::print("Error when building the triangle fragment shader module");
    }
    else {
        fmt::print("Triangle fragment shader succesfully loaded");
    }
    std::string triangleFs = exePath + "/shaders/colored_triangle.vert.spv";

    VkShaderModule triangleVertexShader;
    if (!vkutil::load_shader_module(triangleFs.c_str(), this->_logicDevice, &triangleVertexShader)) {
        fmt::print("Error when building the triangle vertex shader module");
    }
    else {
        fmt::print("Triangle vertex shader succesfully loaded");
    }

    //build the pipeline layout that controls the inputs/outputs of the shader
    //we are not using descriptor sets or other systems yet, so no need to use anything other than empty default
    VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
    VK_CHECK(vkCreatePipelineLayout(this->_logicDevice, &pipeline_layout_info, nullptr, &this->_trianglePipelineLayout));


    PipelineBuilder pipelineBuilder;

    //use the triangle layout we created
    pipelineBuilder._pipelineLayout = this->_trianglePipelineLayout;
    //connecting the vertex and pixel shaders to the pipeline
    pipelineBuilder.set_shaders(triangleVertexShader, triangleFragShader);
    //it will draw triangles
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    //filled triangles
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    //no backface culling
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    //no multisampling
    pipelineBuilder.set_multisampling_none();
    //no blending
    pipelineBuilder.disable_blending();
    //no depth testing
    pipelineBuilder.disable_depthtest();

    //connect the image format we will draw into, from draw image
    pipelineBuilder.set_color_attachment_format(_drawImage.imageFormat);
    pipelineBuilder.set_depth_format(VK_FORMAT_UNDEFINED);

    //finally build the pipeline
    this->_trianglePipeline = pipelineBuilder.build_pipeline(this->_logicDevice);

    //clean structures
    vkDestroyShaderModule(this->_logicDevice, triangleFragShader, nullptr);
    vkDestroyShaderModule(this->_logicDevice, triangleVertexShader, nullptr);

    this->_mainDeletionQueue.push_function([&]() {
        vkDestroyPipelineLayout(this->_logicDevice, this->_trianglePipelineLayout, nullptr);
        vkDestroyPipeline(this->_logicDevice, this->_trianglePipeline, nullptr);
        });

}

void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VK_CHECK(vkResetFences(this->_logicDevice, 1, &this->_immFence));
    VK_CHECK(vkResetCommandBuffer(this->_immCommandBuffer, 0));

    VkCommandBuffer cmd = this->_immCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    function(cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);

    // submit command buffer to the queue and execute it.
    //  _renderFence will now block until the graphic commands finish execution
    VK_CHECK(vkQueueSubmit2(this->_graphicsQueue, 1, &submit, this->_immFence));

    VK_CHECK(vkWaitForFences(this->_logicDevice, 1, &this->_immFence, true, 9999999999));
}

void VulkanEngine::init_imgui()
{
    // 1: create descriptor pool for IMGUI
    //  the size of the pool is very oversize, but it's copied from imgui demo
    //  itself.

    uint32_t maxSets = 1000;
    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, maxSets },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSets },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSets },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxSets },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxSets },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxSets },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxSets },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, maxSets },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxSets } };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = maxSets;
    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(this->_logicDevice, &pool_info, nullptr, &imguiPool));


    // 2: initialize imgui library

    // this initializes the core structures of imgui
    ImGui::CreateContext();

    // this initializes imgui for SDL
    ImGui_ImplSDL2_InitForVulkan(this->_window);

    // this initializes imgui for Vulkan
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = _instance;
    init_info.PhysicalDevice = this->_physicalDevice;
    init_info.Device = this->_logicDevice;
    init_info.Queue = this->_graphicsQueue;
    init_info.DescriptorPool = imguiPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    //不使用 Vulkan 渲染通道，而是使用动态渲染；与计算着色器不同，我们将直接在交换链中绘制 Dear ImGui。
    //dynamic rendering parameters for imgui to use
    init_info.UseDynamicRendering = true;
    init_info.PipelineRenderingCreateInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &this->_swapchainImageFormat;


    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info);

    ImGui_ImplVulkan_CreateFontsTexture();

    // add the destroy the imgui created structures
    _mainDeletionQueue.push_function([=]() {
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(this->_logicDevice, imguiPool, nullptr);
        });
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
    //帧缓存附件--创建颜色附件-绑定imageView
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    //类似于帧缓存-绑定附件
    VkRenderingInfo renderInfo = vkinit::rendering_info(this->_swapchainExtent, &colorAttachment, nullptr);
    //绑定帧缓存
    vkCmdBeginRendering(cmd, &renderInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

void VulkanEngine::drawImgui()
{
    // imgui new frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    //some imgui UI to test
    this->drawUI();
    //make imgui calculate internal draw structures
    ImGui::Render();
}

void VulkanEngine::drawUI()
{
    //some imgui UI to test
    ImGui::ShowDemoWindow();


    if (ImGui::Begin("background")) {

        ComputeEffect& selected = this->_backgroundEffects[this->_currentBackgroundEffect];

        ImGui::Text("Selected effect: ", selected._name);

        ImGui::SliderInt("Effect Index", &this->_currentBackgroundEffect, 0, this->_backgroundEffects.size() - 1);

        ImGui::InputFloat4("data1", (float*)&selected._data.data1);
        ImGui::InputFloat4("data2", (float*)&selected._data.data2);
        ImGui::InputFloat4("data3", (float*)&selected._data.data3);
        ImGui::InputFloat4("data4", (float*)&selected._data.data4);
    }
    ImGui::End();
}