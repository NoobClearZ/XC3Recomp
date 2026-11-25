#!/usr/bin/env bash
# [XCDE] Runtime DLLs of the temporal upscalers next to build/xcde.exe (not in git: vendor binaries, ~60 MB):
# AMD FidelityFX SDK 1.1.4 (FSR 3.1, signed prebuilt) and NVIDIA Streamline 2.14.16 (DLSS). Downloaded and
# signature-checked into sdk/ (default); SDK_DIR=<dir> for another location. Get them from AMD / NVIDIA yourself.
# [XC3] AMD FidelityFX SDK 2.4.0 (FSR 3.1 + FSR 4: loader + upscaler DLLs, signed): in the repository,
# third_party/fidelityfx (MIT, its license.md); FFX2_DIR=<a signedbin folder> for another. The runtime prefers them over
# the 1.1.4 DLL. DLSS (NVIDIA Streamline) is still not redistributed: sdk/ as above.
cd "$(dirname "$0")/.."
S=${SDK_DIR:-sdk}
F=${FFX2_DIR:-third_party/fidelityfx}
case "$(uname -s)" in
  Linux*)
    mkdir -p build
    if [ -f "$S/ffx-vk-linux/build/libffx_vk_linux.so" ]; then
      for f in "$S/ffx-vk-linux/build/libffx_vk_linux.so" \
               "$F/amd_fidelityfx_loader_vk.dll" "$F/amd_fidelityfx_upscaler_vk.dll"; do
        b=build/$(basename "$f")
        [ -f "$b" ] && [ ! "$f" -nt "$b" ] && continue
        cp -f "$f" "$b" 2>/dev/null && echo "copied $(basename "$f")" || echo "missing $f (that upscaler will be unavailable)"
      done
    fi
    exit 0
    ;;
esac
for f in "$F/amd_fidelityfx_loader_vk.dll" "$F/amd_fidelityfx_upscaler_vk.dll" \
         "$S/ffx/PrebuiltSignedDLL/amd_fidelityfx_vk.dll" "$S/sl/bin/x64/sl.interposer.dll" "$S/sl/bin/x64/sl.common.dll" \
         "$S/sl/bin/x64/sl.dlss.dll" "$S/sl/bin/x64/nvngx_dlss.dll" "$S/sl/bin/x64/NvLowLatencyVk.dll"; do
  b=build/$(basename "$f")
  [ -f "$b" ] && [ ! "$f" -nt "$b" ] && continue
  cp -f "$f" "$b" 2>/dev/null && echo "copied $(basename "$f")" || echo "missing $f (that upscaler will be unavailable)"
done
