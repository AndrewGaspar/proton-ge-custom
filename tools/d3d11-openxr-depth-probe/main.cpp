#define WIN32_LEAN_AND_MEAN
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include "wineopenxr.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

template <typename T>
static void
release(T *& object)
{
    if (object)
        object->Release();
    object = nullptr;
}

static bool
xr_ok(XrResult result, const char * call)
{
    if (XR_SUCCEEDED(result))
        return true;
    std::fprintf(stderr, "FAIL: %s returned %d\n", call, result);
    return false;
}

static bool
hr_ok(HRESULT result, const char * call)
{
    if (SUCCEEDED(result))
        return true;
    std::fprintf(stderr, "FAIL: %s returned 0x%08lx\n", call, static_cast<unsigned long>(result));
    return false;
}

#define XR_CHECK(CALL)                      \
    do                                      \
    {                                       \
        if (!xr_ok((CALL), #CALL))          \
            return 1;                       \
    } while (false)

#define HR_CHECK(CALL)                      \
    do                                      \
    {                                       \
        if (!hr_ok((CALL), #CALL))          \
            return 1;                       \
    } while (false)

struct SXrFunctions
{
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireImage = nullptr;
    PFN_xrWaitSwapchainImage waitImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseImage = nullptr;
};

template <typename T>
static bool
load_xr(SXrFunctions & xr, XrInstance instance, const char * name, T & function)
{
    return xr_ok(xr.getInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&function)), name) &&
           function;
}

static bool
luid_equal(const LUID & a, const LUID & b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static int
create_matching_device(const LUID & wantedLuid, ID3D11Device ** device, ID3D11DeviceContext ** context)
{
    IDXGIFactory1 * factory = nullptr;
    HR_CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)));

    IDXGIAdapter1 * selected = nullptr;
    for (UINT index = 0;; index++)
    {
        IDXGIAdapter1 * adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::printf("adapter[%u]: LUID=%08lx:%08lx flags=0x%x\n",
                    index,
                    static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                    static_cast<unsigned long>(desc.AdapterLuid.LowPart),
                    desc.Flags);
        if (luid_equal(desc.AdapterLuid, wantedLuid))
        {
            selected = adapter;
            break;
        }
        adapter->Release();
    }

    if (!selected)
    {
        std::fprintf(stderr,
                     "FAIL: no DXGI adapter matches runtime LUID %08lx:%08lx\n",
                     static_cast<unsigned long>(wantedLuid.HighPart),
                     static_cast<unsigned long>(wantedLuid.LowPart));
        factory->Release();
        return 1;
    }

    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT result = D3D11CreateDevice(selected,
                                             D3D_DRIVER_TYPE_UNKNOWN,
                                             nullptr,
                                             0,
                                             requested,
                                             ARRAYSIZE(requested),
                                             D3D11_SDK_VERSION,
                                             device,
                                             &featureLevel,
                                             context);
    selected->Release();
    factory->Release();
    HR_CHECK(result);
    std::printf("D3D11 feature level: 0x%x\n", static_cast<unsigned>(featureLevel));
    return 0;
}

