#include <WindowsHost/SceneMaterials.h>

#include <cstdint>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <Assets/AssetCatalog.h>
#include <Platform/FileSystem.h>

#include "SceneMaterialValues.h"

namespace cue
{
namespace
{
/// @brief Editor の既存の COM Apartment を尊重し、この取得分だけ解放する
class ImageApartment final
{
  public:
    /// @brief 既存 Apartment がない場合に画像 Decoder 用に初期化する
    ImageApartment() noexcept : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }
    /// @brief 自身が取得した COM の初期化参照だけを解除する
    ~ImageApartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
    HRESULT result;
};

/// @brief OS の PNG Decoder に所有バイト列を渡し、上から順に詰めた RGBA8 を返す
Result<TexturePixels> read_png(IFileSystem &a_files, const TextureAsset &a_asset)
{
    constexpr std::uint32_t maxBytes = 64u * 1024u * 1024u;
    constexpr std::uint32_t maxDimension = 16384;
    auto bytes = a_files.read_all(a_asset.source, maxBytes);
    if (!bytes.has_value())
        return Result<TexturePixels>::failure(*bytes.try_error());
    if (bytes.try_value()->empty())
        return Result<TexturePixels>::failure(
            {ErrorCategory::InvalidArgument, "Texture.png.empty:" + a_asset.source.utf8()});
    const ImageApartment apartment;
    const auto failure = [&](const char *a_operation, HRESULT a_code)
    {
        return Result<TexturePixels>::failure(
            {ErrorCategory::PlatformFailure, std::string(a_operation) + ":" + a_asset.source.utf8(), a_code});
    };
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
        return failure("Texture.png.apartment", apartment.result);

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    auto result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result))
        return failure("Texture.png.factory", result);
    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (FAILED(result))
        return failure("Texture.png.stream", result);
    auto &input = *bytes.try_value();
    result = stream->InitializeFromMemory(reinterpret_cast<BYTE *>(input.data()), static_cast<DWORD>(input.size()));
    if (FAILED(result))
        return failure("Texture.png.stream_data", result);
    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(result))
        return failure("Texture.png.decoder", result);
    GUID format{};
    result = decoder->GetContainerFormat(&format);
    if (FAILED(result) || format != GUID_ContainerFormatPng)
        return failure("Texture.png.format", FAILED(result) ? result : WINCODEC_ERR_UNKNOWNIMAGEFORMAT);
    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result))
        return failure("Texture.png.frame", result);
    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    const auto pixelBytes = std::uint64_t{width} * height * 4;
    if (FAILED(result) || width == 0 || height == 0 || width > maxDimension || height > maxDimension ||
        pixelBytes > maxBytes)
        return failure("Texture.png.dimensions", FAILED(result) ? result : WINCODEC_ERR_IMAGESIZEOUTOFRANGE);
    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result))
        return failure("Texture.png.converter", result);
    result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                   WICBitmapPaletteTypeCustom);
    if (FAILED(result))
        return failure("Texture.png.convert", result);
    TexturePixels decoded{width, height, a_asset.colorSpace, {}};
    decoded.rgba.resize(static_cast<std::size_t>(pixelBytes));
    result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixelBytes),
                                   reinterpret_cast<BYTE *>(decoded.rgba.data()));
    if (FAILED(result))
        return failure("Texture.png.pixels", result);
    return Result<TexturePixels>::success(std::move(decoded));
}

/// @brief Catalog の型付き参照だけを使い、Material 間で PNG のデコードを共有する
Result<std::vector<SceneMaterialDesc>> decode_materials(IFileSystem &a_files, const AssetCatalog &a_catalog)
{
    return detail::collect_scene_materials(a_catalog.materials(),
                                           [&](const std::string &a_id)
                                           {
                                               auto texture = a_catalog.texture(a_id);
                                               if (!texture.has_value())
                                                   return Result<TexturePixels>::failure(*texture.try_error());
                                               return read_png(a_files, **texture.try_value());
                                           });
}
} // namespace

Result<std::vector<SceneMaterialDesc>> load_project_materials(IFileSystem &a_files, const Path &a_contentRoot)
{
    auto catalog = AssetCatalog::load(a_files, a_contentRoot);
    if (!catalog.has_value())
        return Result<std::vector<SceneMaterialDesc>>::failure(*catalog.try_error());
    return decode_materials(a_files, *catalog.try_value());
}

Result<void> set_project_materials(ISceneRenderer &a_renderer, IFileSystem &a_files, const AssetCatalog &a_catalog)
{
    auto materials = decode_materials(a_files, a_catalog);
    if (!materials.has_value())
        return Result<void>::failure(*materials.try_error());
    return a_renderer.set_materials(*materials.try_value());
}
} // namespace cue
