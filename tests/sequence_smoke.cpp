#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT h, const char *s)
{
    if (FAILED(h))
    {
        printf("FAIL %s %08lX\n", s, (unsigned long)h);
        throw std::runtime_error(s);
    }
}
static void Expect(bool ok, const char *s)
{
    if (!ok)
        throw std::runtime_error(s);
}
static void Name(ID3D11DeviceChild *o, const char *s)
{
    Check(o->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(s), s), "name");
}
int wmain(int argc, wchar_t **argv)
{
    if (argc != 4)
        return 1;
    try
    {
        auto driver = wcscmp(argv[2], L"warp") == 0 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        bool modern = wcscmp(argv[3], L"factory2") == 0;
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Inspector sequence smoke", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Expect(hwnd != nullptr, "window");
        HMODULE runtime = LoadLibraryW(argv[1]);
        Expect(runtime != nullptr, "runtime");
        auto create = (decltype(&D3D11CreateDevice))GetProcAddress(runtime, "D3D11CreateDevice");
        auto createSwap =
            (decltype(&D3D11CreateDeviceAndSwapChain))GetProcAddress(runtime, "D3D11CreateDeviceAndSwapChain");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<IDXGISwapChain> swap;
        if (!modern)
        {
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferDesc.Width = sd.BufferDesc.Height = 4;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 1;
            sd.OutputWindow = hwnd;
            sd.Windowed = TRUE;
            Check(createSwap(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr,
                             &ctx),
                  "create with swapchain");
        }
        else
        {
            Check(create(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx),
                  "create device");
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            ComPtr<IDXGISwapChain1> one;
            Check(device.As(&dxgi), "dxgi device");
            Check(dxgi->GetAdapter(&adapter), "adapter");
            Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "factory2");
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = sd.Height = 4;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            Check(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &one), "create for hwnd");
            Check(one.As(&swap), "base swap");
        }
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\d3dcompiler_47.dll");
        auto compile = (decltype(&D3DCompile))GetProcAddress(LoadLibraryW(path), "D3DCompile");
        Expect(compile != nullptr, "compiler");
        const char *source =
            "struct B{float4 v;float4 pad;};StructuredBuffer<B> bones:register(t0);ByteAddressBuffer raw:register(t1);"
            "Buffer<float4> tint:register(t2);RWStructuredBuffer<float4> result:register(u0);"
            "struct O{float4 p:SV_Position;float4 c:COLOR;};"
            "O vs(uint i:SV_VertexID){O "
            "o;o.p=float4(i==2?3:-1,i==1?3:-1,0,1);o.c=bones[0].v+asfloat(raw.Load4(0));return o;}"
            "float4 ps(O i):SV_Target{return i.c+tint[0];}"
            "float4 other(O i):SV_Target{return 0;}"
            "[numthreads(1,1,1)]void cs(){result[0]=bones[0].v+asfloat(raw.Load4(0))+tint[0];}";
        ComPtr<ID3DBlob> vb, pb, ob, cb, err;
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, &err),
              "compile VS");
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, &err),
              "compile PS");
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "other", "ps_5_0", 0, 0, &ob, &err),
              "compile other");
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "cs", "cs_5_0", 0, 0, &cb, &err),
              "compile CS");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps, other;
        ComPtr<ID3D11ComputeShader> cs;
        Check(device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "VS");
        Check(device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps), "PS");
        Check(device->CreatePixelShader(ob->GetBufferPointer(), ob->GetBufferSize(), nullptr, &other), "other PS");
        Check(device->CreateComputeShader(cb->GetBufferPointer(), cb->GetBufferSize(), nullptr, &cs), "CS");
        Name(vs.Get(), "Skin/VS");
        Name(ps.Get(), "SequenceMatch/PS");
        Name(other.Get(), "Other/PS");
        Name(cs.Get(), "SequenceMatch/CS");
        float bones[40] = {}, raw[12] = {}, typed[12] = {};
        for (int i = 0; i < 40; i++)
            bones[i] = (float)(100 + i);
        for (int i = 0; i < 12; i++)
            raw[i] = typed[i] = (float)(200 + i);
        for (int i = 0; i < 4; i++)
            raw[4 + i] = typed[4 + i] = 0.0625f;
        ComPtr<ID3D11Buffer> bb, rb, tb, out, read, largeBuffer;
        ComPtr<ID3D11ShaderResourceView> bv, rv, tv, largeView;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(bones);
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 32;
        D3D11_SUBRESOURCE_DATA data{bones, 0, 0};
        Check(device->CreateBuffer(&bd, &data, &bb), "bones");
        Name(bb.Get(), "Test/Bones");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 2;
        sv.Buffer.NumElements = 2;
        Check(device->CreateShaderResourceView(bb.Get(), &sv, &bv), "bone view");
        bd.ByteWidth = sizeof(raw);
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        bd.StructureByteStride = 0;
        data.pSysMem = raw;
        Check(device->CreateBuffer(&bd, &data, &rb), "raw");
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.BufferEx.FirstElement = 4;
        sv.BufferEx.NumElements = 4;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device->CreateShaderResourceView(rb.Get(), &sv, &rv), "raw view");
        bd.MiscFlags = 0;
        data.pSysMem = typed;
        Check(device->CreateBuffer(&bd, &data, &tb), "typed");
        sv = {};
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sv.Buffer.FirstElement = 1;
        sv.Buffer.NumElements = 1;
        Check(device->CreateShaderResourceView(tb.Get(), &sv, &tv), "typed view");
        UINT large[20000];
        for (UINT i = 0; i < 20000; ++i)
            large[i] = i ^ 0x1badcafe;
        bd.ByteWidth = sizeof(large);
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        data.pSysMem = large;
        Check(device->CreateBuffer(&bd, &data, &largeBuffer), "large buffer");
        sv = {};
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.BufferEx.FirstElement = 4;
        sv.BufferEx.NumElements = 19996;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device->CreateShaderResourceView(largeBuffer.Get(), &sv, &largeView), "large SRV");
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 16;
        Check(device->CreateBuffer(&bd, nullptr, &out), "CS result");
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateUnorderedAccessView(out.Get(), nullptr, &uav), "UAV");
        bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateBuffer(&bd, nullptr, &read), "read result");
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(swap->GetBuffer(0, IID_PPV_ARGS(&target)), "backbuffer");
        Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "RTV");
        D3D11_TEXTURE2D_DESC td;
        target->GetDesc(&td);
        td.BindFlags = td.MiscFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&td, nullptr, &staging), "pixel readback");
        FILE *request = nullptr;
        Expect(_wfopen_s(&request, (InspectorSessionDirectory() + L"\\capture.request").c_str(), L"wt") == 0,
               "request");
        fputs("100\nbuffers=1\nbuffer_limit=131072\nframes=3\nonly_matching=1\nfilter=SequenceMatch\n", request);
        fclose(request);
        Sleep(600);
        swap->Present(0, DXGI_PRESENT_TEST); // Must not consume a present interval.
        for (int frame = 0; frame < 3; frame++)
        {
            bones[16] = (frame + 1) / 16.0f;
            bones[17] = 0.125f;
            bones[18] = 0.25f;
            bones[19] = 0.5f;
            ctx->UpdateSubresource(bb.Get(), 0, nullptr, bones, 0, 0);
            auto targetView = rtv.Get();
            ctx->OMSetRenderTargets(1, &targetView, nullptr);
            D3D11_VIEWPORT vp{0, 0, 4, 4, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ID3D11ShaderResourceView *vsViews[] = {bv.Get(), rv.Get()};
            ctx->VSSetShaderResources(0, 2, vsViews);
            auto largeSrv = largeView.Get();
            ctx->VSSetShaderResources(3, 1, &largeSrv);
            auto typedView = tv.Get();
            ctx->PSSetShaderResources(2, 1, &typedView);
            ctx->PSSetShader(other.Get(), nullptr, 0);
            ctx->Draw(3, 0); // Excluded by only_matching.
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(3, 0);
            ctx->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            Check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "map pixels");
            float expected[4] = {bones[16] + 0.125f, 0.25f, 0.375f, 0.625f};
            for (UINT y = 0; y < 4; y++)
                for (UINT x = 0; x < 4; x++)
                    for (int i = 0; i < 4; i++)
                        Expect(abs((int)((unsigned char *)m.pData)[y * m.RowPitch + x * 4 + i] -
                                   (int)(expected[i] * 255 + 0.5f)) <= 1,
                               "GPU SRV pixel mismatch");
            ctx->Unmap(staging.Get(), 0);
            ID3D11ShaderResourceView *csViews[] = {bv.Get(), rv.Get(), tv.Get()};
            ctx->CSSetShaderResources(0, 3, csViews);
            ctx->CSSetShader(cs.Get(), nullptr, 0);
            auto output = uav.Get();
            ctx->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            ctx->Dispatch(1, 1, 1);
            ctx->CopyResource(read.Get(), out.Get());
            Check(ctx->Map(read.Get(), 0, D3D11_MAP_READ, 0, &m), "map CS");
            Expect(memcmp(m.pData, expected, sizeof(expected)) == 0, "GPU CS SRV mismatch");
            ctx->Unmap(read.Get(), 0);
            if (modern)
            {
                ComPtr<IDXGISwapChain1> one;
                Check(swap.As(&one), "swap1");
                DXGI_PRESENT_PARAMETERS pp{};
                Check(one->Present1(0, 0, &pp), "present1");
            }
            else
                Check(swap->Present(0, 0), "present");
        }
        ctx->ClearState();
        ctx->Flush();
        DestroyWindow(hwnd);
        puts("PASS structured/raw/typed SRVs reached VS/PS/CS and 3 present intervals completed");
        return 0;
    }
    catch (const std::exception &e)
    {
        printf("FAIL %s\n", e.what());
        return 2;
    }
}
