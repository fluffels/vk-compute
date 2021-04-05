#include <Windows.h>

#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

LARGE_INTEGER counterEpoch;
LARGE_INTEGER counterFrequency;
FILE* logFile;

float GetElapsed() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    auto result =
        (t.QuadPart - counterEpoch.QuadPart)
        / (float)counterFrequency.QuadPart;
    return result;
}

#define TIME()\
    fprintf(logFile, "[%f]", GetElapsed());

#define LOC()\
    fprintf(logFile, "[%s:%d]", __FILE__, __LINE__);

#define LOG(level, ...)\
    LOC()\
    TIME()\
    fprintf(logFile, "[%s] ", level);\
    fprintf(logFile, __VA_ARGS__); \
    fprintf(logFile, "\n"); \
    fflush(logFile);

#define FATAL(...)\
    LOG("FATAL", __VA_ARGS__);\
    exit(1);

#define WARN(...) LOG("WARN", __VA_ARGS__);

#define ERR(...) LOG("ERROR", __VA_ARGS__);

#define INFO(...)\
    LOG("INFO", __VA_ARGS__)

#define CHECK(x, ...)\
    if (!x) { FATAL(__VA_ARGS__) }

#define LERROR(x) \
    if (x) { \
        char buffer[1024]; \
        strerror_s(buffer, errno); \
        FATAL(buffer); \
    }

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

#include "jcwk/MathLib.cpp"

#pragma pack(push, 1)
struct Uniforms {
    float proj[16];
    Vec4 eye;
    Quaternion rotation;
};
#pragma pack(pop)

#define READ(buffer, type, offset) (type*)(buffer + offset)

#define STB_DS_IMPLEMENTATION
#include "stb/stb_ds.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG 
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb/stb_image.h"

#ifdef WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#include "jcwk/FileSystem.cpp"
#include "jcwk/Win32/DirectInput.cpp"
#include "jcwk/Win32/Controller.cpp"
#include "jcwk/Win32/Mouse.cpp"
#define VULKAN_COMPUTE
#include "jcwk/Vulkan.cpp"
#include <vulkan/vulkan_win32.h>
#endif

const float DELTA_MOVE_PER_S = 100.f;
const float MOUSE_SENSITIVITY = 0.1f;
const float JOYSTICK_SENSITIVITY = 5;
bool keyboard[VK_OEM_CLEAR] = {};

LRESULT
WindowProc(
    HWND    window,
    UINT    message,
    WPARAM  wParam,
    LPARAM  lParam
) {
    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) PostQuitMessage(0);
            else keyboard[(uint16_t)wParam] = true;
            break;
        case WM_KEYUP:
            keyboard[(uint16_t)wParam] = false;
            break;
    }
    return DefWindowProc(window, message, wParam, lParam);
}

