/*
 * screenshot.cpp -- Saves a PNG of a monitor with DXGI desktop duplication, for the Capture button.
 *
 * HDR desktops are captured in 16-bit float and mapped to SDR the way Windows shows SDR content: the display's SDR
 * white level becomes white, brighter highlights roll off instead of clipping.
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <DirectXPackedVector.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

extern "C"
{
#include "screenshot.h"
#include "tray.h"
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

template <class T> static void release(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

// SDR white level of the display behind a GDI device name such as \\.\DISPLAY2, in nits; 80 when unknown
static float sdr_white_nits(const WCHAR *gdi_name)
{
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
    {
        return 80.0f;
    }
    DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)calloc(path_count, sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)calloc(mode_count, sizeof(DISPLAYCONFIG_MODE_INFO));
    float nits = 80.0f;
    if (paths != nullptr && modes != nullptr &&
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, nullptr) == ERROR_SUCCESS)
    {
        for (UINT32 i = 0; i < path_count; i++)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                wcscmp(source.viewGdiDeviceName, gdi_name) != 0)
            {
                continue;
            }
            DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = paths[i].targetInfo.adapterId;
            white.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            {
                nits = white.SDRWhiteLevel * 80.0f / 1000.0f; // 1000 = 80 nits
            }
            break;
        }
    }
    free(paths);
    free(modes);
    return nits;
}

// Linear 0..1 to 8-bit sRGB, through a table (a 5120x1440 frame has 7.4 million pixels)
#define SRGB_TABLE_SIZE 4096
static BYTE srgb_table[SRGB_TABLE_SIZE + 1];

static void init_srgb_table()
{
    for (int i = 0; i <= SRGB_TABLE_SIZE; i++)
    {
        float linear = (float)i / SRGB_TABLE_SIZE;
        float s = linear <= 0.0031308f ? 12.92f * linear : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
        srgb_table[i] = (BYTE)(s * 255.0f + 0.5f);
    }
}

static BYTE hdr_to_srgb8(float value, float white_scale)
{
    float v = value * white_scale;
    if (!(v > 0.0f)) // negative (outside sRGB colours) or NaN
    {
        return 0;
    }
    // Highlights above 80 % of SDR white roll off towards white instead of clipping
    const float knee = 0.8f;
    if (v > knee)
    {
        v = knee + (1.0f - knee) * (1.0f - expf(-(v - knee) / (1.0f - knee)));
    }
    return srgb_table[(int)(v * SRGB_TABLE_SIZE + 0.5f)];
}

// The interactive user's Screenshots folder, created if missing
static bool screenshots_folder(WCHAR *folder, size_t size)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &path)))
    {
        return false;
    }
    wcsncpy_s(folder, size, path, _TRUNCATE);
    CoTaskMemFree(path);
    return true;
}

static HRESULT save_png(const WCHAR *path, UINT width, UINT height, BYTE *bgra)
{
    IWICImagingFactory *wic = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(height, width * 4, width * height * 4, bgra);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    release(frame);
    release(encoder);
    release(stream);
    release(wic);
    if (FAILED(hr))
    {
        DeleteFileW(path);
    }
    return hr;
}

// Copies the current image of the output that shows `monitor` into an 8-bit BGRA buffer (caller frees it)
static HRESULT capture(HMONITOR monitor, BYTE **pixels, UINT *width, UINT *height)
{
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    IDXGIOutput1 *output1 = nullptr;
    IDXGIOutput5 *output5 = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGIOutputDuplication *duplication = nullptr;
    IDXGIResource *resource = nullptr;
    ID3D11Texture2D *frame = nullptr;
    ID3D11Texture2D *staging = nullptr;
    DXGI_OUTPUT_DESC output_desc = {};

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    for (UINT a = 0; SUCCEEDED(hr) && output == nullptr && factory->EnumAdapters1(a, &adapter) == S_OK; a++)
    {
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; o++)
        {
            if (SUCCEEDED(output->GetDesc(&output_desc)) && output_desc.Monitor == monitor)
            {
                break;
            }
            release(output);
        }
        if (output == nullptr)
        {
            release(adapter);
        }
    }
    if (SUCCEEDED(hr) && output == nullptr) hr = DXGI_ERROR_NOT_FOUND;
    if (SUCCEEDED(hr))
    {
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                               nullptr, &context);
    }
    if (SUCCEEDED(hr))
    {
        // HDR desktops need DuplicateOutput1 with a float format; it requires per-monitor DPI awareness
        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output->QueryInterface(IID_PPV_ARGS(&output5));
        if (SUCCEEDED(hr)) hr = output5->DuplicateOutput1(device, 0, ARRAYSIZE(formats), formats, &duplication);
        if (FAILED(hr))
        {
            hr = output->QueryInterface(IID_PPV_ARGS(&output1));
            if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(device, &duplication);
        }
        if (previous != nullptr)
        {
            SetThreadDpiAwarenessContext(previous);
        }
    }

    // The first frame can arrive without an image yet; a game presents new frames constantly, so wait for one
    bool have_image = false;
    for (int attempt = 0; SUCCEEDED(hr) && !have_image && attempt < 5; attempt++)
    {
        DXGI_OUTDUPL_FRAME_INFO info = {};
        HRESULT acquired = duplication->AcquireNextFrame(attempt == 0 ? 500 : 100, &info, &resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT)
        {
            have_image = staging != nullptr; // nothing changed on screen: the copy taken so far is current
            continue;
        }
        hr = acquired;
        if (SUCCEEDED(hr)) hr = resource->QueryInterface(IID_PPV_ARGS(&frame));
        if (SUCCEEDED(hr) && staging == nullptr)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            frame->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            hr = device->CreateTexture2D(&desc, nullptr, &staging);
        }
        if (SUCCEEDED(hr))
        {
            context->CopyResource(staging, frame);
            have_image = info.LastPresentTime.QuadPart != 0;
        }
        release(frame);
        release(resource);
        duplication->ReleaseFrame();
    }
    if (SUCCEEDED(hr) && staging == nullptr) hr = DXGI_ERROR_WAIT_TIMEOUT;

    if (SUCCEEDED(hr))
    {
        D3D11_TEXTURE2D_DESC desc = {};
        staging->GetDesc(&desc);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        BYTE *out = (BYTE *)malloc((size_t)desc.Width * desc.Height * 4);
        hr = out != nullptr ? context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            float white_scale = 80.0f / sdr_white_nits(output_desc.DeviceName);
            for (UINT y = 0; y < desc.Height; y++)
            {
                const BYTE *row = (const BYTE *)mapped.pData + (size_t)y * mapped.RowPitch;
                BYTE *dst = out + (size_t)y * desc.Width * 4;
                if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    const DirectX::PackedVector::HALF *src = (const DirectX::PackedVector::HALF *)row;
                    for (UINT x = 0; x < desc.Width; x++, src += 4, dst += 4)
                    {
                        dst[0] = hdr_to_srgb8(DirectX::PackedVector::XMConvertHalfToFloat(src[2]), white_scale);
                        dst[1] = hdr_to_srgb8(DirectX::PackedVector::XMConvertHalfToFloat(src[1]), white_scale);
                        dst[2] = hdr_to_srgb8(DirectX::PackedVector::XMConvertHalfToFloat(src[0]), white_scale);
                        dst[3] = 255;
                    }
                }
                else
                {
                    memcpy(dst, row, (size_t)desc.Width * 4);
                    for (UINT x = 0; x < desc.Width; x++)
                    {
                        dst[x * 4 + 3] = 255;
                    }
                }
            }
            context->Unmap(staging, 0);
            *pixels = out;
            *width = desc.Width;
            *height = desc.Height;
        }
        else
        {
            free(out);
        }
    }

    release(staging);
    release(duplication);
    release(context);
    release(device);
    release(output5);
    release(output1);
    release(output);
    release(adapter);
    release(factory);
    return hr;
}

extern "C" BOOL take_screenshot(void)
{
    static INIT_ONCE table_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(
        &table_once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL { init_srgb_table(); return TRUE; }, nullptr, nullptr);

    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    BYTE *pixels = nullptr;
    UINT width = 0, height = 0;
    WCHAR folder[MAX_PATH];
    WCHAR path[MAX_PATH];

    HRESULT hr = capture(monitor, &pixels, &width, &height);
    if (SUCCEEDED(hr))
    {
        hr = screenshots_folder(folder, ARRAYSIZE(folder)) ? S_OK : E_FAIL;
    }
    if (SUCCEEDED(hr))
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        swprintf_s(path, L"%s\\Screenshot %04u-%02u-%02u %02u%02u%02u.png", folder, now.wYear, now.wMonth, now.wDay,
                   now.wHour, now.wMinute, now.wSecond);
        for (int n = 2; GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && n < 100; n++)
        {
            swprintf_s(path, L"%s\\Screenshot %04u-%02u-%02u %02u%02u%02u (%d).png", folder, now.wYear, now.wMonth,
                       now.wDay, now.wHour, now.wMinute, now.wSecond, n);
        }
        hr = save_png(path, width, height, pixels);
    }
    free(pixels);

    if (FAILED(hr))
    {
        WCHAR text[128];
        swprintf_s(text, L"Screenshot failed (error 0x%08lX)", (unsigned long)hr);
        tray_show_notification(NT_TRAY_WARNING, (LPTSTR)L"Stadia Controller", text);
    }
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    return SUCCEEDED(hr);
}
