#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include <cmath>
#include <cstdio>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
static bool DrawColor(ID3D11Device *d, ID3D11DeviceContext *c, ID3D11RenderTargetView *rtv)
{
    wchar_t lib[MAX_PATH];
    GetSystemDirectoryW(lib, MAX_PATH);
    wcscat_s(lib, L"\\d3dcompiler_47.dll");
    HMODULE m = LoadLibraryW(lib);
    if (!m)
        return false;
    auto compile = (decltype(&D3DCompile))GetProcAddress(m, "D3DCompile");
    if (!compile)
        return false;
    const char *vs = "float4 main(uint id:SV_VertexID):SV_Position { float2 "
                     "p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}";
    const char *ps = "float4 main():SV_Target {return float4(0.25,0.5,0.75,1);}";
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    HRESULT hr = compile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vb, &err);
    if (err)
        err->Release();
    if (FAILED(hr))
        return false;
    err = nullptr;
    hr = compile(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &pb, &err);
    if (err)
        err->Release();
    if (FAILED(hr))
        return false;
    ID3D11VertexShader *v = nullptr;
    ID3D11PixelShader *p = nullptr;
    hr = d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &v);
    if (FAILED(hr))
        return false;
    hr = d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &p);
    if (FAILED(hr))
        return false;
    c->VSSetShader(v, nullptr, 0);
    c->PSSetShader(p, nullptr, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    FILE *f = nullptr;
    if (_wfopen_s(&f, (InspectorSessionDirectory() + L"\\capture.request").c_str(), L"wt") != 0)
        return false;
    fputs("1", f);
    fclose(f);
    Sleep(600);
    c->Draw(3, 0);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    v->Release();
    p->Release();
    vb->Release();
    pb->Release();
    FreeLibrary(m);
    return true;
}
static int VerifyDevice(ID3D11Device *d, ID3D11DeviceContext *c)
{
    printf("GetFeatureLevel=0x%X\n", (unsigned)d->GetFeatureLevel());
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 4;
    desc.Height = 4;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *t = nullptr, *read = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    HRESULT hr = d->CreateTexture2D(&desc, nullptr, &t);
    if (FAILED(hr))
        return 11;
    hr = d->CreateRenderTargetView(t, nullptr, &rtv);
    if (FAILED(hr))
        return 12;
    float color[4] = {0.f, 0.f, 0.f, 0.f};
    c->ClearRenderTargetView(rtv, color);
    if (!DrawColor(d, c, rtv))
        return 16;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = d->CreateTexture2D(&desc, nullptr, &read);
    if (FAILED(hr))
        return 13;
    c->CopyResource(read, t);
    D3D11_MAPPED_SUBRESOURCE data = {};
    hr = c->Map(read, 0, D3D11_MAP_READ, 0, &data);
    if (FAILED(hr))
        return 14;
    bool ok = true;
    const int expected[4] = {64, 128, 191, 255};
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++)
            for (unsigned i = 0; i < 4; i++)
            {
                int value = ((unsigned char *)data.pData)[y * data.RowPitch + x * 4 + i];
                if (abs(value - expected[i]) > 1)
                    ok = false;
            }
    c->Unmap(read, 0);
    read->Release();
    rtv->Release();
    t->Release();
    return ok ? 0 : 15;
}
int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2)
        return 1;
    HMODULE m = LoadLibraryW(argv[1]);
    if (!m)
    {
        printf("load failed %lu\n", GetLastError());
        return 2;
    }
    auto create = (decltype(&D3D11CreateDevice))GetProcAddress(m, "D3D11CreateDevice");
    auto swap = (decltype(&D3D11CreateDeviceAndSwapChain))GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    if (!create || !swap)
        return 3;
    for (int variant = 0; variant < 6; variant++)
    {
        ID3D11Device *d = nullptr;
        ID3D11DeviceContext *c = nullptr;
        D3D_FEATURE_LEVEL level;
        HRESULT hr;
        if (variant % 3 == 2)
            hr =
                swap(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, (variant < 3 ? 0 : D3D11_CREATE_DEVICE_SINGLETHREADED),
                     nullptr, 0, D3D11_SDK_VERSION, nullptr, nullptr, &d, &level, &c);
        else
            hr = create(nullptr, variant % 3 == 0 ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                        (variant < 3 ? 0 : D3D11_CREATE_DEVICE_SINGLETHREADED), nullptr, 0, D3D11_SDK_VERSION, &d,
                        &level, &c);
        printf("variant=%d create=0x%08lX level=0x%X\n", variant, (ULONG)hr, SUCCEEDED(hr) ? (unsigned)level : 0);
        if (FAILED(hr))
            return 4;
        int result = VerifyDevice(d, c);
        printf("texture readback result=%d\n", result);
        c->ClearState();
        c->Flush();
        c->Release();
        d->Release();
        if (result)
            return result;
    }

    printf("PASS device creation, forwarding and GPU texture readback\n");
    FreeLibrary(m);
    return 0;
}
