#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 1;
    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy)
        return 2;
    auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(proxy, "D3D11CreateDevice"));
    if (!create)
        return 3;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\d3dcompiler_47.dll");
    HMODULE compiler = LoadLibraryW(path);
    if (!compiler)
        return 4;
    auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile)
        return 5;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                      &context)))
        return 6;
    const char *source = "RWTexture2D<float4> target:register(u0); [numthreads(4,4,1)] void main(uint3 "
                         "id:SV_DispatchThreadID){target[id.xy]=float4(0.75,0.25,0.5,1);}";
    ComPtr<ID3DBlob> bytes, errors;
    if (FAILED(compile(source, strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &bytes, &errors)))
        return 7;
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(bytes->GetBufferPointer(), bytes->GetBufferSize(), nullptr, &shader)))
        return 8;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 4;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11UnorderedAccessView> view;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)))
        return 9;
    if (FAILED(device->CreateUnorderedAccessView(target.Get(), nullptr, &view)))
        return 10;
    const float zero[4] = {};
    context->ClearUnorderedAccessViewFloat(view.Get(), zero);
    auto rawView = view.Get();
    context->CSSetUnorderedAccessViews(0, 1, &rawView, nullptr);
    context->CSSetShader(shader.Get(), nullptr, 0);
    FILE *request = nullptr;
    if (_wfopen_s(&request, (InspectorSessionDirectory() + L"\\capture.request").c_str(), L"wt"))
        return 11;
    fputs("1", request);
    fclose(request);
    context->Dispatch(1, 1, 1);
    rawView = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &rawView, nullptr);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return 12;
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE data{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data)))
        return 13;
    bool okay = true;
    const int expected[4] = {191, 64, 128, 255};
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++)
            for (unsigned c = 0; c < 4; c++)
            {
                int actual = static_cast<unsigned char *>(data.pData)[y * data.RowPitch + x * 4 + c];
                if (abs(actual - expected[c]) > 1)
                    okay = false;
            }
    context->Unmap(staging.Get(), 0);
    context->ClearState();
    context->Flush();
    puts(okay ? "PASS compute Dispatch, UAV write and GPU readback" : "FAIL compute pixels");
    return okay ? 0 : 14;
}
