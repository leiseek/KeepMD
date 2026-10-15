// Direct2D's SVG interfaces are declared only for the Windows 10 Creators Update headers (RS2); the rest of the
// build targets plain Windows 10. KeepMD ships only for Windows 11, which always provides them.
#define NTDDI_VERSION 0x0A000003
#include "image_decode.h"
#include <algorithm>
#include <cmath>
#include <d2d1_3.h>
#include <fstream>
#include <limits>
#include <string_view>
#include <wincodec.h>
#include <wrl/client.h>

namespace keepmd {
namespace {
using Microsoft::WRL::ComPtr;

constexpr uint32_t kMaxEdge = 16384;
constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;
constexpr float kMaxRaster = 1600;
constexpr size_t kMaxSvgBytes = 4 * 1024 * 1024;
// Direct2D renders a subset of SVG and silently drops everything else. These elements carry meaning that would
// vanish without a trace — badge labels, class-based colours, masked, patterned, symbol or HTML content — so a
// document using any of them is shown as a placeholder rather than as a misleading partial picture.
constexpr std::string_view kSvgDropped[] = {"<text",  "<tspan",   "<textpath", "<foreignobject",
                                            "<style", "<mask",    "<pattern",  "<symbol"};

ImagePixels failure(ImageFailure kind, std::wstring name = {}) {
    ImagePixels result;
    result.failure = kind;
    result.error = image_failure_message(kind, name);
    return result;
}

ImagePixels decode(IWICImagingFactory *factory, IWICBitmapDecoder *decoder) {
    ImagePixels result;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, frame.GetAddressOf());
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr))
        hr = frame->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (!w || !h || w > kMaxEdge || h > kMaxEdge || (uint64_t)w * h > kMaxPixels))
        return failure(ImageFailure::TooLarge);
    if (SUCCEEDED(hr)) {
        float scale = std::min({1.f, kMaxRaster / w, kMaxRaster / h});
        result.width = std::max(1u, (UINT)(w * scale));
        result.height = std::max(1u, (UINT)(h * scale));
        ComPtr<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = scaler->Initialize(frame.Get(), result.width, result.height, WICBitmapInterpolationModeFant);
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr))
            hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                       nullptr, 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) {
            result.pixels.resize((size_t)result.width * result.height * 4);
            hr = converter->CopyPixels(nullptr, result.width * 4, (UINT)result.pixels.size(), result.pixels.data());
        }
    }
    if (FAILED(hr))
        return failure(ImageFailure::Unsupported);
    return result;
}

std::string lower_ascii(std::span<const uint8_t> bytes) {
    std::string text(bytes.begin(), bytes.end());
    for (auto &c : text)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return text;
}

// SVG is XML text, so it is recognised by its root element rather than a magic number. Only consulted once
// WIC has rejected the bytes, so raster images never take this path.
bool looks_like_svg(std::string_view lower) {
    return lower.substr(0, 64 * 1024).find("<svg") != std::string_view::npos;
}

float svg_length(ID2D1SvgElement *root, const wchar_t *name) {
    if (!root->IsAttributeSpecified(name))
        return 0;
    D2D1_SVG_LENGTH length{};
    if (FAILED(root->GetAttributeValue(name, D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &length, sizeof(length))) ||
        length.units != D2D1_SVG_LENGTH_UNITS_NUMBER || !(length.value > 0))
        return 0;
    return length.value;
}

// Intrinsic size in layout units, the way browsers size an <img> of an SVG: explicit width/height, else the
// viewBox, else 300×150; one explicit side with a viewBox keeps the viewBox aspect ratio.
D2D1_SIZE_F svg_size(ID2D1SvgDocument *document) {
    ComPtr<ID2D1SvgElement> root;
    document->GetRoot(root.GetAddressOf());
    if (!root)
        return {0, 0};
    float w = svg_length(root.Get(), L"width"), h = svg_length(root.Get(), L"height");
    D2D1_SVG_VIEWBOX box{};
    bool hasBox = root->IsAttributeSpecified(L"viewBox") &&
                  SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box,
                                                    sizeof(box))) &&
                  box.width > 0 && box.height > 0;
    if (!w && !h) {
        w = hasBox ? box.width : 300;
        h = hasBox ? box.height : 150;
    } else if (!w)
        w = hasBox ? h * box.width / box.height : 300;
    else if (!h)
        h = hasBox ? w * box.height / box.width : 150;
    return {w, h};
}

// A WIC-backed software Direct2D context over a fresh w×h bitmap, with the SVG parsed for it. Device-dependent
// SVG documents belong to the context that created them, so each target parses its own.
HRESULT svg_target(IWICImagingFactory *wic, ID2D1Factory *d2d, IStream *stream, UINT w, UINT h,
                   D2D1_SIZE_F viewport, ComPtr<IWICBitmap> &bitmap, ComPtr<ID2D1DeviceContext5> &context,
                   ComPtr<ID2D1SvgDocument> &document) {
    HRESULT hr = wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                   bitmap.ReleaseAndGetAddressOf());
    ComPtr<ID2D1RenderTarget> target;
    if (SUCCEEDED(hr))
        hr = d2d->CreateWicBitmapRenderTarget(
            bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            target.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = target.As(&context);
    LARGE_INTEGER start{};
    if (SUCCEEDED(hr))
        hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(hr))
        hr = context->CreateSvgDocument(stream, viewport, document.ReleaseAndGetAddressOf());
    return hr;
}

