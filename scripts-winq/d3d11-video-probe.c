/*
 * Host-side D3D11 Video Decoder probe.
 *
 * Standalone diagnostic: initialises D3D11 the same way the winq-emu
 * virglrenderer Windows backend does, enumerates video decoder profiles,
 * and prints which codecs + output formats the host GPU supports. If this
 * program shows H264 / H265 / AV1 profiles, the Alpha 6 VA-API backend has
 * the infrastructure it needs; any issues thereafter are either in the
 * backend code or in the virtio-9p/virtio-gpu wiring into the guest.
 *
 * Build (MSYS2 UCRT64):
 *   gcc -o d3d11-video-probe.exe d3d11-video-probe.c \
 *       -ld3d11 -ldxgi -ldxguid -luuid
 */

#define COBJMACROS
#define CINTERFACE
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <d3d11.h>

static const struct {
    const GUID *guid;
    const char *name;
} kProfiles[] = {
    { &D3D11_DECODER_PROFILE_H264_VLD_NOFGT,   "H.264 VLD (no FGT)" },
    { &D3D11_DECODER_PROFILE_H264_VLD_FGT,     "H.264 VLD (FGT)" },
    { &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,    "H.265 Main" },
    { &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,  "H.265 Main10" },
    { &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, "VP9 profile 0" },
    { &D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2, "VP9 profile 2 (10bit)" },
    { &D3D11_DECODER_PROFILE_MPEG2_VLD,        "MPEG-2 VLD" },
    { &D3D11_DECODER_PROFILE_MPEG1_VLD,        "MPEG-1 VLD" },
};

/* AV1 GUIDs are only in newer SDK headers; declare as a local copy. */
static const GUID k_AV1_VLD_PROFILE0 = { 0xb8be4ccb, 0xcf53, 0x46ba,
    { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } };

static void guid_to_str(const GUID *g, char out[40]) {
    snprintf(out, 40, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
             g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

int main(void) {
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11VideoDevice *vdev = NULL;
    D3D_FEATURE_LEVEL got_fl;
    HRESULT hr;

    const D3D_FEATURE_LEVEL req_fl[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                           req_fl, (UINT)(sizeof(req_fl)/sizeof(req_fl[0])),
                           D3D11_SDK_VERSION,
                           &dev, &got_fl, &ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    printf("D3D11 device created, feature level 0x%04X\n", got_fl);

    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **)&vdev);
    if (FAILED(hr)) {
        fprintf(stderr, "QueryInterface(ID3D11VideoDevice) failed: 0x%08lX\n",
                (unsigned long)hr);
        return 2;
    }
    printf("ID3D11VideoDevice acquired\n");

    UINT n = ID3D11VideoDevice_GetVideoDecoderProfileCount(vdev);
    printf("\nHost exposes %u decoder profile GUID(s):\n", n);
    for (UINT i = 0; i < n; i++) {
        GUID g;
        if (SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderProfile(vdev, i, &g))) {
            char sg[40];
            const char *nm = "(unknown)";
            for (size_t k = 0; k < sizeof(kProfiles)/sizeof(kProfiles[0]); k++) {
                if (IsEqualGUID(&g, kProfiles[k].guid)) { nm = kProfiles[k].name; break; }
            }
            if (IsEqualGUID(&g, &k_AV1_VLD_PROFILE0)) nm = "AV1 profile 0";
            guid_to_str(&g, sg);
            printf("  [%02u] %s  — %s\n", i, sg, nm);

            BOOL supported = FALSE;
            ID3D11VideoDevice_CheckVideoDecoderFormat(vdev, &g, DXGI_FORMAT_NV12, &supported);
            printf("       NV12 output: %s\n", supported ? "YES" : "no");
        }
    }

    /*
     * Exercise the same path the backend uses in create_codec: pick
     * H264_VLD_NOFGT at 1920x1080 and instantiate a decoder.
     */
    printf("\nSmoke-test: create H.264 1080p NV12 decoder...\n");
    D3D11_VIDEO_DECODER_DESC desc = {
        .Guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
        .SampleWidth = 1920, .SampleHeight = 1080,
        .OutputFormat = DXGI_FORMAT_NV12,
    };
    UINT cfg_count = 0;
    hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(vdev, &desc, &cfg_count);
    if (FAILED(hr) || cfg_count == 0) {
        printf("  No H.264 decoder configs on this host: 0x%08lX\n", (unsigned long)hr);
    } else {
        D3D11_VIDEO_DECODER_CONFIG cfg;
        ID3D11VideoDevice_GetVideoDecoderConfig(vdev, &desc, 0, &cfg);
        printf("  %u config(s); config[0] ConfigBitstreamRaw=%u\n",
               cfg_count, cfg.ConfigBitstreamRaw);
        ID3D11VideoDecoder *dec = NULL;
        hr = ID3D11VideoDevice_CreateVideoDecoder(vdev, &desc, &cfg, &dec);
        if (SUCCEEDED(hr)) {
            printf("  CreateVideoDecoder succeeded — backend init path works.\n");
            ID3D11VideoDecoder_Release(dec);
        } else {
            printf("  CreateVideoDecoder failed: 0x%08lX\n", (unsigned long)hr);
        }
    }

    ID3D11VideoDevice_Release(vdev);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    return 0;
}
