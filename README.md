# foxpostfx  (libfoxpostfx.so for the GTA V Android APK)

Vulkan loader shim: runs `foxfx_net` -> `foxfx_detail` -> `foxfx_final` fullscreen passes on the
swapchain image right before `vkQueuePresentKHR`. No inline hook / Dobby needed.

## Build on GitHub
1. Push this folder to a repo (branch `main`).
2. Actions -> **build-libfoxpostfx** -> *Run workflow* (also runs on every push).
3. Download artifact **libfoxpostfx-arm64-v8a**:
   - `normal/libfoxpostfx.so` (+ `foxpostfx.cfg`)
   - `dropin/libgtavvk.so`    (+ `foxpostfx.cfg`)
   Tag a commit `v1` to also get a Release.
Toolchain: NDK r27c (27.2.12479018), arm64-v8a, android-26, c++_static, 16 KB page size, glslc from the NDK.

## Put it into the APK  (pick ONE)
**A. Drop-in (no dex edit)** - replace `lib/arm64-v8a/libgtavvk.so` with `dropin/libgtavvk.so`, re-sign.
It chains to `libgtav_vulkan.so` (what the original 6 KB bridge loads).

**B. Separate lib** - add `libfoxpostfx.so` to `lib/arm64-v8a/` and make the app set
`GTAV_VULKAN_LIBRARY` to `libfoxpostfx.so` instead of `libgtavvk.so` (smali const-string edit).
It chains to `libgtavvk.so`. Env override for the next lib: `FOXPOSTFX_NEXT`.

## Config
`foxpostfx.cfg` is searched in: `$FOXPOSTFX_CFG`, next to the .so, then
`/sdcard/Android/data/COM.FOXENT.GTAV/files/foxpostfx.cfg`, `/storage/emulated/0/Download/foxpostfx.cfg`.
Live reload (~2 s). `enable=0` turns everything off, `net/detail/final=0` turns single passes off.

## Check it works
`adb logcat -s foxpostfx` should show: `next vkGetInstanceProcAddr from ...` and `swapchain WxH ... postfx ready`.

## Limits (be aware)
- UNTESTED on device. Not compiled locally (no Vulkan headers here) - first CI run may need a fix; send the log.
- Only 8-bit RGBA/BGRA swapchains (UNORM/SRGB). Anything else (HDR/10-bit) passes through untouched.
- Gloss/car reflection needs `uGlossMask`; a black dummy is bound for now, so only road reflection + bloom + detail act.
- `foxfx_net.frag` is an analytic approximation of `fox_net_custom.h`, not the conv net itself.
- Runs on the final swapchain image, so HUD/minimap are processed too (`hud=` rect only masks road reflection).
- Cost: 2 image copies + up to 3 fullscreen passes per frame, fence wait per swapchain image.
- Known shader issues (not changed): banding in road reflection (fixed march step), reflection a bit strong.
