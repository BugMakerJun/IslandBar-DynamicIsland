// =====================================================================
//  ambientpreview -- every island::AmbientKind, rendered to a PNG.
//
//      ambientpreview <out-dir>
//
//  Draws the same glass pill for every kind and the ambient layer over it,
//  so the weather effects can be judged without waiting for the weather.
//  ASCII-only on purpose; the Chinese on the pill is written as \u escapes.
//
//  Not part of the shipped bar, like every other probe here.
// =====================================================================
#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <utility>
#include <string>
#include <vector>

#include "render/Ambient.h"
#include "render/Theme.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 800;
constexpr UINT kHeight = 160;

std::wstring Widen(const char* text) {
    if (!text) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring out(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
    return out;
}

ComPtr<ID2D1Bitmap1> MakeBitmap(ID2D1DeviceContext* context, D2D1_BITMAP_OPTIONS options) {
    ComPtr<ID2D1Bitmap1> bitmap;
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    context->CreateBitmap(D2D1::SizeU(kWidth, kHeight), nullptr, 0, &properties, bitmap.GetAddressOf());
    return bitmap;
}

bool SavePng(IWICImagingFactory* wic, const std::wstring& path, const std::vector<BYTE>& pixels);

bool RenderKind(ID2D1DeviceContext* context, IDWriteFactory* dwrite, ID2D1Bitmap1* target,
                ID2D1Bitmap1* readback, IDWriteTextFormat* dateFormat,
                IDWriteTextFormat* timeFormat, IDWriteTextFormat* temperatureFormat,
                const island::Theme& theme, island::AmbientKind kind, float phase,
                std::vector<BYTE>& pixels) {
    if (!context || !target || !readback) return false;

    context->SetTarget(target);
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0.42f, 0.47f, 0.54f));

    // The pill: one fixed glass body for every kind, light enough that both the warm sunset and the
    // white rain read on it.
    const D2D1_ROUNDED_RECT pill = {D2D1::RectF(20.0f, 20.0f, kWidth - 20.0f, kHeight - 20.0f),
                                    60.0f, 60.0f};
    const D2D1_GRADIENT_STOP glassStops[2] = {
        {0.0f, D2D1::ColorF(0.90f, 0.94f, 1.0f, 1.0f)},
        {1.0f, D2D1::ColorF(0.70f, 0.76f, 0.88f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> glassCollection;
    if (SUCCEEDED(context->CreateGradientStopCollection(glassStops, 2, glassCollection.GetAddressOf())) &&
        glassCollection) {
        ComPtr<ID2D1LinearGradientBrush> glass;
        context->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, pill.rect.top),
                                                D2D1::Point2F(0.0f, pill.rect.bottom)),
            glassCollection.Get(), glass.GetAddressOf());
        if (glass) context->FillRoundedRectangle(&pill, glass.Get());
    }

    island::DrawAmbient(context, pill, kind, phase, 1.0f, theme);

    ComPtr<ID2D1SolidColorBrush> rim;
    if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f),
                                                 rim.GetAddressOf())) &&
        rim) {
        context->DrawRoundedRectangle(&pill, rim.Get(), 2.0f);
    }

    // The status line, as text: the compact date on the left, the clock in the middle, the temperature
    // on the right -- the same three zones the bar draws, so the contrast can be judged too.
    ComPtr<ID2D1SolidColorBrush> ink;
    if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.05f, 0.08f, 1.0f),
                                                 ink.GetAddressOf())) &&
        ink && dwrite) {
        const wchar_t date[] = L"\u5468\u65e5 10.18";
        const wchar_t time[] = L"17:24";
        const wchar_t temperature[] = L"14\u00b0";
        if (dateFormat) {
            context->DrawTextW(date, static_cast<UINT32>(std::wcslen(date)), dateFormat,
                               D2D1::RectF(50.0f, 20.0f, 280.0f, kHeight - 20.0f), ink.Get());
        }
        if (timeFormat) {
            context->DrawTextW(time, static_cast<UINT32>(std::wcslen(time)), timeFormat,
                               D2D1::RectF(290.0f, 20.0f, 510.0f, kHeight - 20.0f), ink.Get());
        }
        if (temperatureFormat) {
            context->DrawTextW(temperature, static_cast<UINT32>(std::wcslen(temperature)),
                               temperatureFormat,
                               D2D1::RectF(520.0f, 20.0f, 750.0f, kHeight - 20.0f), ink.Get());
        }
    }

    if (FAILED(context->EndDraw())) return false;

    if (FAILED(readback->CopyFromBitmap(nullptr, target, nullptr))) return false;
    D2D1_MAPPED_RECT map = {};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &map))) return false;

    pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    for (UINT y = 0; y < kHeight; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * kWidth * 4,
                    map.bits + static_cast<size_t>(y) * map.pitch, kWidth * 4);
    }
    readback->Unmap();

    return true;
}

