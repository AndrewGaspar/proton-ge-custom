#!/usr/bin/env python3

import pathlib
import unittest


PROTON_ROOT = pathlib.Path(__file__).resolve().parents[2]


class StagedColorState:
    """CPU model of the proxy/transport image and FIFO release contract."""

    def __init__(self, image_count=3):
        self.available = list(range(image_count))
        self.acquired = []
        self.proxy_layout = ["canonical"] * image_count
        self.host_layout = ["color"] * image_count
        self.copy_count = [0] * image_count

    def acquire(self):
        index = self.available.pop(0)
        self.acquired.append(index)
        return index

    def release_oldest(self):
        index = self.acquired.pop(0)
        assert self.proxy_layout[index] == "canonical"
        assert self.host_layout[index] == "color"
        self.proxy_layout[index] = "transfer-src"
        self.host_layout[index] = "transfer-dst"
        self.copy_count[index] += 1
        self.proxy_layout[index] = "canonical"
        self.host_layout[index] = "color"
        self.available.append(index)
        return index


class StagedColorContractTest(unittest.TestCase):
    def test_pair_verifier_checks_generated_table_and_both_handshake_halves(self):
        verifier = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/verify_wineopenxr_pair.py").read_text()
        self.assertIn("__wine_unix_call_funcs", verifier)
        self.assertIn("expected_bytes = len(entries) * 8", verifier)
        self.assertIn("PE/Unix ABI handshake failed", verifier)
        self.assertIn("PE/Unix ABI magic mismatch", verifier)

    def test_private_pe_unix_abi_is_handshaken(self):
        loader = (PROTON_ROOT / "wineopenxr/openxr_loader.c").read_text()
        header = (PROTON_ROOT / "wineopenxr/openxr_loader.h").read_text()
        unix = (PROTON_ROOT / "wineopenxr/openxr.c").read_text()

        self.assertIn("WINEOPENXR_UNIX_ABI_MAGIC", header)
        self.assertIn("WINEOPENXR_UNIX_ABI_REQUEST", header)
        self.assertIn("pe_abi_magic", header)
        self.assertIn("unix_abi_magic", header)
        self.assertIn("unix_call_count", header)
        self.assertIn(".pe_abi_magic = WINEOPENXR_UNIX_ABI_MAGIC", loader)
        self.assertIn(".create_instance_callback = WINEOPENXR_UNIX_ABI_REQUEST", loader)
        self.assertIn("params.unix_call_count != unix_count", loader)
        self.assertIn("params->pe_abi_magic != WINEOPENXR_UNIX_ABI_MAGIC", unix)
        marker_check = "params->create_instance_callback != WINEOPENXR_UNIX_ABI_REQUEST"
        self.assertIn(marker_check, unix)
        self.assertLess(unix.index(marker_check), unix.index("params->pe_abi_magic"))
        self.assertIn("params->unix_call_count = unix_count", unix)

    def test_release_uses_wait_order_and_restores_boundaries(self):
        state = StagedColorState()
        first = state.acquire()
        second = state.acquire()

        self.assertEqual(state.release_oldest(), first)
        self.assertEqual(state.release_oldest(), second)
        self.assertEqual(state.proxy_layout, ["canonical"] * 3)
        self.assertEqual(state.host_layout, ["color"] * 3)
        self.assertEqual(sum(state.copy_count), 2)

    def test_repeated_and_static_lifecycles(self):
        repeated = StagedColorState(2)
        for _ in range(8):
            index = repeated.acquire()
            self.assertEqual(repeated.release_oldest(), index)
        self.assertEqual(sum(repeated.copy_count), 8)

        static = StagedColorState(1)
        self.assertEqual(static.acquire(), 0)
        self.assertEqual(static.release_oldest(), 0)
        self.assertEqual(static.copy_count, [1])

    def test_source_keeps_experimental_path_narrow_and_ordered(self):
        loader = (PROTON_ROOT / "wineopenxr/openxr_loader.c").read_text()
        header = (PROTON_ROOT / "wineopenxr/openxr_loader.h").read_text()
        carrier = (
            PROTON_ROOT
            / "patches/wine-hotfixes/pending/0002-wineopenxr-staged-cross-gpu-color.patch"
        ).read_text()

        self.assertIn('STAGED_COLOR_ENV "WINEOPENXR_STAGED_CROSS_GPU_COLOR"', loader)
        self.assertIn("if (!session->staged_color_enabled)", loader)
        self.assertIn("info->sampleCount != 1", loader)
        self.assertIn("XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT", loader)
        self.assertIn("our_createInfo.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT", loader)
        self.assertIn("ID3D11Device_CreateTexture2D", loader)
        self.assertIn("GetVulkanImageInfo", loader)
        self.assertIn("proxy_info.format != expected_format", loader)
        self.assertIn("proxy_info.tiling != VK_IMAGE_TILING_OPTIMAL", loader)
        self.assertIn("for (mip = 0; mip < proxy_info->mipLevels; mip++)", loader)
        self.assertIn("proxy_info->arrayLayers", loader)
        self.assertIn("VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL", loader)
        self.assertIn("VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL", loader)
        self.assertIn("VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL", loader)
        self.assertIn("collect_staged_color_timestamp", loader)
        self.assertIn("staged_color_copy_total_ns", header)
        self.assertIn("VkCommandPool staged_color_command_pool", header)
        session_fields = header.split("typedef struct {\n  XrSession host_session", 1)[1].split(
            "} wine_XrSession;", 1
        )[0]
        swapchain_fields = header.split("typedef struct {\n  XrSwapchain host_swapchain", 1)[1].split(
            "} wine_XrSwapchain;", 1
        )[0]
        self.assertNotIn("VkCommandPool staged_color_command_pool", session_fields)
        self.assertIn("VkCommandPool staged_color_command_pool", swapchain_fields)

        self.assertIn(
            "Contract: staged-color command pools are swapchain-owned, not session-owned.",
            carrier,
        )
        carrier_header = carrier.split(
            "diff --git i/dlls/wineopenxr/openxr_loader.h", 1
        )[1]
        carrier_session = carrier_header.split(
            "   XrSession host_session;", 1
        )[1].split("+typedef struct {\n+  VkImage host_image;", 1)[0]
        carrier_swapchain = carrier_header.split(
            "   XrSwapchain host_swapchain;", 1
        )[1]
        self.assertNotIn("VkCommandPool staged_color_command_pool", carrier_session)
        self.assertIn("VkCommandPool staged_color_command_pool", carrier_swapchain)
        self.assertIn(
            "+      .commandPool = swapchain->staged_color_command_pool,", carrier
        )
        self.assertNotIn(
            "+      .commandPool = session->staged_color_command_pool,", carrier
        )

        release = loader.split("XrResult WINAPI xrReleaseSwapchainImage", 1)[1].split(
            "XrResult WINAPI xrBeginFrame", 1
        )[0]
        lock_helper = loader.split("static void lock_d3d_queue", 1)[1].split(
            "static void unlock_d3d_queue", 1
        )[0]
        self.assertLess(lock_helper.index("FlushRenderingCommands"), lock_helper.index("LockSubmissionQueue"))
        self.assertLess(release.index("lock_d3d_queue"), release.index("vkQueueSubmit"))
        self.assertLess(release.index("vkQueueSubmit"), release.index("UNIX_CALL(xrReleaseSwapchainImage"))
        self.assertLess(release.index("UNIX_CALL(xrReleaseSwapchainImage"), release.rindex("unlock_d3d_queue"))
        locked = release.split("lock_d3d_queue", 1)[1].rsplit("unlock_d3d_queue", 1)[0]
        for d3d_call in ("CreateTexture2D", "GetVulkanImageInfo", "TransitionSurfaceLayout"):
            self.assertNotIn(d3d_call, locked)


if __name__ == "__main__":
    unittest.main()
