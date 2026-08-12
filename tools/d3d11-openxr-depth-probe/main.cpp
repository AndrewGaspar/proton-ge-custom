#define WIN32_LEAN_AND_MEAN
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include "wineopenxr.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
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

static bool
wait_for_vr_registry()
{
    constexpr ULONGLONG timeoutMs = 15000;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;

    do
    {
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            DWORD type = 0;
            DWORD state = 0;
            DWORD size = sizeof(state);
            const LSTATUS status = RegQueryValueExA(key, "state", nullptr, &type,
                                                    reinterpret_cast<BYTE *>(&state), &size);
            RegCloseKey(key);
            if (status == ERROR_SUCCESS && type == REG_DWORD && state == 1)
            {
                std::printf("Wine VR registry state: ready\n");
                return true;
            }
            if (status == ERROR_SUCCESS && type == REG_DWORD && state == static_cast<DWORD>(-1))
            {
                std::fprintf(stderr, "FAIL: Wine VR registry initialization reported state -1\n");
                return false;
            }
        }
        Sleep(50);
    } while (GetTickCount64() < deadline);

    std::fprintf(stderr, "FAIL: Wine VR registry did not reach state 1 within 15 seconds\n");
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
    PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
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

struct SDepthSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11DepthStencilView *> dsv;
    std::vector<ID3D11ShaderResourceView *> srv;
};

struct SColorSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView *> rtv;
};

struct SDrawPipeline
{
    ID3D11VertexShader * vertexShader = nullptr;
    ID3D11PixelShader * pixelShader = nullptr;
    ID3D11DepthStencilState * depthState = nullptr;
};

static bool
create_depth_swapchain(const SXrFunctions & xr,
                       XrSession session,
                       ID3D11Device * device,
                       const XrSwapchainCreateInfo & createInfo,
                       bool createViews,
                       SDepthSwapchain & depth)
{
    if (!xr_ok(xr.createSwapchain(session, &createInfo, &depth.handle), "xrCreateSwapchain"))
        return false;

    uint32_t imageCount = 0;
    if (!xr_ok(xr.enumerateImages(depth.handle, 0, &imageCount, nullptr),
               "xrEnumerateSwapchainImages(count)"))
        return false;
    depth.images.assign(imageCount,
                        XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!xr_ok(xr.enumerateImages(depth.handle,
                                  imageCount,
                                  &imageCount,
                                  reinterpret_cast<XrSwapchainImageBaseHeader *>(depth.images.data())),
               "xrEnumerateSwapchainImages(images)"))
        return false;

    if (!createViews)
        return true;

    depth.dsv.resize(imageCount);
    depth.srv.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (!hr_ok(device->CreateDepthStencilView(depth.images[i].texture, &dsvDesc, &depth.dsv[i]),
                   "ID3D11Device::CreateDepthStencilView"))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        if (!hr_ok(device->CreateShaderResourceView(depth.images[i].texture, &srvDesc, &depth.srv[i]),
                   "ID3D11Device::CreateShaderResourceView"))
            return false;
    }
    return true;
}

