/* D3D11 smoke test (DXVK): a 1920x1080 window whose DXGI swapchain is 2x as
 * wide (side-by-side). Left half is red with a yellow bar, right half blue with
 * a cyan bar, so the halves differ. On WM_SIZE the buffers follow
 * 2*client width x client height through ResizeBuffers.
 * Usage: sbstest11.exe [seconds] [factor]   (factor 2 = 2x wide, 1 = window size)
 * Build: x86_64-w64-mingw32-gcc -O2 -o sbstest11.exe sbstest11.c -ld3d11 -ldxgi */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
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

static void clear_rect(ID3D11DeviceContext1 *ctx, ID3D11RenderTargetView *rtv, const float *c,
        LONG l, LONG t, LONG r, LONG b)
{
    D3D11_RECT rc = {l, t, r, b};
    ID3D11DeviceContext1_ClearView(ctx, (ID3D11View *)rtv, c, &rc, 1);
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 8;
    WNDCLASSW wc = {0};
    RECT r = {0, 0, 1920, 1080};
    HWND hwnd;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx0;
    ID3D11DeviceContext1 *ctx;
    IDXGIDevice *dxgi_dev;
    IDXGIAdapter *adapter;
    IDXGIFactory2 *factory;
    IDXGISwapChain1 *sc;
    ID3D11Texture2D *buf = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    UINT frame = 0, resizes = 0;
    DWORD start, last_log = 0;

    if (argc > 2) g_factor = atoi(argv[2]);
    freopen("sbstest11.log", "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"sbstest11";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(L"sbstest11", L"sbstest11", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
            NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    GetClientRect(hwnd, &r);
    g_w = r.right; g_h = r.bottom;
    printf("client %ux%u factor %u\n", g_w, g_h, g_factor);

    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx0));
    CHECK(ID3D11DeviceContext_QueryInterface(ctx0, &IID_ID3D11DeviceContext1, (void **)&ctx));
    CHECK(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev));
    CHECK(IDXGIDevice_GetAdapter(dxgi_dev, &adapter));
    CHECK(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory));

    sd.Width = g_w * g_factor; sd.Height = g_h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = NBUF;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    CHECK(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)dev, hwnd, &sd, NULL, NULL, &sc));
    printf("swapchain buffers %ux%u\n", sd.Width, sd.Height);

#define GET_BUFFER() do { CHECK(IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&buf)); \
    CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)buf, NULL, &rtv)); } while (0)
    GET_BUFFER();

    start = GetTickCount();
    while (GetTickCount() - start < (DWORD)seconds * 1000)
    {
        MSG msg;
        UINT bw = g_w * g_factor, half;
        float red[4] = {0.8f, 0.1f, 0.1f, 1}, yellow[4] = {0.9f, 0.9f, 0.1f, 1};
        float blue[4] = {0.1f, 0.2f, 0.9f, 1}, cyan[4] = {0.1f, 0.9f, 0.9f, 1};
        HRESULT hr;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_resize_pending)
        {
            g_resize_pending = FALSE;
            ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
            ID3D11RenderTargetView_Release(rtv);
            ID3D11Texture2D_Release(buf);
            CHECK(IDXGISwapChain1_ResizeBuffers(sc, NBUF, g_w * g_factor, g_h, DXGI_FORMAT_UNKNOWN, 0));
            GET_BUFFER();
            resizes++;
            printf("resized: client %ux%u -> buffers %ux%u\n", g_w, g_h, g_w * g_factor, g_h);
            bw = g_w * g_factor;
        }
        half = g_factor == 2 ? bw / 2 : bw;

        /* left half: red with a yellow bar at 1/4; right half: blue with a cyan bar at 3/4 */
        clear_rect(ctx, rtv, red, 0, 0, half, g_h);
        clear_rect(ctx, rtv, yellow, half / 4 - 40, g_h / 4, half / 4 + 40, g_h * 3 / 4);
        if (g_factor == 2)
        {
            clear_rect(ctx, rtv, blue, half, 0, bw, g_h);
            clear_rect(ctx, rtv, cyan, half + half * 3 / 4 - 40, g_h / 4, half + half * 3 / 4 + 40, g_h * 3 / 4);
        }
        hr = IDXGISwapChain1_Present(sc, 1, 0);
        if (FAILED(hr)) { printf("FAIL Present: %#lx\n", hr); ExitProcess(1); }
        if (hr != S_OK && frame < 50) printf("Present returned %#lx at frame %u\n", hr, frame);
        frame++;
        if (GetTickCount() - last_log > 2000)
        {
            DXGI_SWAP_CHAIN_DESC1 cur;
            last_log = GetTickCount();
            GetClientRect(hwnd, &r);
            IDXGISwapChain1_GetDesc1(sc, &cur);
            printf("frame %u client %ldx%ld dxgi buffers %ux%u\n", frame, r.right, r.bottom, cur.Width, cur.Height);
        }
    }
    printf("done, %u frames, %u resizes\n", frame, resizes);
    return 0;
}