ImagePixels decode_svg(std::span<const uint8_t> bytes, uint32_t requestedWidth, uint32_t requestedHeight) {
    if (bytes.size() > kMaxSvgBytes)
        return failure(ImageFailure::TooLarge);
    auto lower = lower_ascii(bytes);
    for (auto element : kSvgDropped)
        if (lower.find(element) != std::string::npos)
            return failure(ImageFailure::SvgUnsupported);

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<ID2D1Factory> d2d;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = wic->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), (DWORD)bytes.size());
    if (SUCCEEDED(hr))
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(d2d.GetAddressOf()));
    // Parse once on a 1×1 probe to learn the intrinsic size, then again on a target of the final size.
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1DeviceContext5> context;
    ComPtr<ID2D1SvgDocument> document;
    if (SUCCEEDED(hr))
        hr = svg_target(wic.Get(), d2d.Get(), stream.Get(), 1, 1, D2D1::SizeF(1, 1), bitmap, context, document);
    if (FAILED(hr))
        return failure(ImageFailure::Unsupported);
    auto size = svg_size(document.Get());
    if (!(size.width > 0 && size.height > 0) || size.width > kMaxEdge || size.height > kMaxEdge)
        return failure(ImageFailure::TooLarge);

    // Rasterize at twice the size the image will be shown, matching image_extent(): a requested width wins,
    // else a requested height, and both together scale to the larger of the two.
    float magnify = 1;
    if (requestedWidth && requestedHeight)
        magnify = std::max(requestedWidth / size.width, requestedHeight / size.height);
    else if (requestedWidth)
        magnify = requestedWidth / size.width;
    else if (requestedHeight)
        magnify = requestedHeight / size.height;
    ImagePixels result;
    result.density = std::min(2.f * std::max(1.f, magnify), kMaxRaster / std::max(size.width, size.height));
    result.width = std::max(1u, (UINT)std::lround(size.width * result.density));
    result.height = std::max(1u, (UINT)std::lround(size.height * result.density));
    hr = svg_target(wic.Get(), d2d.Get(), stream.Get(), result.width, result.height, size, bitmap, context, document);
    if (SUCCEEDED(hr)) {
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0, 0, 0));
        context->SetTransform(D2D1::Matrix3x2F::Scale(result.density, result.density));
        context->DrawSvgDocument(document.Get());
        hr = context->EndDraw();
    }
    if (SUCCEEDED(hr)) {
        result.pixels.resize((size_t)result.width * result.height * 4);
        hr = bitmap->CopyPixels(nullptr, result.width * 4, (UINT)result.pixels.size(), result.pixels.data());
    }
    if (FAILED(hr))
        return failure(ImageFailure::Unsupported);
    return result;
}

// Bytes WIC rejected: SVG gets its own decoder, anything else stays unsupported.
ImagePixels decode_non_raster(std::span<const uint8_t> bytes, uint32_t requestedWidth, uint32_t requestedHeight) {
    if (looks_like_svg(lower_ascii(bytes.subspan(0, std::min<size_t>(bytes.size(), 64 * 1024)))))
        return decode_svg(bytes, requestedWidth, requestedHeight);
    return failure(ImageFailure::Unsupported);
}
} // namespace

std::wstring image_failure_message(ImageFailure failure, const std::wstring &name) {
    switch (failure) {
    case ImageFailure::None:
        return {};
    case ImageFailure::Missing:
        return name.empty() ? L"找不到图片文件" : L"找不到图片：" + name;
    case ImageFailure::Unreadable:
        return name.empty() ? L"无法读取图片" : L"无法读取图片：" + name;
    case ImageFailure::Unsupported:
        return L"图片无法解码或格式暂不支持";
    case ImageFailure::TooLarge:
        return L"图片超过预览上限（最长边 16384、总计 64M 像素，SVG 源文件 4 MiB）";
    case ImageFailure::SvgUnsupported:
        return L"SVG 含文字、样式表或遮罩等暂不支持的内容";
    }
    return {};
}

ImagePixels decode_image_file(const std::filesystem::path &path, uint32_t requestedWidth, uint32_t requestedHeight) {
    // WIC reports a missing file and an unopenable one with the same HRESULT, so probe the path first to
    // give the reader the reason that actually applies.
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status))
        return failure(ImageFailure::Missing, path.filename().wstring());
    if (!std::filesystem::is_regular_file(status))
        return failure(ImageFailure::Unreadable, path.filename().wstring());

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(stream.GetAddressOf());
    // Opening and format detection are separate steps so each failure keeps its own reason: an open failure
    // on a present regular file is I/O or sharing, while a decoder that rejects the bytes is a format problem.
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_READ);
    if (FAILED(hr))
        return failure(ImageFailure::Unreadable, path.filename().wstring());
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                          decoder.GetAddressOf());
    if (SUCCEEDED(hr))
        return decode(factory.Get(), decoder.Get());
    // Read at most one byte past the SVG limit, so an oversized file is rejected without loading it whole.
    auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return failure(ImageFailure::Unreadable, path.filename().wstring());
    std::vector<uint8_t> bytes((size_t)std::min<uintmax_t>(size, kMaxSvgBytes + 1));
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), (std::streamsize)bytes.size()))
        return failure(ImageFailure::Unreadable, path.filename().wstring());
    return decode_non_raster(bytes, requestedWidth, requestedHeight);
}

ImagePixels decode_image_bytes(std::span<const uint8_t> bytes, uint32_t requestedWidth, uint32_t requestedHeight) {
    if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max())
        return failure(ImageFailure::Unsupported);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), (DWORD)bytes.size());
    if (FAILED(hr))
        return failure(ImageFailure::Unsupported);
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                          decoder.GetAddressOf());
    if (SUCCEEDED(hr))
        return decode(factory.Get(), decoder.Get());
    return decode_non_raster(bytes, requestedWidth, requestedHeight);
}
} // namespace keepmd
