/* D3D12 smoke test: a 1920x1080 window whose DXGI swapchain is 2x as wide
 * (side-by-side). Left half is a red/yellow pattern, right half blue/cyan, so
 * the halves differ. On WM_SIZE the buffers follow 2*client width x client height.
 * Usage: sbstest.exe [seconds] [factor]   (factor 2 = 2x wide, 1 = window size) */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>

#define NBUF 3
static UINT g_w = 1920, g_h = 1080, g_factor = 2;
static BOOL g_resize_pending;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        if (LOWORD(lp) && HIWORD(lp) && (LOWORD(lp) != g_w || HIWORD(lp) != g_h))
        {
            g_w = LOWORD(lp); g_h = HIWORD(lp); g_resize_pending = TRUE;
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s: %#lx\n", #x, hr_); fflush(stdout); ExitProcess(1); } } while (0)

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 8;
    WNDCLASSW wc = {0};
    RECT r = {0, 0, 1920, 1080};
    HWND hwnd;
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12DescriptorHeap *heap;
    ID3D12Fence *fence;
    IDXGIFactory4 *factory;
    IDXGISwapChain1 *sc1;
    IDXGISwapChain3 *sc;
    ID3D12Resource *bufs[NBUF] = {0};
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    UINT64 fval = 0;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    UINT rtv_size, i, frame = 0;
    DWORD start, last_log = 0;

    if (argc > 2) g_factor = atoi(argv[2]);
    freopen("sbstest.log", "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"sbstest";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(L"sbstest", L"sbstest", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
            NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    GetClientRect(hwnd, &r);
    g_w = r.right; g_h = r.bottom;
    printf("client %ux%u factor %u\n", g_w, g_h, g_factor);

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_Close(list);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = NBUF;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
    sd.Width = g_w * g_factor; sd.Height = g_h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = NBUF;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1));
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
    printf("swapchain buffers %ux%u\n", sd.Width, sd.Height);

#define GET_BUFFERS() do { D3D12_CPU_DESCRIPTOR_HANDLE h_; h_ = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap); \
    for (i = 0; i < NBUF; i++) { CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i])); \
        ID3D12Device_CreateRenderTargetView(dev, bufs[i], NULL, h_); h_.ptr += rtv_size; } } while (0)
    GET_BUFFERS();

    start = GetTickCount();
    while (GetTickCount() - start < (DWORD)seconds * 1000)
    {
        MSG msg;
        UINT idx, bw = g_w * g_factor, half;
        D3D12_RESOURCE_BARRIER b = {0};
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        float red[4] = {0.8f, 0.1f, 0.1f, 1}, yellow[4] = {0.9f, 0.9f, 0.1f, 1};
        float blue[4] = {0.1f, 0.2f, 0.9f, 1}, cyan[4] = {0.1f, 0.9f, 0.9f, 1};
        D3D12_RECT rc;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_resize_pending)
        {
            g_resize_pending = FALSE;
            for (i = 0; i < NBUF; i++) ID3D12Resource_Release(bufs[i]);
            CHECK(IDXGISwapChain3_ResizeBuffers(sc, NBUF, g_w * g_factor, g_h, DXGI_FORMAT_UNKNOWN, 0));
            GET_BUFFERS();
            printf("resized: client %ux%u -> buffers %ux%u\n", g_w, g_h, g_w * g_factor, g_h);
            bw = g_w * g_factor;
        }
        half = g_factor == 2 ? bw / 2 : bw;

        idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[idx];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
        h.ptr += idx * rtv_size;
        /* left half: red with a yellow bar at 1/4; right half: blue with a cyan bar at 3/4 */
        rc.left = 0; rc.top = 0; rc.right = half; rc.bottom = g_h;
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, red, 1, &rc);
        rc.left = half / 4 - 40; rc.right = half / 4 + 40; rc.top = g_h / 4; rc.bottom = g_h * 3 / 4;
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, yellow, 1, &rc);
        if (g_factor == 2)
        {
            rc.left = half; rc.top = 0; rc.right = bw; rc.bottom = g_h;
            ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, blue, 1, &rc);
            rc.left = half + half * 3 / 4 - 40; rc.right = half + half * 3 / 4 + 40; rc.top = g_h / 4; rc.bottom = g_h * 3 / 4;
            ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, cyan, 1, &rc);
        }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        CHECK(IDXGISwapChain3_Present(sc, 1, 0));
        ID3D12CommandQueue_Signal(queue, fence, ++fval);
        ID3D12Fence_SetEventOnCompletion(fence, fval, ev);
        WaitForSingleObject(ev, INFINITE);
        frame++;
        if (GetTickCount() - last_log > 2000)
        {
            DXGI_SWAP_CHAIN_DESC1 cur;
            last_log = GetTickCount();
            GetClientRect(hwnd, &r);
            IDXGISwapChain3_GetDesc1(sc, &cur);
            printf("frame %u client %ldx%ld dxgi buffers %ux%u\n", frame, r.right, r.bottom, cur.Width, cur.Height);
        }
    }
    printf("done, %u frames\n", frame);
    return 0;
}
