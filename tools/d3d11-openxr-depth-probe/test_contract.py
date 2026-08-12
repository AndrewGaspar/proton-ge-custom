#!/usr/bin/env python3

import argparse
import os
import pathlib
import sys
import unittest


PROTON_ROOT = pathlib.Path(__file__).resolve().parents[2]

argument_parser = argparse.ArgumentParser(add_help=False)
argument_parser.add_argument("--monado-src")
arguments, unittest_arguments = argument_parser.parse_known_args()
sys.argv[1:] = unittest_arguments
monado_source = arguments.monado_src or os.environ.get("MONADO_SRC")
if not monado_source:
    raise SystemExit("set MONADO_SRC or pass --monado-src PATH")
MONADO_ROOT = pathlib.Path(monado_source).resolve()

ATTACHMENT = "DEPTH_STENCIL_ATTACHMENT_OPTIMAL"
READ_ONLY = "DEPTH_STENCIL_READ_ONLY_OPTIMAL"
UNDEFINED = "UNDEFINED"


class LayoutState:
    """Small model of the Monado acquire/release and DXVK layout contract."""

    def __init__(self):
        self.actual = UNDEFINED
        self.tracked = ATTACHMENT

    def monado_acquire(self):
        # UNDEFINED is a discard transition; it need not equal the old contents'
        # layout. Monado completes this queue submission before the app uses it.
        self.actual = ATTACHMENT

    def dxvk_transition(self, destination):
        self.assert_tracking_matches_actual()
        self.actual = destination
        self.tracked = destination

    def dxvk_restore(self):
        self.dxvk_transition(ATTACHMENT)

    def monado_release(self):
        # Under synchronization2 this equal-layout barrier is a memory dependency,
        # not a layout transition.
        self.assert_tracking_matches_actual()
        self.assertEqual(self.actual, ATTACHMENT)

    def assert_tracking_matches_actual(self):
        if self.actual != self.tracked:
            raise AssertionError(f"actual {self.actual} != tracked {self.tracked}")

    def assertEqual(self, left, right):
        if left != right:
            raise AssertionError(f"{left} != {right}")