static bool
destroy_depth_swapchain(const SXrFunctions & xr, SDepthSwapchain & depth)
{
    for (auto * view: depth.srv)
        release(view);
    for (auto * view: depth.dsv)
        release(view);
    depth.srv.clear();
    depth.dsv.clear();
    depth.images.clear();

    if (depth.handle == XR_NULL_HANDLE)
        return true;
    const XrResult result = xr.destroySwapchain(depth.handle);
    depth.handle = XR_NULL_HANDLE;
    return xr_ok(result, "xrDestroySwapchain");
}
static bool create_color_swapchain(const SXrFunctions &xr, XrSession session,
                                   ID3D11Device *device,
                                   const XrSwapchainCreateInfo &createInfo,
                                   SColorSwapchain &color) {
  color.format = static_cast<DXGI_FORMAT>(createInfo.format);
  if (!xr_ok(xr.createSwapchain(session, &createInfo, &color.handle),
             "xrCreateSwapchain(color)"))
    return false;

  uint32_t imageCount = 0;
  if (!xr_ok(xr.enumerateImages(color.handle, 0, &imageCount, nullptr),
             "xrEnumerateSwapchainImages(color count)"))
    return false;
  color.images.assign(
      imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  if (!xr_ok(xr.enumerateImages(color.handle, imageCount, &imageCount,
                                reinterpret_cast<XrSwapchainImageBaseHeader *>(
                                    color.images.data())),
             "xrEnumerateSwapchainImages(color images)"))
    return false;

  color.rtv.resize(imageCount);
  for (uint32_t i = 0; i < imageCount; i++) {
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = color.format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (!hr_ok(device->CreateRenderTargetView(color.images[i].texture, &rtvDesc,
                                              &color.rtv[i]),
               "ID3D11Device::CreateRenderTargetView"))
      return false;

    D3D11_TEXTURE2D_DESC desc{};
    color.images[i].texture->GetDesc(&desc);
    std::printf("color image[%u]: %ux%u mips=%u layers=%u format=%u samples=%u "
                "bind=0x%x usage=%u misc=0x%x\n",
                i, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize,
                static_cast<unsigned>(desc.Format), desc.SampleDesc.Count,
                desc.BindFlags, desc.Usage, desc.MiscFlags);
    if (desc.Width != createInfo.width || desc.Height != createInfo.height ||
        desc.MipLevels != createInfo.mipCount ||
        desc.ArraySize != createInfo.arraySize ||
        desc.SampleDesc.Count != createInfo.sampleCount ||
        desc.Format != color.format ||
        !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
      std::fprintf(stderr,
                   "FAIL: imported color image %u does not match its swapchain "
                   "contract\n",
                   i);
      return false;
    }
  }
  return true;
}

static bool destroy_color_swapchain(const SXrFunctions &xr,
                                    SColorSwapchain &color) {
  for (auto *view : color.rtv)
    release(view);
  color.rtv.clear();
  color.images.clear();

  if (color.handle == XR_NULL_HANDLE)
    return true;
  const XrResult result = xr.destroySwapchain(color.handle);
  color.handle = XR_NULL_HANDLE;
  return xr_ok(result, "xrDestroySwapchain(color)");
}

static bool compile_shader(const char *source, const char *entry,
                           const char *target, ID3DBlob **blob) {
  ID3DBlob *errors = nullptr;
  const HRESULT result = D3DCompile(
      source, std::strlen(source), "mixed-color-depth-draw", nullptr, nullptr,
      entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, &errors);
  if (errors) {
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    errors->Release();
  }
  return hr_ok(result, "D3DCompile");
}

static bool create_draw_pipeline(ID3D11Device *device,
                                 SDrawPipeline &pipeline) {
  static const char vertexSource[] =
      "float4 main(uint vertex : SV_VertexID) : SV_Position {"
      "  float2 p[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, "
      "-1.0) };"
      "  return float4(p[vertex], 0.5, 1.0);"
      "}";
  static const char pixelSource[] =
      "float4 main() : SV_Target { return float4(0.125, 0.5, 0.875, 1.0); }";

  ID3DBlob *vertexBlob = nullptr;
  ID3DBlob *pixelBlob = nullptr;
  if (!compile_shader(vertexSource, "main", "vs_5_0", &vertexBlob) ||
      !compile_shader(pixelSource, "main", "ps_5_0", &pixelBlob)) {
    release(vertexBlob);
    release(pixelBlob);
    return false;
  }
  const HRESULT vertexResult = device->CreateVertexShader(
      vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr,
      &pipeline.vertexShader);
  const HRESULT pixelResult = device->CreatePixelShader(
      pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr,
      &pipeline.pixelShader);
  release(vertexBlob);
  release(pixelBlob);
  if (!hr_ok(vertexResult, "ID3D11Device::CreateVertexShader") ||
      !hr_ok(pixelResult, "ID3D11Device::CreatePixelShader"))
    return false;

  D3D11_DEPTH_STENCIL_DESC depthDesc{};
  depthDesc.DepthEnable = TRUE;
  depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  return hr_ok(
      device->CreateDepthStencilState(&depthDesc, &pipeline.depthState),
      "ID3D11Device::CreateDepthStencilState");
}

static void destroy_draw_pipeline(SDrawPipeline &pipeline) {
  release(pipeline.depthState);
  release(pipeline.pixelShader);
  release(pipeline.vertexShader);
}

static bool acquire_swapchain_image(const SXrFunctions &xr,
                                    XrSwapchain swapchain,
                                    uint32_t &imageIndex) {
  XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (!xr_ok(xr.acquireImage(swapchain, &acquireInfo, &imageIndex),
             "xrAcquireSwapchainImage"))
    return false;
  XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  waitInfo.timeout = XR_INFINITE_DURATION;
  return xr_ok(xr.waitImage(swapchain, &waitInfo), "xrWaitSwapchainImage");
}

static bool release_swapchain_image(const SXrFunctions &xr,
                                    XrSwapchain swapchain) {
  XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  return xr_ok(xr.releaseImage(swapchain, &releaseInfo),
               "xrReleaseSwapchainImage");
}

static bool draw_color_depth_pair(
    const SXrFunctions &xr, ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11DeviceContext1 *context1, const SDrawPipeline &pipeline,
    SColorSwapchain &color, SDepthSwapchain &depth, uint32_t width,
    uint32_t height, bool discard, bool fenceBeforeRelease) {
  uint32_t colorIndex = 0;
  uint32_t depthIndex = 0;
  if (!acquire_swapchain_image(xr, color.handle, colorIndex) ||
      !acquire_swapchain_image(xr, depth.handle, depthIndex))
    return false;

  if (discard) {
    context1->DiscardView(color.rtv[colorIndex]);
    context1->DiscardView(depth.dsv[depthIndex]);
  }

  const float clearColor[]{0.0f, 0.0f, 0.0f, 1.0f};
  context->ClearRenderTargetView(color.rtv[colorIndex], clearColor);
  context->ClearDepthStencilView(
      depth.dsv[depthIndex], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(width);
  viewport.Height = static_cast<float>(height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, &color.rtv[colorIndex], depth.dsv[depthIndex]);
  context->OMSetDepthStencilState(pipeline.depthState, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(pipeline.vertexShader, nullptr, 0);
  context->PSSetShader(pipeline.pixelShader, nullptr, 0);
  context->Draw(3, 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);

  if (fenceBeforeRelease && !wait_for_gpu(device, context))
    return false;
  if (!release_swapchain_image(xr, depth.handle) ||
      !release_swapchain_image(xr, color.handle))
    return false;

  std::printf("drew color image %u with depth image %u%s\n", colorIndex,
              depthIndex,
              fenceBeforeRelease ? " and completed an event fence" : "");
  return true;
}

static bool begin_stereo_session(const SXrFunctions &xr, XrInstance instance,
                                 XrSession session) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult result = xr.pollEvent(instance, &event);
    if (result == XR_EVENT_UNAVAILABLE) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (!xr_ok(result, "xrPollEvent"))
      return false;
    if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
      continue;

    const auto &changed =
        *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
    if (changed.session != session)
      continue;
    std::printf("session state changed to %d\n",
                static_cast<int>(changed.state));
    if (changed.state == XR_SESSION_STATE_READY) {
      XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
      beginInfo.primaryViewConfigurationType =
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      return xr_ok(xr.beginSession(session, &beginInfo), "xrBeginSession");
    }
    if (changed.state == XR_SESSION_STATE_EXITING ||
        changed.state == XR_SESSION_STATE_LOSS_PENDING) {
      std::fprintf(stderr,
                   "FAIL: session entered terminal state %d before READY\n",
                   static_cast<int>(changed.state));
      return false;
    }
  }

  std::fprintf(stderr, "FAIL: session did not reach READY within 20 seconds\n");
  return false;
}

static bool run_projection_frame(const SXrFunctions &xr, XrInstance instance,
                                 XrSession session, ID3D11Device *device,
                                 ID3D11DeviceContext *context,
                                 const SDrawPipeline &pipeline,
                                 std::array<SColorSwapchain, 2> &colors,
                                 std::array<SDepthSwapchain, 2> &depths,
                                 uint32_t width, uint32_t height) {
  if (!begin_stereo_session(xr, instance, session))
    return false;

  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
  XrSpace space = XR_NULL_HANDLE;
  if (!xr_ok(xr.createReferenceSpace(session, &spaceInfo, &space),
             "xrCreateReferenceSpace"))
    return false;

  ID3D11DeviceContext1 *context1 = nullptr;
  if (!hr_ok(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                     reinterpret_cast<void **>(&context1)),
             "ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext1)"))
    return false;

  bool submitted = false;
  for (uint32_t attempt = 0; attempt < 120 && !submitted; attempt++) {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!xr_ok(xr.waitFrame(session, &waitInfo, &frameState), "xrWaitFrame"))
      return false;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!xr_ok(xr.beginFrame(session, &beginInfo), "xrBeginFrame"))
      return false;

    if (!frameState.shouldRender) {
      XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
      endInfo.displayTime = frameState.predictedDisplayTime;
      endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
      if (!xr_ok(xr.endFrame(session, &endInfo), "xrEndFrame(no layers)"))
        return false;
      std::printf("frame attempt %u was not renderable\n", attempt);
      continue;
    }

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{
        XrView{XR_TYPE_VIEW},
        XrView{XR_TYPE_VIEW},
    };
    uint32_t viewCount = 0;
    if (!xr_ok(xr.locateViews(session, &locateInfo, &viewState,
                              static_cast<uint32_t>(views.size()), &viewCount,
                              views.data()),
               "xrLocateViews"))
      return false;
    constexpr XrViewStateFlags requiredViewFlags =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (viewCount != views.size() ||
        (viewState.viewStateFlags & requiredViewFlags) != requiredViewFlags) {
      std::fprintf(
          stderr, "FAIL: xrLocateViews returned %u views with flags 0x%llx\n",
          viewCount, static_cast<unsigned long long>(viewState.viewStateFlags));
      return false;
    }

    for (uint32_t eye = 0; eye < 2; eye++) {
      if (!draw_color_depth_pair(xr, device, context, context1, pipeline,
                                 colors[eye], depths[eye], width, height, true,
                                 false))
        return false;
    }

    std::array<XrCompositionLayerDepthInfoKHR, 2> depthInfo{
        XrCompositionLayerDepthInfoKHR{
            XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
        XrCompositionLayerDepthInfoKHR{
            XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
    };
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{
        XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };
    for (uint32_t eye = 0; eye < 2; eye++) {
      depthInfo[eye].subImage.swapchain = depths[eye].handle;
      depthInfo[eye].subImage.imageRect.extent = {
          static_cast<int32_t>(width),
          static_cast<int32_t>(height),
      };
      depthInfo[eye].minDepth = 0.0f;
      depthInfo[eye].maxDepth = 1.0f;
      depthInfo[eye].nearZ = 0.05f;
      depthInfo[eye].farZ = 1000.0f;

      projectionViews[eye].next = &depthInfo[eye];
      projectionViews[eye].pose = views[eye].pose;
      projectionViews[eye].fov = views[eye].fov;
      projectionViews[eye].subImage.swapchain = colors[eye].handle;
      projectionViews[eye].subImage.imageRect.extent = {
          static_cast<int32_t>(width),
          static_cast<int32_t>(height),
      };
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space;
    layer.viewCount = static_cast<uint32_t>(projectionViews.size());
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader *layers[]{
        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer),
    };
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = ARRAYSIZE(layers);
    endInfo.layers = layers;
    if (!xr_ok(xr.endFrame(session, &endInfo), "xrEndFrame(projection+depth)"))
      return false;
    submitted = true;
    std::printf("submitted two separate-eye color/depth projection views\n");
  }

  release(context1);
  xr.destroySpace(space);
  if (!submitted) {
    std::fprintf(stderr,
                 "FAIL: no renderable frame was available in 120 attempts\n");
    return false;
  }
  return wait_for_gpu(device, context);
}