bool SavePng(IWICImagingFactory* wic, const std::wstring& path, const std::vector<BYTE>& pixels) {
    if (!wic || pixels.size() < static_cast<size_t>(kWidth) * kHeight * 4) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.GetAddressOf())) || !stream) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        !encoder) {
        return false;
    }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) || !frame) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(kWidth, kHeight))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels(kHeight, kWidth * 4, static_cast<UINT>(pixels.size()),
                                  const_cast<BYTE*>(pixels.data())))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

} // namespace

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring directory = (argc > 1) ? Widen(argv[1]) : L"ambient-preview";
    CreateDirectoryW(directory.c_str(), nullptr);

    // --- the device chain ------------------------------------------------
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3dContext;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 d3d.GetAddressOf(), nullptr, d3dContext.GetAddressOf())) ||
        !d3d) {
        std::printf("ambientpreview: no D3D11 device\n");
        return 1;
    }
    ComPtr<IDXGIDevice> dxgi;
    d3d.As(&dxgi);

    ComPtr<ID2D1Factory1> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(factory.GetAddressOf()))) ||
        !factory) {
        std::printf("ambientpreview: no D2D factory\n");
        return 1;
    }
    ComPtr<ID2D1Device> device;
    if (FAILED(factory->CreateDevice(dxgi.Get(), device.GetAddressOf())) || !device) {
        std::printf("ambientpreview: no D2D device\n");
        return 1;
    }
    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                           context.GetAddressOf())) ||
        !context) {
        std::printf("ambientpreview: no D2D context\n");
        return 1;
    }

    ComPtr<ID2D1Bitmap1> target = MakeBitmap(context.Get(), D2D1_BITMAP_OPTIONS_TARGET);
    ComPtr<ID2D1Bitmap1> readback = MakeBitmap(
        context.Get(), D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    if (!target || !readback) {
        std::printf("ambientpreview: no target bitmap\n");
        return 1;
    }

    ComPtr<IDWriteFactory> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    ComPtr<IDWriteTextFormat> dateFormat;
    ComPtr<IDWriteTextFormat> timeFormat;
    ComPtr<IDWriteTextFormat> temperatureFormat;
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 19.0f,
                                 L"zh-CN", dateFormat.GetAddressOf());
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_EXTRA_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40.0f,
                                 L"zh-CN", timeFormat.GetAddressOf());
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 19.0f,
                                 L"zh-CN", temperatureFormat.GetAddressOf());
        if (dateFormat) {
            dateFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dateFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (timeFormat) {
            timeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            timeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (temperatureFormat) {
            temperatureFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            temperatureFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(wic.GetAddressOf()));

    island::Theme theme = {};
    theme.highlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

    struct Entry {
        const char* name;
        island::AmbientKind kind;
    };
    const Entry entries[] = {
        {"01-clear-day", island::AmbientKind::ClearDay},
        {"02-sunset", island::AmbientKind::Sunset},
        {"03-night", island::AmbientKind::Night},
        {"04-rain", island::AmbientKind::Rain},
        {"05-snow", island::AmbientKind::Snow},
        {"06-cloud", island::AmbientKind::Cloud},
        {"07-fog", island::AmbientKind::Fog},
        {"08-thunder", island::AmbientKind::Thunder},
    };

    int written = 0;
    for (const Entry& entry : entries) {
        std::vector<BYTE> pixels;
        if (!RenderKind(context.Get(), dwrite.Get(), target.Get(), readback.Get(),
                        dateFormat.Get(), timeFormat.Get(), temperatureFormat.Get(), theme,
                        entry.kind, 3.0f, pixels)) {
            std::printf("ambientpreview: %s failed to render\n", entry.name);
            continue;
        }
        const std::wstring path = directory + L"\\" + Widen(entry.name) + L".png";
        if (!SavePng(wic.Get(), path, pixels)) {
            std::printf("ambientpreview: %s failed to save\n", entry.name);
            continue;
        }
        std::printf("ambientpreview: wrote %ls\n", path.c_str());
        ++written;
    }

    std::printf("ambientpreview: %d image(s)\n", written);
    return written == static_cast<int>(sizeof(entries) / sizeof(entries[0])) ? 0 : 1;
}



