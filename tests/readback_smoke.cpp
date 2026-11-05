#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "config.h"
#include <cstdio>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <wrl/client.h>

// Exercises the optional readback: VS constants bound at a D3D11.1 offset, PS constants, VS/PS textures,
// per-vertex and per-instance buffers. Only the draw whose debug name matches the request filter is read back.
// Data patterns are mirrored by tests/readback_checks.py.
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr, const char *stage)
{
    if (FAILED(hr))
    {
        printf("FAIL %s HRESULT=%08lX\n", stage, (unsigned long)hr);
        throw std::runtime_error(stage);
    }
}
static void Expect(bool condition, const char *stage)
{
    if (!condition)
        throw std::runtime_error(stage);
}
static void Name(ID3D11DeviceChild *object, const char *name)
{
    Check(object->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name), "set debug name");
}
static ComPtr<ID3D11Texture2D> Texture(ID3D11Device *device, UINT size, DXGI_FORMAT format, const void *data,
                                       UINT pitch, ComPtr<ID3D11ShaderResourceView> &view)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&desc, &initial, &texture), "create texture");
    Check(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "create SRV");
    return texture;
}
int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 || (wcscmp(argv[2], L"warp") && wcscmp(argv[2], L"hardware")))
        return 1;
    try
    {
        HMODULE runtime = LoadLibraryW(argv[1]);
        Expect(runtime != nullptr, "load D3D11");
        auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, "D3D11CreateDevice"));
        Expect(create != nullptr, "resolve D3D11CreateDevice");
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(create(nullptr, wcscmp(argv[2], L"warp") == 0 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                     0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context),
              "create device");
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        Check(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)), "query options");
        Expect(options.ConstantBufferOffsetting, "constant buffer offsetting unsupported");
        ComPtr<ID3D11DeviceContext1> context1;
        Check(context.As(&context1), "query Context1");

        wchar_t compilerPath[MAX_PATH];
        GetSystemDirectoryW(compilerPath, MAX_PATH);
        wcscat_s(compilerPath, L"\\d3dcompiler_47.dll");
        HMODULE compiler = LoadLibraryW(compilerPath);
        Expect(compiler != nullptr, "load compiler");
        auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
        Expect(compile != nullptr, "resolve compiler");
        const char *source =
            "cbuffer V:register(b0){float4 values[16];};"
            "Texture2D<float> heights:register(t0);"
            "struct O{float4 pos:SV_Position;float4 col:COLOR0;};"
            "O vs(float2 p:POSITION,float4 inst:INSTANCE){O o;o.pos=float4(p,0,1);"
            "o.col=values[0]+0*inst+0*heights.Load(int3(0,0,0));return o;}"
            "cbuffer P:register(b0){float4 tint;};"
            "Texture2D colors:register(t0);Texture2D blocks:register(t1);"
            "float4 ps(O i):SV_Target{return i.col+0*tint+0*colors.Load(int3(0,0,0))+0*blocks.Load(int3(0,0,0));}"
            "float4 other(O i):SV_Target{return i.col;}";
        ComPtr<ID3DBlob> vsBytes, psBytes, otherBytes, errors;
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsBytes, &errors),
              "compile VS");
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psBytes, &errors),
              "compile PS");
        Check(compile(source, strlen(source), nullptr, nullptr, nullptr, "other", "ps_5_0", 0, 0, &otherBytes,
                      &errors),
              "compile second PS");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps, other;
        Check(device->CreateVertexShader(vsBytes->GetBufferPointer(), vsBytes->GetBufferSize(), nullptr, &vs),
              "create VS");
        Check(device->CreatePixelShader(psBytes->GetBufferPointer(), psBytes->GetBufferSize(), nullptr, &ps),
              "create PS");
        Check(device->CreatePixelShader(otherBytes->GetBufferPointer(), otherBytes->GetBufferSize(), nullptr, &other),
              "create second PS");
        Name(vs.Get(), "Shared/VS");
        Name(ps.Get(), "Readback/Match");
        Name(other.Get(), "Other/NoMatch");

        // 32 VS constants; the shader sees constants 16..31 through the offset binding, values[0] is the color.
        float vsConstants[32][4];
        for (int k = 0; k < 32; k++)
        {
            vsConstants[k][0] = (float)k;
            vsConstants[k][1] = k + 0.5f;
            vsConstants[k][2] = (float)-k;
            vsConstants[k][3] = 7.0f;
        }
        const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        memcpy(vsConstants[16], color, sizeof(color));
        const float psConstants[4] = {0.125f, 0.375f, 0.625f, 0.875f};
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.ByteWidth = sizeof(vsConstants);
        D3D11_SUBRESOURCE_DATA initial{vsConstants, 0, 0};
        ComPtr<ID3D11Buffer> vsBuffer, psBuffer, vertexBuffer, instanceBuffer;
        Check(device->CreateBuffer(&bufferDesc, &initial, &vsBuffer), "create VS constants");
        bufferDesc.ByteWidth = sizeof(psConstants);
        initial.pSysMem = psConstants;
        Check(device->CreateBuffer(&bufferDesc, &initial, &psBuffer), "create PS constants");
        const float vertices[6] = {-1, -1, -1, 3, 3, -1};
        const float instances[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bufferDesc.ByteWidth = sizeof(vertices);
        initial.pSysMem = vertices;
        Check(device->CreateBuffer(&bufferDesc, &initial, &vertexBuffer), "create vertices");
        bufferDesc.ByteWidth = sizeof(instances);
        initial.pSysMem = instances;
        Check(device->CreateBuffer(&bufferDesc, &initial, &instanceBuffer), "create instances");

        unsigned char rgba[64], bc1[32];
        for (int j = 0; j < 64; j++)
            rgba[j] = (unsigned char)((j * 7 + 3) & 255);
        for (int j = 0; j < 32; j++)
            bc1[j] = (unsigned char)((j * 13 + 5) & 255);
        const float heights[4] = {1.5f, -2.25f, 3.0f, 1000.0f};
        ComPtr<ID3D11ShaderResourceView> rgbaView, bc1View, heightView;
        auto rgbaTexture = Texture(device.Get(), 4, DXGI_FORMAT_R8G8B8A8_UNORM, rgba, 16, rgbaView);
        auto bc1Texture = Texture(device.Get(), 8, DXGI_FORMAT_BC1_UNORM, bc1, 16, bc1View);
        auto heightTexture = Texture(device.Get(), 2, DXGI_FORMAT_R32_FLOAT, heights, 8, heightView);

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        ComPtr<ID3D11InputLayout> layout;
        Check(device->CreateInputLayout(elements, 2, vsBytes->GetBufferPointer(), vsBytes->GetBufferSize(), &layout),
              "create layout");
        D3D11_TEXTURE2D_DESC targetDesc{};
        targetDesc.Width = targetDesc.Height = 4;
        targetDesc.MipLevels = targetDesc.ArraySize = targetDesc.SampleDesc.Count = 1;
        targetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateTexture2D(&targetDesc, nullptr, &target), "create target");
        Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "create RTV");

        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer *streams[] = {vertexBuffer.Get(), instanceBuffer.Get()};
        const UINT strides[] = {8, 16}, offsets[] = {0, 0};
        context->IASetVertexBuffers(0, 2, streams, strides, offsets);
        context->VSSetShader(vs.Get(), nullptr, 0);
        const UINT first = 16, count = 16;
        auto rawVs = vsBuffer.Get();
        context1->VSSetConstantBuffers1(0, 1, &rawVs, &first, &count);
        auto rawHeight = heightView.Get();
        context->VSSetShaderResources(0, 1, &rawHeight);
        auto rawPs = psBuffer.Get();
        context->PSSetConstantBuffers(0, 1, &rawPs);
        ID3D11ShaderResourceView *psViews[] = {rgbaView.Get(), bc1View.Get()};
        context->PSSetShaderResources(0, 2, psViews);
        D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
        context->RSSetViewports(1, &viewport);
        auto rawRtv = rtv.Get();
        context->OMSetRenderTargets(1, &rawRtv, nullptr);
        const float black[4] = {};
        context->ClearRenderTargetView(rtv.Get(), black);

        FILE *request = nullptr;
        Expect(_wfopen_s(&request, (InspectorSessionDirectory() + L"\\capture.request").c_str(), L"wt") == 0,
               "create capture request");
        fputs("2\nconstants=1\ntextures=1\nfilter=Readback\n", request);
        fclose(request);
        Sleep(600);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->DrawInstanced(3, 2, 0, 0);
        // Read back the first draw's color before the unmatched draw renders over a cleared target.
        D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
        stagingDesc.BindFlags = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "create staging");
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map target");
        const int expected[4] = {64, 128, 191, 255};
        for (UINT y = 0; y < 4; y++)
            for (UINT x = 0; x < 4; x++)
                for (int i = 0; i < 4; i++)
                {
                    int value = ((unsigned char *)mapped.pData)[y * mapped.RowPitch + x * 4 + i];
                    Expect(abs(value - expected[i]) <= 1, "offset-bound constant did not reach the VS");
                }
        context->Unmap(staging.Get(), 0);
        context->ClearRenderTargetView(rtv.Get(), black);
        context->PSSetShader(other.Get(), nullptr, 0);
        context->DrawInstanced(3, 2, 0, 0);
        context->ClearState();
        context->Flush();
        (void)rgbaTexture;
        (void)bc1Texture;
        (void)heightTexture;
        puts("PASS readback draw rendered offset-bound VS constants; unmatched draw issued");
        return 0;
    }
    catch (const std::exception &error)
    {
        printf("FAIL %s\n", error.what());
        return 2;
    }
}