int
WinMain(
    HINSTANCE instance,
    HINSTANCE prevInstance,
    LPSTR commandLine,
    int showCommand
) {
    // NOTE: Initialize logging.
    {
        auto error = fopen_s(&logFile, "LOG", "w");
        if (error) return 1;

        QueryPerformanceCounter(&counterEpoch);
        QueryPerformanceFrequency(&counterFrequency);
    }

    // NOTE: Create window.
    HWND window = NULL;
    {
        WNDCLASSEX windowClassProperties = {};
        windowClassProperties.cbSize = sizeof(windowClassProperties);
        windowClassProperties.style = CS_HREDRAW | CS_VREDRAW;
        windowClassProperties.lpfnWndProc = WindowProc;
        windowClassProperties.hInstance = instance;
        windowClassProperties.lpszClassName = "MainWindowClass";
        ATOM windowClass = RegisterClassEx(&windowClassProperties);
        CHECK(windowClass, "Could not create window class");

        window = CreateWindowEx(
            0,
            "MainWindowClass",
            "Vulkan Computer Shader",
            WS_POPUP | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            800,
            800,
            NULL,
            NULL,
            instance,
            NULL
        );
        CHECK(window, "Could not create window");

        SetWindowPos(
            window,
            HWND_TOP,
            0,
            0,
            GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN),
            SWP_FRAMECHANGED
        );
        ShowCursor(FALSE);

        INFO("Window created");
    }

    // Create Vulkan instance.
    Vulkan vk;
    vk.extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    createVKInstance(vk);
    INFO("Vulkan instance created");

    // Create Windows surface.
    {
        VkWin32SurfaceCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = instance;
        createInfo.hwnd = window;

        auto result = vkCreateWin32SurfaceKHR(
            vk.handle,
            &createInfo,
            nullptr,
            &vk.swap.surface
        );
        VKCHECK(result, "could not create win32 surface");
        INFO("Surface created");
    }

    // Initialize Vulkan.
    initVK(vk);
    INFO("Vulkan initialized");

    // Init & execute compute shader.
    VulkanBuffer computedBuffer;
    const int computeWidth = 1920;
    const int computeHeight = 1080;
    const int computeSize = computeWidth * computeHeight * 4;
    {
        VulkanPipeline pipeline;
        initVKPipelineCompute(
            vk,
            "cs",
            pipeline
        );

        createComputeResultsBuffer(
            vk.device,
            vk.memories,
            vk.computeQueueFamily,
            computeSize,
            computedBuffer
        );
        updateStorageBuffer(
            vk.device,
            pipeline.descriptorSet,
            0,
            computedBuffer.handle
        );

        VkCommandPool pool = {};
        {
            VkCommandPoolCreateInfo create = {};
            create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            create.pNext = nullptr;
            create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            create.queueFamilyIndex = vk.computeQueueFamily;
            vkCreateCommandPool(
                vk.device,
                &create,
                nullptr,
                &pool
            );
        }

        VkCommandBuffer cmd = {};
        {
            VkCommandBufferAllocateInfo allocate = {};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.pNext = nullptr;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandPool = pool;
            allocate.commandBufferCount = 1;
            vkAllocateCommandBuffers(
                vk.device,
                &allocate,
                &cmd
            );
        }

        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.pNext = nullptr;
        begin.flags = 0;
        begin.pInheritanceInfo = nullptr;
        vkBeginCommandBuffer(cmd, &begin);

        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline.handle
        );
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline.layout,
            0, 1, &pipeline.descriptorSet,
            0, nullptr
        );

        vkCmdDispatch(
            cmd,
            computeWidth,
            computeHeight,
            1
        );

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = nullptr;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 0;
        submit.pSignalSemaphores = nullptr;
        submit.waitSemaphoreCount = 0;
        submit.pWaitSemaphores = 0;
        submit.pWaitDstStageMask = nullptr;

        vkQueueSubmit(
            vk.computeQueue,
            1,
            &submit,
            VK_NULL_HANDLE
        );

        // Might as well wait here since there will be nothing to display
        // otherwise.
        vkQueueWaitIdle(vk.computeQueue);

        // TODO: Record these and submit them in one.
        VkBufferMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.pNext = nullptr;
        barrier.buffer = computedBuffer.handle;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        barrier.srcQueueFamilyIndex = vk.computeQueueFamily;
        barrier.dstQueueFamilyIndex = vk.queueFamily;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = 0;

        vkBeginCommandBuffer(cmd, &begin);
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            0,
            0, nullptr,
            1, &barrier,
            0, nullptr
        );
        vkEndCommandBuffer(cmd);

        vkQueueSubmit(
            vk.computeQueue,
            1,
            &submit,
            VK_NULL_HANDLE
        );

        // Might as well wait here since there will be nothing to display
        // otherwise.
        vkQueueWaitIdle(vk.computeQueue);
    }

    {
        VkBufferMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.pNext = nullptr;
        barrier.buffer = computedBuffer.handle;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        barrier.srcQueueFamilyIndex = vk.computeQueueFamily;
        barrier.dstQueueFamilyIndex = vk.queueFamily;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkCommandBuffer cmd = {};
        createCommandBuffers(
            vk.device,
            vk.cmdPoolTransient,
            1,
            &cmd
        );
        beginCommandBuffer(cmd, 0);
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            0,
            0, nullptr,
            1, &barrier,
            0, nullptr
        );
        endCommandBuffer(cmd);

        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = nullptr;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 0;
        submit.pSignalSemaphores = nullptr;
        submit.waitSemaphoreCount = 0;
        submit.pWaitSemaphores = 0;
        submit.pWaitDstStageMask = nullptr;

        vkQueueSubmit(
            vk.queue,
            1,
            &submit,
            VK_NULL_HANDLE
        );
    }

    // Upload test texture.
    VulkanSampler computedSampler = {};
    createTextureFromBuffer(
        vk.device,
        vk.memories,
        vk.queue,
        vk.queueFamily,
        vk.cmdPoolTransient,
        32, 32, 32 * 32 * 4,
        computedBuffer,
        computedSampler
    );

    // Record command buffers.
    VkCommandBuffer* cmds = NULL;
    {
        float vertices[] = {
            -1, -1, 0,
            0, 0,

            1, 1, 0,
            1, 1,

            -1, 1, 0,
            0, 1,

            -1, -1, 0,
            0, 0,

            1, -1, 0,
            1, 0,

            1, 1, 0,
            1, 1
        };

        VulkanMesh mesh = {};
        uploadMesh(
            vk.device,
            vk.memories,
            vk.queueFamily,
            vertices,
            (3+2)*6*sizeof(float),
            mesh
        );

        VulkanPipeline defaultPipeline;
        initVKPipeline(
            vk,
            "default",
            defaultPipeline
        );

        updateCombinedImageSampler(
            vk.device,
            defaultPipeline.descriptorSet,
            0,
            &computedSampler,
            1
        );

        u32 framebufferCount = vk.swap.images.size();
        arrsetlen(cmds, framebufferCount);
        createCommandBuffers(vk.device, vk.cmdPool, framebufferCount, cmds);
        for (size_t swapIdx = 0; swapIdx < framebufferCount; swapIdx++) {
            auto& cmd = cmds[swapIdx];
            beginFrameCommandBuffer(cmd);

            VkClearValue colorClear;
            colorClear.color = {};
            VkClearValue depthClear;
            depthClear.depthStencil = { 1.f, 0 };
            VkClearValue clears[] = { colorClear, depthClear };

            VkRenderPassBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            beginInfo.clearValueCount = 2;
            beginInfo.pClearValues = clears;
            beginInfo.framebuffer = vk.swap.framebuffers[swapIdx];
            beginInfo.renderArea.extent = vk.swap.extent;
            beginInfo.renderArea.offset = {0, 0};
            beginInfo.renderPass = vk.renderPass;

            vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                defaultPipeline.handle
            );
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(
                cmd,
                0, 1,
                &mesh.vBuff.handle,
                offsets
            );
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                defaultPipeline.layout,
                0, 1,
                &defaultPipeline.descriptorSet,
                0, nullptr
            );
            vkCmdDraw(
                cmd,
                6,
                1,
                0,
                0
            );

            vkCmdEndRenderPass(cmd);

            VKCHECK(vkEndCommandBuffer(cmd));
        }
    }

    // Main loop.
    BOOL done = false;
    int errorCode = 0;
    while (!done) {
        MSG msg;
        BOOL messageAvailable; 
        do {
            messageAvailable = PeekMessage(
                &msg,
                (HWND)nullptr,
                0, 0,
                PM_REMOVE
            );
            TranslateMessage(&msg); 
            if (msg.message == WM_QUIT) {
                done = true;
                errorCode = (int)msg.wParam;
            }
            DispatchMessage(&msg); 
        } while(!done && messageAvailable);

        if (done) {
            break;
        }

        // Render frame.
        present(vk, cmds, 1);
    }
    arrfree(cmds);

    return errorCode;
}