class D32S8ImportContractTest(unittest.TestCase):
    def test_two_dsv_srv_cycles_keep_actual_and_tracked_layouts_equal(self):
        state = LayoutState()

        for _ in range(2):
            state.monado_acquire()
            state.assert_tracking_matches_actual()

            # The typed DSV write stays in the persistent attachment layout.
            state.assert_tracking_matches_actual()

            # A depth-plane SRV temporarily uses the read-only layout, then DXVK
            # restores borrowed/shared images before command submission ends.
            state.dxvk_transition(READ_ONLY)
            state.dxvk_restore()
            state.monado_release()

    def test_source_implements_contract(self):
        wineopenxr = (PROTON_ROOT / "wineopenxr/openxr_loader.c").read_text()
        dxvk = (PROTON_ROOT / "dxvk/src/d3d11/d3d11_texture.cpp").read_text()
        probe = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/main.cpp").read_text()
        monado = (MONADO_ROOT / "src/xrt/compositor/client/comp_vk_client.c").read_text()
        support = (MONADO_ROOT / "src/xrt/auxiliary/vk/vk_compositor_flags.c").read_text()
        allocator = (MONADO_ROOT / "src/xrt/auxiliary/vk/vk_image_allocator.c").read_text()

        self.assertIn("return DXGI_FORMAT_R32G8X24_TYPELESS;", wineopenxr)
        self.assertIn("desc.Format = dxgi_swapchain_resource_format", wineopenxr)
        self.assertIn("our_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;", wineopenxr)
        self.assertIn("our_d3d11[i].next = NULL;", wineopenxr)
        self.assertIn("our_d3d11[j].texture->lpVtbl->Release", wineopenxr)

        destroy = wineopenxr.split("XrResult WINAPI xrDestroySwapchain", 1)[1].split(
            "static D3D11_USAGE", 1
        )[0]
        self.assertLess(destroy.index("WaitForGpuIdle"), destroy.index("release_d3d11_resources"))
        self.assertLess(destroy.index("release_d3d11_resources"), destroy.index("UNIX_CALL(xrDestroySwapchain"))
        enter = destroy.index("EnterCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)")
        wait = destroy.index("WaitForGpuIdle")
        leave = destroy.index("LeaveCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)")
        release = destroy.index("release_d3d11_resources")
        self.assertLess(enter, wait)
        self.assertLess(wait, leave)
        self.assertLess(leave, release)
        self.assertEqual(destroy.count("EnterCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)"), 1)
        self.assertEqual(destroy.count("LeaveCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)"), 1)
        self.assertIn("swapchain->images = NULL;", wineopenxr)
        self.assertIn("swapchain->image_count = 0;", wineopenxr)

        wine_interop = (PROTON_ROOT / "wineopenxr/dxvk-interop.h").read_text()
        dxvk_interop = (PROTON_ROOT / "dxvk/src/d3d11/d3d11_interop.cpp").read_text()
        dxvk_interfaces = (PROTON_ROOT / "dxvk/src/dxgi/dxgi_interfaces.h").read_text()
        dxvk_device = (PROTON_ROOT / "dxvk/src/d3d11/d3d11_device.cpp").read_text()
        self.assertIn("0xa0,0x93,0x25", wine_interop)
        self.assertIn("IDXGIVkInteropDevice2 : public IDXGIVkInteropDevice1", dxvk_interfaces)
        self.assertIn("0xa0,0x93,0x25", dxvk_interfaces)
        self.assertIn("immediateContext->SynchronizeCsThread", dxvk_interop)
        self.assertIn("m_device->GetDXVKDevice()->waitForIdle", dxvk_interop)
        wait_for_idle = dxvk_interop.split("D3D11VkInterop::WaitForGpuIdle", 1)[1]
        self.assertLess(wait_for_idle.index("immediateContext->Flush"),
                        wait_for_idle.index("immediateContext->SynchronizeCsThread"))
        self.assertLess(wait_for_idle.index("immediateContext->SynchronizeCsThread"),
                        wait_for_idle.index("m_device->GetDXVKDevice()->waitForIdle"))
        self.assertIn("riid == __uuidof(IDXGIVkInteropDevice1)", dxvk_device)
        self.assertIn("riid == __uuidof(IDXGIVkInteropDevice2)", dxvk_device)

        self.assertIn("vkImage != VK_NULL_HANDLE", dxvk)
        self.assertIn("VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL", dxvk)
        self.assertIn("trackedLayout = imageInfo.layout;", dxvk)
        self.assertIn("importImage(imageInfo, vkImage, trackedLayout", dxvk)
        self.assertIn("dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;", probe)
        self.assertIn("srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;", probe)
        self.assertIn("for (uint32_t cycle = 0; cycle < 2; cycle++)", probe)
        self.assertIn("constexpr uint32_t swapchainCount = 6;", probe)
        self.assertIn("context1->DiscardView", probe)
        self.assertIn('std::strcmp(stage, "teardown")', probe)
        self.assertIn('std::strcmp(stage, "recreate")', probe)
        self.assertIn('d3d11-openxr-depth-probe.stdout.log', probe)
        self.assertIn('d3d11-openxr-depth-probe.stderr.log', probe)

        runner = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/run-guarded.sh").read_text()
        self.assertIn("expected='PASS: create and enumerate only'", runner)
        self.assertIn('grep -Fqx "$expected" "$stdout_log"', runner)

        for usage in (
            "XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL",
            "XRT_SWAPCHAIN_USAGE_SAMPLED",
            "XRT_SWAPCHAIN_USAGE_TRANSFER_SRC",
            "XRT_SWAPCHAIN_USAGE_TRANSFER_DST",
            "XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT",
        ):
            self.assertIn(usage, monado)
        self.assertIn("client_vk_local_depth_required_usage", monado)
        self.assertIn("client_vk_prepare_local_depth_create_info", monado)
        self.assertIn("vk_csci_is_local_image_format_supported", monado)
        self.assertIn("vk_ic_allocate_local", monado)
        self.assertIn("create_image(vk, xscci, false", allocator)
        self.assertIn("if (usage == 0)", support)
        local_helper = support.split("vk_csci_is_local_image_format_supported", 1)[1].split(
            "vk_csci_is_format_supported", 1
        )[0]
        self.assertNotIn("VkPhysicalDeviceExternalImageFormatInfo", local_helper)
        self.assertIn("VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT", support)
        self.assertIn(".flags = get_image_create_flags", support)


if __name__ == "__main__":
    unittest.main()