static bool
wait_for_gpu(ID3D11Device * device, ID3D11DeviceContext * context)
{
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
    ID3D11Query * query = nullptr;
    if (!hr_ok(device->CreateQuery(&queryDesc, &query), "ID3D11Device::CreateQuery"))
        return false;

    context->End(query);
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    HRESULT result = S_FALSE;
    while (result == S_FALSE && std::chrono::steady_clock::now() < deadline)
    {
        result = context->GetData(query, nullptr, 0, 0);
        if (result == S_FALSE)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    query->Release();
    if (result == S_FALSE)
    {
        std::fprintf(stderr, "FAIL: D3D11 event query timed out after five seconds\n");
        return false;
    }
    return hr_ok(result, "ID3D11DeviceContext::GetData");
}

int main(int argc, char ** argv)
{
    const char * stage = argc > 1 ? argv[1] : "enumerate";
    if (std::strcmp(stage, "enumerate") != 0 && std::strcmp(stage, "views") != 0 &&
        std::strcmp(stage, "clear") != 0 && std::strcmp(stage, "sample") != 0)
    {
        std::fprintf(stderr, "usage: %s [enumerate|views|clear|sample]\n", argv[0]);
        return 2;
    }

    HMODULE loader = LoadLibraryA("openxr_loader.dll");
    if (!loader)
    {
        std::fprintf(stderr, "FAIL: LoadLibrary(openxr_loader.dll) returned %lu\n", GetLastError());
        return 1;
    }

    SXrFunctions xr{};
    xr.getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!xr.getInstanceProcAddr || !load_xr(xr, XR_NULL_HANDLE, "xrCreateInstance", xr.createInstance))
    {
        std::fprintf(stderr, "FAIL: OpenXR loader has no global entry points\n");
        return 1;
    }

    const char * extensions[]{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(instanceInfo.applicationInfo.applicationName,
                 "d3d11-openxr-depth-probe",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(instanceInfo.applicationInfo.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instanceInfo.enabledExtensionCount = ARRAYSIZE(extensions);
    instanceInfo.enabledExtensionNames = extensions;
    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xr.createInstance(&instanceInfo, &instance));

    if (!load_xr(xr, instance, "xrDestroyInstance", xr.destroyInstance) ||
        !load_xr(xr, instance, "xrGetSystem", xr.getSystem) ||
        !load_xr(xr, instance, "xrGetD3D11GraphicsRequirementsKHR", xr.getD3D11Requirements) ||
        !load_xr(xr, instance, "xrCreateSession", xr.createSession) ||
        !load_xr(xr, instance, "xrDestroySession", xr.destroySession) ||
        !load_xr(xr, instance, "xrEnumerateSwapchainFormats", xr.enumerateFormats) ||
        !load_xr(xr, instance, "xrCreateSwapchain", xr.createSwapchain) ||
        !load_xr(xr, instance, "xrDestroySwapchain", xr.destroySwapchain) ||
        !load_xr(xr, instance, "xrEnumerateSwapchainImages", xr.enumerateImages) ||
        !load_xr(xr, instance, "xrAcquireSwapchainImage", xr.acquireImage) ||
        !load_xr(xr, instance, "xrWaitSwapchainImage", xr.waitImage) ||
        !load_xr(xr, instance, "xrReleaseSwapchainImage", xr.releaseImage))
        return 1;

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XR_CHECK(xr.getSystem(instance, &systemInfo, &system));

    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(xr.getD3D11Requirements(instance, system, &requirements));
    std::printf("runtime adapter LUID=%08lx:%08lx minimum feature level=0x%x\n",
                static_cast<unsigned long>(requirements.adapterLuid.HighPart),
                static_cast<unsigned long>(requirements.adapterLuid.LowPart),
                static_cast<unsigned>(requirements.minFeatureLevel));

    ID3D11Device * device = nullptr;
    ID3D11DeviceContext * context = nullptr;
    if (create_matching_device(requirements.adapterLuid, &device, &context) != 0)
        return 1;

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xr.createSession(instance, &sessionInfo, &session));

    uint32_t formatCount = 0;
    XR_CHECK(xr.enumerateFormats(session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xr.enumerateFormats(session, formatCount, &formatCount, formats.data()));
    bool hasD32S8 = false;
    for (const int64_t format: formats)
        hasD32S8 |= format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if (!hasD32S8)
    {
        std::fprintf(stderr, "FAIL: DXGI_FORMAT_D32_FLOAT_S8X24_UINT is not advertised\n");
        return 1;
    }

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags =
        XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = 2064;
    swapchainInfo.height = 2208;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XR_CHECK(xr.createSwapchain(session, &swapchainInfo, &swapchain));

    uint32_t imageCount = 0;
    XR_CHECK(xr.enumerateImages(swapchain, 0, &imageCount, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> images(imageCount,
                                                 XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xr.enumerateImages(swapchain,
                                imageCount,
                                &imageCount,
                                reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())));
    std::printf("enumerated %u D3D11 images\n", imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        D3D11_TEXTURE2D_DESC desc{};
        images[i].texture->GetDesc(&desc);
        std::printf("image[%u]: %ux%u mips=%u layers=%u format=%u samples=%u bind=0x%x usage=%u misc=0x%x\n",
                    i,
                    desc.Width,
                    desc.Height,
                    desc.MipLevels,
                    desc.ArraySize,
                    static_cast<unsigned>(desc.Format),
                    desc.SampleDesc.Count,
                    desc.BindFlags,
                    desc.Usage,
                    desc.MiscFlags);
    }
    if (std::strcmp(stage, "enumerate") == 0)
    {
        std::printf("PASS: create and enumerate only\n");
        return 0;
    }

    std::vector<ID3D11DepthStencilView *> dsv(imageCount);
    std::vector<ID3D11ShaderResourceView *> srv(imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        HR_CHECK(device->CreateDepthStencilView(images[i].texture, &dsvDesc, &dsv[i]));

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        HR_CHECK(device->CreateShaderResourceView(images[i].texture, &srvDesc, &srv[i]));
    }
    std::printf("created typed DSV and depth-plane SRV for all images\n");
    if (std::strcmp(stage, "views") == 0)
    {
        std::printf("PASS: create, enumerate, and OVR-style views\n");
        return 0;
    }

    ID3D11ComputeShader * sampleShader = nullptr;
    ID3D11Buffer * sampleResult = nullptr;
    ID3D11Buffer * sampleReadback = nullptr;
    ID3D11UnorderedAccessView * sampleResultView = nullptr;
    if (std::strcmp(stage, "sample") == 0)
    {
        static const char shaderSource[] =
            "Texture2D<float> depthImage : register(t0);"
            "RWStructuredBuffer<float> result : register(u0);"
            "[numthreads(1, 1, 1)] void main() { result[0] = depthImage.Load(int3(0, 0, 0)); }";
        ID3DBlob * shaderBlob = nullptr;
        ID3DBlob * shaderErrors = nullptr;
        const HRESULT compileResult = D3DCompile(shaderSource,
                                                 sizeof(shaderSource) - 1,
                                                 "depth-sample",
                                                 nullptr,
                                                 nullptr,
                                                 "main",
                                                 "cs_5_0",
                                                 0,
                                                 0,
                                                 &shaderBlob,
                                                 &shaderErrors);
        if (shaderErrors)
        {
            std::fwrite(shaderErrors->GetBufferPointer(), 1, shaderErrors->GetBufferSize(), stderr);
            shaderErrors->Release();
        }
        HR_CHECK(compileResult);
        HR_CHECK(device->CreateComputeShader(shaderBlob->GetBufferPointer(),
                                             shaderBlob->GetBufferSize(),
                                             nullptr,
                                             &sampleShader));
        shaderBlob->Release();

        D3D11_BUFFER_DESC resultDesc{};
        resultDesc.ByteWidth = sizeof(float);
        resultDesc.Usage = D3D11_USAGE_DEFAULT;
        resultDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        resultDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        resultDesc.StructureByteStride = sizeof(float);
        HR_CHECK(device->CreateBuffer(&resultDesc, nullptr, &sampleResult));

        D3D11_UNORDERED_ACCESS_VIEW_DESC resultViewDesc{};
        resultViewDesc.Format = DXGI_FORMAT_UNKNOWN;
        resultViewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        resultViewDesc.Buffer.NumElements = 1;
        HR_CHECK(device->CreateUnorderedAccessView(sampleResult, &resultViewDesc, &sampleResultView));

        D3D11_BUFFER_DESC readbackDesc = resultDesc;
        readbackDesc.Usage = D3D11_USAGE_STAGING;
        readbackDesc.BindFlags = 0;
        readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        readbackDesc.MiscFlags = 0;
        readbackDesc.StructureByteStride = 0;
        HR_CHECK(device->CreateBuffer(&readbackDesc, nullptr, &sampleReadback));
    }

    for (uint32_t cycle = 0; cycle < 2; cycle++)
    {
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XR_CHECK(xr.acquireImage(swapchain, &acquireInfo, &imageIndex));
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xr.waitImage(swapchain, &waitInfo));

        const float expectedDepth = 0.25f * static_cast<float>(cycle + 1);
        context->ClearDepthStencilView(dsv[imageIndex],
                                       D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                       expectedDepth,
                                       static_cast<UINT8>(0x5a + cycle));

        if (sampleShader)
        {
            context->CSSetShader(sampleShader, nullptr, 0);
            context->CSSetShaderResources(0, 1, &srv[imageIndex]);
            context->CSSetUnorderedAccessViews(0, 1, &sampleResultView, nullptr);
            context->Dispatch(1, 1, 1);

            ID3D11ShaderResourceView * nullSrv = nullptr;
            ID3D11UnorderedAccessView * nullUav = nullptr;
            context->CSSetShaderResources(0, 1, &nullSrv);
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            context->CopyResource(sampleReadback, sampleResult);
        }

        if (!wait_for_gpu(device, context))
            return 1;

        if (sampleShader)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HR_CHECK(context->Map(sampleReadback, 0, D3D11_MAP_READ, 0, &mapped));
            const float actualDepth = *static_cast<const float *>(mapped.pData);
            context->Unmap(sampleReadback, 0);
            if (actualDepth != expectedDepth)
            {
                std::fprintf(stderr,
                             "FAIL: cycle %u sampled depth %f, expected %f\n",
                             cycle,
                             actualDepth,
                             expectedDepth);
                return 1;
            }
        }

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xr.releaseImage(swapchain, &releaseInfo));
        std::printf("cycle %u completed on image %u\n", cycle, imageIndex);
    }
    std::printf("PASS: two clear%s cycles and GPU event fences completed\n",
                sampleShader ? "/sample" : "");

    release(sampleResultView);
    release(sampleReadback);
    release(sampleResult);
    release(sampleShader);

    for (auto * view: srv)
        release(view);
    for (auto * view: dsv)
        release(view);
    xr.destroySwapchain(swapchain);
    xr.destroySession(session);
    release(context);
    release(device);
    xr.destroyInstance(instance);
    FreeLibrary(loader);
    return 0;
}
