#include <iostream>
#define NOMINMAX
#include <Unknwn.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>

namespace winrt {
    using namespace ::winrt::Windows::Graphics::Capture;
    using namespace ::winrt::Windows::Graphics::DirectX;
    using namespace ::winrt::Windows::Graphics::DirectX::Direct3D11;
}
namespace Windows {
    using namespace ::Windows::Graphics::DirectX::Direct3D11;
}

struct D3DContext {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::IDirect3DDevice winrt_device{nullptr};
};

D3DContext CreateD3DContext() {
    D3DContext result;

    D3D_FEATURE_LEVEL featureLevel{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        result.device.put(),
        &featureLevel,
        result.context.put());

    if (FAILED(hr))
        throw winrt::hresult_error(hr);

    // 将 DXGI Device 转换为 WinRT Direct3D Device
    auto dxgiDevice = result.device.as<IDXGIDevice>();

    winrt::com_ptr<IInspectable> inspectable;

    hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(),
        inspectable.put()
  
    if (FAILED(hr))
        throw winrt::hresult_error(hr);

    result.winrt_device =
        inspectable.as<winrt::IDirect3DDevice>();
  

    rn result;
}



t::GraphicsCaptureItem CreateCaptureItemFromHWND(HWND hwnd)
{ a
     interop =
		
        t::get_activation_factory<
		
            t::GraphicsCaptureItem,
		
            phicsCaptureItemInterop>();



    t::GraphicsCaptureItem item{ nulptr };


    ULT hr = interop->CreateForWindow(
		
        ,
		
        t::guid_of<winrt::GraphicsCaptureItem>(),
		
        t::put_abi(item)
	)


    FAILED(hr))
		
        w winrt::hresult_error(hr);



    rn item;
}

bool SaveStagingTextureAsBmp(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* stagingTexture,
    const wchar_t* filename);

int main() {
	w
    t::init_apartment();



    !winrt::GraphicsCaptureSession::IsSupported()) {
		
        :wcerr << L"不支持" << std::endl;
		ret
        1;
	}

    W

    nd = FindWindowW(L"Notepad", nullptr);

	if

    nd) {
		std
        err << L"未找到目标窗口\n";
		return 1;
        

	winrt
    r

    sCaptureItem item = CreateCaptureItemFromHWND(hwnd);
    auto context = CreateD3DContext();

    auto size = item.Size();
    auto frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        context.winrt_device,
        winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size
    );

   session = frame_pool.CreateCaptureSession(item);
    
    se

    apture();

    winrt::Direct3D11CaptureFrame frame = nullptr;
    while (!(frame = frame_pool.TryGetNextFrame()));
    if (frame) {
        auto contentSize = frame.ContentSize();
        auto surface = frame.Surface();

        auto access =
            frame.Surface().as<Windows::IDirect3DDxgiInterfaceAccess>();

        winrt::com_ptr<ID3D11Texture2D> sourceTexture;

        HRESULT hr = access->GetInterface(
            __uuidof(ID3D11Texture2D),
            sourceTexture.put_void()
        );
if (FAILED(hr))
            throw winrt::hresult_error(hr);

        D3D11_TEXTURE2D_DESC desc{};
        sourceTexture->GetDesc(&desc);

        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        winrt::com_ptr<ID3D11Texture2D> stagingTexture;

        hr = context.device->CreateTexture2D(
            &desc,
            nullptr,
            stagingTexture.put()
        );
if (FAILED(hr))
            throw winrt::hresult_error(hr);

        context.context->CopyResource(
            stagingTexture.get(),
            sourceTexture.get()
        );
D3D11_MAPPED_SUBRESOURCE mapped{};

        hr = context.context->Map(
            stagingTexture.get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped
        );
if (FAILED(hr))
            throw winrt::hresult_error(hr);

        // mapped.pData：像素数据的起始地址
        // mapped.RowPitch：每一行实际占用的字节数
        // 使用完毕后必须 Unmap
        SaveStagingTextureAsBmp(context.context.get(), stagingTexture.get(), L"test.bmp");

        context.context->Unmap(stagingTexture.get(), 0);
    }

	// winrt::uninit_apartment();
	return

    }