static bool
clear_depth_swapchain(const SXrFunctions & xr,
                      ID3D11DeviceContext * context,
                      ID3D11DeviceContext1 * context1,
                      SDepthSwapchain & depth,
                      bool discard)
{
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xr_ok(xr.acquireImage(depth.handle, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage"))
        return false;
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (!xr_ok(xr.waitImage(depth.handle, &waitInfo), "xrWaitSwapchainImage"))
        return false;

    if (discard)
        context1->DiscardView(depth.dsv[imageIndex]);
    context->ClearDepthStencilView(depth.dsv[imageIndex],
                                   D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                   0.5f,
                                   0x5a);

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return xr_ok(xr.releaseImage(depth.handle, &releaseInfo), "xrReleaseSwapchainImage");
}

int main(int argc, char ** argv)
{
    if (!std::freopen("d3d11-openxr-depth-probe.stdout.log", "w", stdout) ||
        !std::freopen("d3d11-openxr-depth-probe.stderr.log", "w", stderr))
        return 3;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const char * stage = argc > 1 ? argv[1] : "enumerate";
    std::printf("probe stage: %s\n", stage);
  if (std::strcmp(stage, "enumerate") != 0 &&
      std::strcmp(stage, "views") != 0 && std::strcmp(stage, "clear") != 0 &&
      std::strcmp(stage, "sample") != 0 &&
      std::strcmp(stage, "teardown") != 0 &&
      std::strcmp(stage, "recreate") != 0 &&
      std::strcmp(stage, "discard") != 0 && std::strcmp(stage, "draw") != 0 &&
      std::strcmp(stage, "frame") != 0) {
    std::fprintf(
        stderr,
        "usage: %s "
        "[enumerate|views|clear|sample|teardown|recreate|discard|draw|frame]\n",
                     argv[0]);
        return 2;
    }

    if (!wait_for_vr_registry())
        return 1;

    HMODULE loader = LoadLibraryA("openxr_loader.dll");
  if (!loader) {
    std::fprintf(stderr, "FAIL: LoadLibrary(openxr_loader.dll) returned %lu\n",
                 GetLastError());
        return 1;
    }

    SXrFunctions xr{};
  xr.getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
      GetProcAddress(loader, "xrGetInstanceProcAddr"));
  if (!xr.getInstanceProcAddr ||
      !load_xr(xr, XR_NULL_HANDLE, "xrCreateInstance", xr.createInstance) ||
      !load_xr(xr, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
               xr.enumerateExtensions)) {
        std::fprintf(stderr, "FAIL: OpenXR loader has no global entry points\n");
        return 1;
    }

  const bool frameStage = std::strcmp(stage, "frame") == 0;
  uint32_t extensionCount = 0;
  XR_CHECK(xr.enumerateExtensions(nullptr, 0, &extensionCount, nullptr));
  std::vector<XrExtensionProperties> extensionProperties(
      extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
  XR_CHECK(xr.enumerateExtensions(nullptr, extensionCount, &extensionCount,
                                  extensionProperties.data()));
  bool hasDepthComposition = false;
  for (const auto &extension : extensionProperties)
    hasDepthComposition |=
        std::strcmp(extension.extensionName,
                    XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0;
  if (frameStage && !hasDepthComposition) {
    std::fprintf(stderr,
                 "FAIL: frame stage requires XR_KHR_composition_layer_depth\n");
    return 1;
  }

    std::vector<const char *> extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (frameStage)
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(instanceInfo.applicationInfo.applicationName,
               "d3d11-openxr-depth-probe", XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
  std::strncpy(instanceInfo.applicationInfo.engineName, "none",
               XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  instanceInfo.enabledExtensionNames = extensions.data();
    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xr.createInstance(&instanceInfo, &instance));

    if (!load_xr(xr, instance, "xrDestroyInstance", xr.destroyInstance) ||
        !load_xr(xr, instance, "xrGetSystem", xr.getSystem) ||
      !load_xr(xr, instance, "xrGetD3D11GraphicsRequirementsKHR",
               xr.getD3D11Requirements) ||
        !load_xr(xr, instance, "xrCreateSession", xr.createSession) ||
        !load_xr(xr, instance, "xrDestroySession", xr.destroySession) ||
      !load_xr(xr, instance, "xrPollEvent", xr.pollEvent) ||
      !load_xr(xr, instance, "xrBeginSession", xr.beginSession) ||
      !load_xr(xr, instance, "xrWaitFrame", xr.waitFrame) ||
      !load_xr(xr, instance, "xrBeginFrame", xr.beginFrame) ||
      !load_xr(xr, instance, "xrEndFrame", xr.endFrame) ||
      !load_xr(xr, instance, "xrCreateReferenceSpace",
               xr.createReferenceSpace) ||
      !load_xr(xr, instance, "xrDestroySpace", xr.destroySpace) ||
      !load_xr(xr, instance, "xrLocateViews", xr.locateViews) ||
      !load_xr(xr, instance, "xrEnumerateSwapchainFormats",
               xr.enumerateFormats) ||
        !load_xr(xr, instance, "xrCreateSwapchain", xr.createSwapchain) ||
        !load_xr(xr, instance, "xrDestroySwapchain", xr.destroySwapchain) ||
      !load_xr(xr, instance, "xrEnumerateSwapchainImages",
               xr.enumerateImages) ||
        !load_xr(xr, instance, "xrAcquireSwapchainImage", xr.acquireImage) ||
        !load_xr(xr, instance, "xrWaitSwapchainImage", xr.waitImage) ||
        !load_xr(xr, instance, "xrReleaseSwapchainImage", xr.releaseImage))
        return 1;

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XR_CHECK(xr.getSystem(instance, &systemInfo, &system));

  XrGraphicsRequirementsD3D11KHR requirements{
      XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
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
  XR_CHECK(
      xr.enumerateFormats(session, formatCount, &formatCount, formats.data()));
    bool hasD32S8 = false;
  DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
  for (const int64_t format : formats) {
        hasD32S8 |= format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if (colorFormat == DXGI_FORMAT_UNKNOWN &&
        format == DXGI_FORMAT_R8G8B8A8_UNORM)
      colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
      colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  }
  if (!hasD32S8) {
    std::fprintf(stderr,
                 "FAIL: DXGI_FORMAT_D32_FLOAT_S8X24_UINT is not advertised\n");
    return 1;
  }
  if ((std::strcmp(stage, "draw") == 0 || frameStage) &&
      colorFormat == DXGI_FORMAT_UNKNOWN) {
    std::fprintf(
        stderr,
        "FAIL: neither RGBA8 UNORM nor RGBA8 SRGB is advertised for color\n");
        return 1;
    }

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = 2064;
    swapchainInfo.height = 2208;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    const bool createViews = std::strcmp(stage, "enumerate") != 0;
    SDepthSwapchain depth;
  if (!create_depth_swapchain(xr, session, device, swapchainInfo, createViews,
                              depth))
        return 1;

    const uint32_t imageCount = static_cast<uint32_t>(depth.images.size());
    std::printf("enumerated %u D3D11 images\n", imageCount);
  for (uint32_t i = 0; i < imageCount; i++) {
        D3D11_TEXTURE2D_DESC desc{};
        depth.images[i].texture->GetDesc(&desc);
    std::printf("image[%u]: %ux%u mips=%u layers=%u format=%u samples=%u "
                "bind=0x%x usage=%u misc=0x%x\n",
                i, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize,
                static_cast<unsigned>(desc.Format), desc.SampleDesc.Count,
                desc.BindFlags, desc.Usage, desc.MiscFlags);
    if (desc.Width != swapchainInfo.width ||
        desc.Height != swapchainInfo.height ||
        desc.MipLevels != swapchainInfo.mipCount ||
        desc.ArraySize != swapchainInfo.arraySize ||
        desc.SampleDesc.Count != swapchainInfo.sampleCount ||
        desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS ||
        (desc.BindFlags &
         (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) !=
            (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) {
      std::fprintf(stderr,
                   "FAIL: imported depth image %u does not match its swapchain "
                   "contract\n",
                   i);
      return 1;
    }
  }
  if (std::strcmp(stage, "enumerate") == 0) {
        if (!destroy_depth_swapchain(xr, depth))
            return 1;
        std::printf("PASS: create and enumerate only\n");
        return 0;
    }

    std::printf("created typed DSV and depth-plane SRV for all images\n");
  if (std::strcmp(stage, "views") == 0) {
        if (!destroy_depth_swapchain(xr, depth))
            return 1;
        std::printf("PASS: create, enumerate, and OVR-style views\n");
        return 0;
    }

  if (std::strcmp(stage, "draw") == 0 || frameStage) {
    XrSwapchainCreateInfo colorInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    colorInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                           XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    colorInfo.format = colorFormat;
    colorInfo.sampleCount = swapchainInfo.sampleCount;
    colorInfo.width = swapchainInfo.width;
    colorInfo.height = swapchainInfo.height;
    colorInfo.faceCount = swapchainInfo.faceCount;
    colorInfo.arraySize = swapchainInfo.arraySize;
    colorInfo.mipCount = swapchainInfo.mipCount;

    std::array<SColorSwapchain, 2> colors;
    std::array<SDepthSwapchain, 2> depths;
    depths[0] = std::move(depth);
    const uint32_t pairCount = frameStage ? 2 : 1;
    for (uint32_t eye = 0; eye < pairCount; eye++) {
      if (!create_color_swapchain(xr, session, device, colorInfo, colors[eye]))
            return 1;
      if (eye > 0 && !create_depth_swapchain(xr, session, device, swapchainInfo,
                                             true, depths[eye]))
            return 1;
    }

    SDrawPipeline pipeline;
    if (!create_draw_pipeline(device, pipeline))
      return 1;

    bool succeeded = false;
    if (frameStage)
      succeeded = run_projection_frame(
          xr, instance, session, device, context, pipeline, colors, depths,
          swapchainInfo.width, swapchainInfo.height);
    else
      succeeded = draw_color_depth_pair(
          xr, device, context, nullptr, pipeline, colors[0], depths[0],
          swapchainInfo.width, swapchainInfo.height, false, true);

    destroy_draw_pipeline(pipeline);
    for (uint32_t eye = 0; eye < pairCount; eye++) {
      if (!destroy_depth_swapchain(xr, depths[eye]) ||
          !destroy_color_swapchain(xr, colors[eye]))
        return 1;
    }
    if (!succeeded)
      return 1;
    if (frameStage)
      std::printf("PASS: stereo color/depth draw and projection+depth "
                  "xrEndFrame completed\n");
    else
      std::printf("PASS: paired color RTV and D32S8 DSV draw completed with an "
                  "event fence\n");
        return 0;
    }

  if (std::strcmp(stage, "teardown") == 0) {
    if (!clear_depth_swapchain(xr, context, nullptr, depth, false))
      return 1;
    std::printf("released a cleared image; destroying immediately without a "
                "probe-side fence\n");
    if (!destroy_depth_swapchain(xr, depth) || !wait_for_gpu(device, context))
      return 1;
    std::printf("PASS: swapchain teardown drained outstanding D3D11 work; "
                "post-destroy fence completed\n");
    return 0;
  }

    if (std::strcmp(stage, "recreate") == 0 || std::strcmp(stage, "discard") == 0)
    {
        constexpr uint32_t swapchainCount = 6;
        std::vector<SDepthSwapchain> swapchains(swapchainCount);
        swapchains[0] = std::move(depth);
        for (uint32_t i = 1; i < swapchainCount; i++)
        {
            if (!create_depth_swapchain(xr, session, device, swapchainInfo, true, swapchains[i]))
                return 1;
        }

        ID3D11DeviceContext1 * context1 = nullptr;
        const bool discard = std::strcmp(stage, "discard") == 0;
        if (discard)
            HR_CHECK(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                             reinterpret_cast<void **>(&context1)));
        for (auto & item: swapchains)
        {
            if (!clear_depth_swapchain(xr, context, context1, item, discard))
                return 1;
        }
        std::printf("released work on six depth swapchains; destroying immediately without a probe-side fence\n");
        for (auto & item: swapchains)
        {
            if (!destroy_depth_swapchain(xr, item))
                return 1;
        }
        std::printf("destroyed the first generation of six OVR-style depth swapchains\n");

        for (auto & item: swapchains)
        {
            if (!create_depth_swapchain(xr, session, device, swapchainInfo, true, item))
                return 1;
        }
        for (auto & item: swapchains)
        {
            if (!clear_depth_swapchain(xr, context, context1, item, discard))
                return 1;
        }
        release(context1);
        if (!wait_for_gpu(device, context))
            return 1;

        for (auto & item: swapchains)
        {
            if (!destroy_depth_swapchain(xr, item))
                return 1;
        }
        if (!wait_for_gpu(device, context))
            return 1;
        std::printf("PASS: six depth swapchains destroyed, recreated, %s, fenced, and destroyed again\n",
                    discard ? "discarded/cleared" : "cleared");
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
        XR_CHECK(xr.acquireImage(depth.handle, &acquireInfo, &imageIndex));
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xr.waitImage(depth.handle, &waitInfo));

        const float expectedDepth = 0.25f * static_cast<float>(cycle + 1);
        context->ClearDepthStencilView(depth.dsv[imageIndex],
                                       D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                       expectedDepth,
                                       static_cast<UINT8>(0x5a + cycle));

        if (sampleShader)
        {
            context->CSSetShader(sampleShader, nullptr, 0);
            context->CSSetShaderResources(0, 1, &depth.srv[imageIndex]);
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
        XR_CHECK(xr.releaseImage(depth.handle, &releaseInfo));
        std::printf("cycle %u completed on image %u\n", cycle, imageIndex);
    }
    std::printf("PASS: two clear%s cycles and GPU event fences completed\n",
                sampleShader ? "/sample" : "");

    release(sampleResultView);
    release(sampleReadback);
    release(sampleResult);
    release(sampleShader);

    if (!destroy_depth_swapchain(xr, depth))
        return 1;
    xr.destroySession(session);
    release(context);
    release(device);
    xr.destroyInstance(instance);
    FreeLibrary(loader);
    return 0;
}
