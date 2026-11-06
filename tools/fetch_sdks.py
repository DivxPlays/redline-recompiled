"""Downloads the upscaler SDK files the runtime builds and runs with, into third_party/fsr and third_party/dlss.

Usage: python tools/fetch_sdks.py   (tools/build.py runs it when they are missing)

- AMD FSR SDK 2.4.0 (headers MIT; the signed DLLs under AMD's licence for redistributing them in binary form,
  with its notices): the ffx API and upscaler headers and the signed DLLs (the loader and FSR upscaling), all
  DirectX 12: the runtime runs them on a D3D12 device next to its D3D11 one (up12.cpp).
- NVIDIA DLSS SDK 310.9.16 (NVIDIA RTX SDKs licence): the NGX headers, the static NGX library and nvngx_dlss.dll
  (Super Resolution, which runs on D3D11 directly; dlss.c). The licence allows shipping it inside an application
  but not putting the SDK under an open-source licence, which is why none of this is committed: each build fetches
  it from NVIDIA's and AMD's GitHub releases at pinned tags, and every file is checked against its SHA-256 below.
"""
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FSR = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/"
DLSS = "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/"

# (url, destination under third_party, its SHA-256)
FILES = [
    (FSR + "api/include/ffx_api.h", "fsr/api/include/ffx_api.h", "4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf"),
    (FSR + "api/include/ffx_api_types.h", "fsr/api/include/ffx_api_types.h", "53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c"),
    (FSR + "api/include/ffx_api_loader.h", "fsr/api/include/ffx_api_loader.h", "18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e"),
    (FSR + "api/include/dx12/ffx_api_dx12.h", "fsr/api/include/dx12/ffx_api_dx12.h", "c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"),
    (FSR + "upscalers/include/ffx_upscale.h", "fsr/upscalers/include/ffx_upscale.h", "13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d"),
    (FSR + "signedbin/amd_fidelityfx_loader_dx12.dll", "fsr/bin/amd_fidelityfx_loader_dx12.dll", "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"),
    (FSR + "signedbin/amd_fidelityfx_upscaler_dx12.dll", "fsr/bin/amd_fidelityfx_upscaler_dx12.dll", "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"),
    (FSR + "docs/license.md", "fsr/license.md", "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb"),
    (DLSS + "include/nvsdk_ngx.h", "dlss/include/nvsdk_ngx.h", "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"),
    (DLSS + "include/nvsdk_ngx_defs.h", "dlss/include/nvsdk_ngx_defs.h", "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"),
    (DLSS + "include/nvsdk_ngx_params.h", "dlss/include/nvsdk_ngx_params.h", "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"),
    (DLSS + "include/nvsdk_ngx_helpers.h", "dlss/include/nvsdk_ngx_helpers.h", "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"),
    (DLSS + "include/nvsdk_ngx_helpers_d3d.h", "dlss/include/nvsdk_ngx_helpers_d3d.h", "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"),
    (DLSS + "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib", "dlss/lib/nvsdk_ngx_s.lib", "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"),
    (DLSS + "lib/Windows_x86_64/rel/nvngx_dlss.dll", "dlss/bin/nvngx_dlss.dll", "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"),
    (DLSS + "LICENSE.txt", "dlss/LICENSE.txt", "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    record = "--record" in sys.argv  # print the hashes of what is there, to pin them here
    for url, rel, want in FILES:
        dest = os.path.join(ROOT, "third_party", rel)
        if not os.path.exists(dest):
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            print(f"downloading {url}", flush=True)
            with urllib.request.urlopen(url, timeout=120) as r, open(dest + ".part", "wb") as f:
                while block := r.read(1 << 20):
                    f.write(block)
            os.replace(dest + ".part", dest)
        got = sha256(dest)
        if record:
            print(f'    ("{url}", "{rel}", "{got}"),')
        elif want and got != want:
            os.remove(dest)
            sys.exit(f"{rel}: SHA-256 {got} is not the pinned {want}; removed it, run again")
    print("SDK files ready in third_party/fsr and third_party/dlss")


if __name__ == "__main__":
    main()