#include <cstdint>
#inclu
    <fstream>
#in#include <cstdint>
#include <fstream>
#include <limits>
#include <vector>1DeviceContext* context,
    ID3D11Texture2D* stagingTexture,
    const wchar_t* filename)
{
    if (!context || !stagingTexture || filename)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    stagingTexture->GetDesc(&desc);

    // 这里只处理单张、非多重采样的 2D 纹理。
    if (desc.Width == 0 ||
        desc.Height == 0 ||
        desc.SampleDesc.Count != 1 ||
        desc.ArraySize != 1)
    {
        return false;
    }

    // 24 位 BMP 写入时 BGR。
    const bool isBGRA =
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    const bool isRGBA =
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    if (!isBGRA && !isRGBA)
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};

    HRESULT hr = context->Map(
        stagingTexture,
        0,
        D3D11_MAP_READ,
        0,
        &mapped);

    if (FAILED(hr))
        return false;

    bool success = false;

    do {
        const uint64_t width = desc.Width;
        const uint64_t height = desc.Height;

        // BMP 每行必须按 4 字节对齐。
        const uint64_t bmpRowPitch = (width * 3 + 3) & ~uint64_t(3);
        const uint64_t imageSize = bmpRowPitch * height;
        const uint64_t fileSize =
            14 + 40 + imageSize;

        if (imageSize > std::numeric_limits<DWORD>::max() ||
            fileSize > std::numeric_limits<DWORD>::max())
        {
            break;
        }

        std::vector<uint8_t> pixel       static_cast<size_t>(imageSize), 0);

        const auto* srcBase =
            static_cast<const uint8_t*>(mapped.pData);

        // BMP 使用正高度时，像素按 bottom-up 存储。
        for (uint32_t y = 0; y < desc.Height; ++y)
        {
            const uint8_t* src =
                srcBase + static_cast<size_t>( d.RowPitch;

            uint8_t* dst =
                pixels.data() +
                static_cast<size_t>(desc.Height - 1 - y) *
                static_cast<size_t>(bmpRowPitch);

            for (uint32_t x = 0; x < d
                               {
                const uint8_t* p = src + x * 4;
                uint8_t* q  

                if (isBGRA) {
                    q[0] = p[0]; // B
                    q[1] = p[1]; // G
                    q[2] = p[2]; // R
                 }
                else {
                     q[0] = p[2]; // B
                     q[1] = p              q[2] = p[0]; // R
                 }
            }
        }

         BITMAPFILEHEADER fileHeader{};          fileHeader.bfType = 0x4D42; // "BM"
        fileHeader.bfOffBits =
            sizeof(BITMAPFILEHEADER) + sizeof(BITMA  INFOHEADER);
        fileHeader.bfSize = static_cast<DWORD>(fileSize);

        BITMAPINFOHEADER infoHeader{};
        infoHeader.biSize = sizeof(BITMAPINFOHEADER);
        infoHeader.biWidth = static_cast<LONG>(desc.Width);
        infoHeader.biHeight = static_cast<LONG>(desc.Height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 24;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = static_cast<DWORD>(imageSize);

        std::ofstream file(filename, std::ios::binary);
        if (!file)
            break;

        // 注意：Windows BMP 文件头是固定格式。
        // 不直接写 BITMAPFILEHEADER，避免结构体填充导致格式不正确。
        const uint16_t bfType = 0x4D42;
        const uint32_t bfSize = static_cast<uint32_t>(fileSize);
        const uint16_t reserved = 0;
        const uint32_t bfOffBits = 14 + 40;

        file.write(reinterpret_cast<const char*>(&bfType), sizeof(bfType));
        file.write(reinterpret_cast<const char*>(&bfSize), sizeof(bfSize));
        file.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
        file.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
        file.write(reinterpret_cast<const char*>(&bfOffBits), sizeof(bfOffBits));

        file.write(
            reinterpret_cast<const char*>(&infoHeader),
            sizeof(infoHeader));

        file.write(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));

        success = file.good();
    } while (false);

    return success;
}
