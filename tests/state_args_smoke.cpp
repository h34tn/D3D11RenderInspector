#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "config.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>

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
struct Params
{
    UINT firstVertex, firstInstance;
    float depth;
    UINT instanced;
    float color[4];
};
static std::vector<unsigned> ReadPixels(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "create readback");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map readback");
    std::vector<unsigned> data(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(data.data() + y * desc.Width, static_cast<char *>(mapped.pData) + y * mapped.RowPitch,
               desc.Width * sizeof(unsigned));
    context->Unmap(staging.Get(), 0);
    return data;
}
static void VerifyColor(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture,
                        bool split = false, bool black = false, bool compute = false)
{
    context->OMSetRenderTargets(0, nullptr, nullptr);
    auto pixels = ReadPixels(device, context, texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    for (UINT y = 0; y < desc.Height; ++y)
        for (UINT x = 0; x < desc.Width; ++x)
        {
            const int expected[4] = {black             ? 0
                                     : compute         ? int(std::lround((x + 1) * 255.0 / 4))
                                     : split && x >= 2 ? 191
                                                       : 64,
                                     black     ? 0
                                     : compute ? int(std::lround((y + 1) * 255.0 / 2))
                                               : 128,
                                     black     ? 0
                                     : compute ? 128
                                               : 191,
                                     black ? 0 : 255};
            auto bytes = reinterpret_cast<const unsigned char *>(&pixels[y * desc.Width + x]);
            for (unsigned component = 0; component < 4; ++component)
                if (abs(int(bytes[component]) - expected[component]) > 1)
                {
                    printf("pixel mismatch x=%u y=%u channel=%u actual=%u expected=%d\n", x, y, component,
                           unsigned(bytes[component]), expected[component]);
                    throw std::runtime_error("pixel mismatch");
                }
        }
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
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(create(nullptr, wcscmp(argv[2], L"warp") == 0 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                     0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
              "create device");
        wchar_t compilerPath[MAX_PATH];
        GetSystemDirectoryW(compilerPath, MAX_PATH);
        wcscat_s(compilerPath, L"\\d3dcompiler_47.dll");
        HMODULE compiler = LoadLibraryW(compilerPath);
        Expect(compiler != nullptr, "load compiler");
        auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
        Expect(compile != nullptr, "resolve compiler");
        const char *graphics =
            "cbuffer Params:register(b0){uint firstVertex;uint firstInstance;float depth;uint instanced;float4 color;}"
            "struct V{float4 pos:SV_Position;float2 clip:SV_ClipDistance0;nointerpolation float4 col:COLOR0;};"
            "V vs(float2 xy:POSITION,float tag:TEXCOORD0,float instance:TEXCOORD1){V o;"
            "uint i=instanced?(uint)instance:0;float low=-1,high=1;"
            "if(instanced){low=-1+(float)i;high=low+1;xy.x=xy.x*0.5+low+0.5;}"
            "o.pos=float4(xy,depth,1);o.clip=float2(xy.x-low,high-xy.x);"
            "if(tag!=(float)firstVertex || i>=2)o.clip=-1;"
            "o.col=color;if(instanced)o.col.r=0.25+0.5*i;return o;}"
            "float4 ps(V i):SV_Target{return i.col;}";
        ComPtr<ID3DBlob> vsBytes, psBytes, errors;
        Check(compile(graphics, strlen(graphics), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsBytes, &errors),
              "compile VS");
        errors.Reset();
        Check(compile(graphics, strlen(graphics), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psBytes, &errors),
              "compile PS");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        Check(device->CreateVertexShader(vsBytes->GetBufferPointer(), vsBytes->GetBufferSize(), nullptr, &vs),
              "create VS");
        Check(device->CreatePixelShader(psBytes->GetBufferPointer(), psBytes->GetBufferSize(), nullptr, &ps),
              "create PS");
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
        context->RSSetViewports(1, &viewport);
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(Params);
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> constants;
        Check(device->CreateBuffer(&bufferDesc, nullptr, &constants), "create constants");
        auto rawConstants = constants.Get();
        context->VSSetConstantBuffers(0, 1, &rawConstants);
        // Real IA inputs make base/start offsets observable without assuming that
        // system-generated shader IDs include those offsets on every driver.
        struct Vertex
        {
            float x, y, tag;
        };
        const Vertex vertices[] = {{5, 5, 0},  {5, 5, 1},   {-1, -1, 2}, {-1, 3, 2},
                                   {3, -1, 2}, {-1, -1, 5}, {-1, 3, 5},  {3, -1, 5}};
        float instances[13];
        for (float &value : instances)
            value = 2;
        instances[0] = instances[7] = instances[11] = 0;
        instances[8] = instances[12] = 1;
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        ComPtr<ID3D11InputLayout> layout;
        Check(device->CreateInputLayout(elements, 3, vsBytes->GetBufferPointer(), vsBytes->GetBufferSize(), &layout),
              "create input layout");
        context->IASetInputLayout(layout.Get());
        ComPtr<ID3D11Buffer> vertexBuffer, instanceBuffer;
        D3D11_SUBRESOURCE_DATA vertexData{vertices, 0, 0};
        bufferDesc.ByteWidth = sizeof(vertices);
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        Check(device->CreateBuffer(&bufferDesc, &vertexData, &vertexBuffer), "create vertices");
        vertexData.pSysMem = instances;
        bufferDesc.ByteWidth = sizeof(instances);
        Check(device->CreateBuffer(&bufferDesc, &vertexData, &instanceBuffer), "create instances");
        ID3D11Buffer *streams[] = {vertexBuffer.Get(), instanceBuffer.Get()};
        const UINT strides[] = {sizeof(Vertex), sizeof(float)}, offsets[] = {0, 0};
        context->IASetVertexBuffers(0, 2, streams, strides, offsets);
        const UINT indices[] = {2, 4, 5, 6};
        D3D11_SUBRESOURCE_DATA initial{indices, 0, 0};
        bufferDesc.ByteWidth = sizeof(indices);
        bufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ComPtr<ID3D11Buffer> indexBuffer;
        Check(device->CreateBuffer(&bufferDesc, &initial, &indexBuffer), "create indices");
        context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        UINT indirectData[16] = {};
        indirectData[4] = 3;
        indirectData[5] = 2;
        indirectData[6] = 5;
        indirectData[7] = 7;
        indirectData[8] = 3;
        indirectData[9] = 2;
        indirectData[10] = 1;
        indirectData[11] = UINT(-2);
        indirectData[12] = 11;
        initial.pSysMem = indirectData;
        bufferDesc.ByteWidth = sizeof(indirectData);
        bufferDesc.BindFlags = 0;
        bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        ComPtr<ID3D11Buffer> indirect;
        Check(device->CreateBuffer(&bufferDesc, &initial, &indirect), "create indirect draw arguments");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 4;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> color, depth;
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateTexture2D(&desc, nullptr, &color), "create color");
        Check(device->CreateRenderTargetView(color.Get(), nullptr, &rtv), "create RTV");
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Check(device->CreateTexture2D(&desc, nullptr, &depth), "create depth");
        ComPtr<ID3D11DepthStencilView> depthViews[4];
        for (UINT flags = 0; flags < 4; ++flags)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            view.Flags = flags;
            Check(device->CreateDepthStencilView(depth.Get(), &view, &depthViews[flags]), "create DSV");
        }
        D3D11_DEPTH_STENCIL_DESC stateDesc{};
        stateDesc.DepthEnable = TRUE;
        stateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        stateDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        stateDesc.StencilEnable = TRUE;
        stateDesc.StencilReadMask = 0xff;
        stateDesc.StencilWriteMask = 0;
        stateDesc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                               D3D11_COMPARISON_EQUAL};
        stateDesc.BackFace = stateDesc.FrontFace;
        ComPtr<ID3D11DepthStencilState> readOnlyState, disabledState;
        Check(device->CreateDepthStencilState(&stateDesc, &readOnlyState), "create read-only state");
        stateDesc.DepthEnable = FALSE;
        stateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        stateDesc.DepthFunc = D3D11_COMPARISON_LESS;
        stateDesc.StencilEnable = FALSE;
        Check(device->CreateDepthStencilState(&stateDesc, &disabledState), "create disabled state");
        FILE *request = nullptr;
        Expect(_wfopen_s(&request, (InspectorSessionDirectory() + L"\\capture.request").c_str(), L"wt") == 0,
               "create capture request");
        fputs("14\nconstants=1\nbuffers=1\n", request);
        fclose(request);
        Sleep(600);
        auto prepare = [&](UINT first, UINT instance, bool instanced, ID3D11DepthStencilView *dsv,
                           ID3D11DepthStencilState *state, UINT ref, float z = 0.25f, bool clear = true,
                           bool red = false) {
            if (clear)
            {
                const float black[4] = {};
                context->ClearRenderTargetView(rtv.Get(), black);
                context->ClearDepthStencilView(depthViews[0].Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.625f,
                                               0x5a);
            }
            Params data{
                first, instance, z, UINT(instanced), {red ? 1.0f : 0.25f, red ? 0.0f : 0.5f, red ? 0.0f : 0.75f, 1.0f}};
            context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
            context->OMSetDepthStencilState(state, ref);
            auto view = rtv.Get();
            context->OMSetRenderTargets(1, &view, dsv);
        };
        puts("case DrawIndexed");
        prepare(2, 0, false, nullptr, nullptr, 0);
        context->DrawIndexed(3, 1, -2);
        VerifyColor(device.Get(), context.Get(), color.Get());
        puts("case DrawInstanced");
        prepare(5, 7, true, nullptr, nullptr, 0);
        context->DrawInstanced(3, 2, 5, 7);
        VerifyColor(device.Get(), context.Get(), color.Get(), true);
        puts("case DrawIndexedInstanced");
        prepare(2, 11, true, nullptr, nullptr, 0);
        context->DrawIndexedInstanced(3, 2, 1, -2, 11);
        VerifyColor(device.Get(), context.Get(), color.Get(), true);
        prepare(5, 7, true, nullptr, nullptr, 0);
        context->DrawInstancedIndirect(indirect.Get(), 16);
        VerifyColor(device.Get(), context.Get(), color.Get(), true);
        prepare(2, 11, true, nullptr, nullptr, 0);
        context->DrawIndexedInstancedIndirect(indirect.Get(), 32);
        VerifyColor(device.Get(), context.Get(), color.Get(), true);
        prepare(5, 0, false, depthViews[0].Get(), nullptr, 9);
        context->Draw(3, 5);
        VerifyColor(device.Get(), context.Get(), color.Get());
        prepare(5, 0, false, depthViews[0].Get(), nullptr, 9, 0.5f, false, true);
        context->Draw(3, 5); // Must be rejected by the default LESS test after the near draw wrote depth.
        VerifyColor(device.Get(), context.Get(), color.Get());
        for (UINT flags = 1; flags < 4; ++flags)
        {
            prepare(5, 0, false, depthViews[flags].Get(), readOnlyState.Get(), 0x5a);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            auto before = ReadPixels(device.Get(), context.Get(), depth.Get());
            auto view = rtv.Get();
            context->OMSetRenderTargets(1, &view, depthViews[flags].Get());
            context->Draw(3, 5);
            VerifyColor(device.Get(), context.Get(), color.Get());
            Expect(before == ReadPixels(device.Get(), context.Get(), depth.Get()), "read-only depth/stencil changed");
        }
        prepare(5, 0, false, nullptr, nullptr, 0);
        context->Draw(3, 5);
        VerifyColor(device.Get(), context.Get(), color.Get());
        prepare(5, 0, false, depthViews[0].Get(), disabledState.Get(), 0, 0.75f);
        context->Draw(3, 5); // Behind the clear depth, but depth testing is disabled.
        VerifyColor(device.Get(), context.Get(), color.Get());
        prepare(5, 0, false, depthViews[3].Get(), readOnlyState.Get(), 0x5b);
        context->Draw(3, 5); // Stencil EQUAL must reject ref 0x5b against clear value 0x5a.
        VerifyColor(device.Get(), context.Get(), color.Get(), false, true);
        const char *compute =
            "RWTexture2D<float4> target:register(u0);[numthreads(1,1,1)]"
            "void cs(uint3 id:SV_DispatchThreadID){target[id.xy]=float4((id.x+1)/4.0,(id.y+1)/2.0,0.5,1);}";
        ComPtr<ID3DBlob> csBytes;
        errors.Reset();
        Check(compile(compute, strlen(compute), nullptr, nullptr, nullptr, "cs", "cs_5_0", 0, 0, &csBytes, &errors),
              "compile CS");
        ComPtr<ID3D11ComputeShader> cs;
        Check(device->CreateComputeShader(csBytes->GetBufferPointer(), csBytes->GetBufferSize(), nullptr, &cs),
              "create CS");
        context->CSSetShader(cs.Get(), nullptr, 0);
        desc.Width = 4;
        desc.Height = 2;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateTexture2D(&desc, nullptr, &target), "create compute target");
        Check(device->CreateUnorderedAccessView(target.Get(), nullptr, &uav), "create UAV");
        auto rawUav = uav.Get();
        const float black[4] = {};
        context->ClearUnorderedAccessViewFloat(rawUav, black);
        context->CSSetUnorderedAccessViews(0, 1, &rawUav, nullptr);
        const UINT dispatchArgs[8] = {0, 0, 0, 4, 2, 1, 0, 0};
        initial.pSysMem = dispatchArgs;
        bufferDesc.ByteWidth = sizeof(dispatchArgs);
        ComPtr<ID3D11Buffer> dispatchBuffer;
        Check(device->CreateBuffer(&bufferDesc, &initial, &dispatchBuffer), "create indirect dispatch arguments");
        context->DispatchIndirect(dispatchBuffer.Get(), 12);
        rawUav = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &rawUav, nullptr);
        VerifyColor(device.Get(), context.Get(), target.Get(), false, false, true);
        context->ClearState();
        context->Flush();
        puts("PASS 14 captured calls, split instances, offsets, negative base vertex, depth/stencil and indirect UAV "
             "readback");
        return 0;
    }
    catch (const std::exception &error)
    {
        printf("FAIL %s\n", error.what());
        return 2;
    }
}
