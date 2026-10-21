#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d11_4.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

extern "C" void ObserverInfo(const char *);
namespace
{
struct Table
{
    void **original;
    std::unique_ptr<void *[]> shadow;
};
std::mutex tableMutex, recordMutex;
auto &tables = *new std::unordered_map<void *, Table *>();
// Retain shadow vtables until process teardown; never invalidate a live COM object on unload.
auto &lifetime = *new std::vector<std::unique_ptr<Table>>();
std::unordered_map<void *, std::string> shaders;
std::once_flag outputOnce;
std::wstring outputPath;
std::wstring requestPath;
std::atomic<ULONGLONG> nextPoll{0};
std::atomic<unsigned> remaining{0};
unsigned shaderCount = 0, captureCount = 0, eventCount = 0;
FILE *trace = nullptr;
std::string Ptr(void *p)
{
    char b[40];
    sprintf_s(b, "%p", p);
    return b;
}
std::string Escape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += (char)c;
        }
        else if (c < 32)
        {
            char b[8];
            sprintf_s(b, "\\u%04x", c);
            o += b;
        }
        else
            o += (char)c;
    }
    return o;
}
void InitOutput()
{
    std::call_once(outputOnce, [] {
        outputPath = InspectorSessionDirectory();
        if (!outputPath.empty())
            requestPath = outputPath + L"\\capture.request";
    });
}

template <class T> T Original(void *obj, unsigned slot)
{
    std::lock_guard<std::mutex> g(tableMutex);
    auto it = tables.find(obj);
    if (it == tables.end())
        __fastfail(7);
    return reinterpret_cast<T>(it->second->original[slot]);
}
void SaveShader(void *shader, const void *bytes, SIZE_T count, const char *stage)
{
    if (!shader || !bytes || !count)
        return;
    InitOutput();
    if (outputPath.empty())
        return;
    std::lock_guard<std::mutex> g(recordMutex);
    char name[100];
    sprintf_s(name, "%s_%u.dxbc", stage, ++shaderCount);
    std::string filename = name;
    std::wstring wide(filename.begin(), filename.end());
    FILE *f = nullptr;
    if (_wfopen_s(&f, (outputPath + L"\\" + wide).c_str(), L"wb") == 0)
    {
        fwrite(bytes, 1, count, f);
        fclose(f);
        shaders[shader] = filename;
    }
}
std::string Resource(ID3D11Resource *r)
{
    if (!r)
        return "null";
    std::ostringstream s;
    s << "{\"id\":\"" << Ptr(r) << "\"";
    D3D11_RESOURCE_DIMENSION kind;
    r->GetType(&kind);
    s << ",\"dimension\":" << (int)kind;
    if (kind == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        ID3D11Texture2D *t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t)))
        {
            D3D11_TEXTURE2D_DESC d;
            t->GetDesc(&d);
            s << ",\"width\":" << d.Width << ",\"height\":" << d.Height << ",\"format\":" << (int)d.Format
              << ",\"samples\":" << d.SampleDesc.Count << ",\"mips\":" << d.MipLevels;
            t->Release();
        }
    }
    else if (kind == D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        ID3D11Buffer *b = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        {
            D3D11_BUFFER_DESC d;
            b->GetDesc(&d);
            s << ",\"bytes\":" << d.ByteWidth;
            b->Release();
        }
    }
    s << "}";
    return s.str();
}
template <class V> std::string View(V *v)
{
    if (!v)
        return "null";
    ID3D11Resource *r = nullptr;
    v->GetResource(&r);
    auto s = Resource(r);
    if (r)
        r->Release();
    return s;
}
void StartCapture()
{
    InitOutput();
    if (requestPath.empty() || GetFileAttributesW(requestPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    FILE *f = nullptr;
    unsigned n = 2000;
    if (_wfopen_s(&f, requestPath.c_str(), L"rt") == 0)
    {
        if (fscanf_s(f, "%u", &n) != 1)
            n = 2000;
        fclose(f);
    }
    n = n < 1 ? 1 : (n > 10000 ? 10000 : n);
    DeleteFileW(requestPath.c_str());
    wchar_t name[80];
    swprintf_s(name, L"\\capture_%u.jsonl", ++captureCount);
    if (_wfopen_s(&trace, (outputPath + name).c_str(), L"wb") != 0)
        return;
    setvbuf(trace, nullptr, _IOFBF, 65536);
    fprintf(trace,
            "{\"type\":\"header\",\"version\":2,\"scope\":\"bounded immediate-context state snapshots, no "
            "Present/frame boundary; constant buffer contents and texture pixels not captured\",\"pid\":%lu}\n",
            GetCurrentProcessId());
    eventCount = 0;
    remaining = n;
    ObserverInfo("observer bounded capture started");
}
void Record(ID3D11DeviceContext *c, const char *kind, bool compute = false)
{
    // Deferred contexts cannot provide reliable Get* binding snapshots.
    if (c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return;
    if (!remaining.load(std::memory_order_relaxed))
    {
        ULONGLONG now = GetTickCount64(), old = nextPoll.load();
        if (now < old || !nextPoll.compare_exchange_strong(old, now + 500))
            return;
        std::lock_guard<std::mutex> g(recordMutex);
        if (!remaining)
            StartCapture();
        if (!remaining)
            return;
    }
    std::lock_guard<std::mutex> g(recordMutex);
    if (!remaining || !trace)
        return;
    std::ostringstream s;
    s << "{\"event\":" << ++eventCount << ",\"tick\":" << GetTickCount64() << ",\"thread\":" << GetCurrentThreadId()
      << ",\"context\":\"" << Ptr(c) << "\",\"context_type\":" << (int)c->GetType() << ",\"call\":\"" << kind << "\"";
    ID3D11DeviceChild *shader = nullptr;
    if (compute)
    {
        ID3D11ComputeShader *p = nullptr;
        c->CSGetShader(&p, nullptr, nullptr);
        shader = p;
    }
    else
    {
        ID3D11PixelShader *p = nullptr;
        c->PSGetShader(&p, nullptr, nullptr);
        shader = p;
    }
    s << ",\"shader\":\"" << Ptr(shader) << "\"";
    auto it = shaders.find(shader);
    if (it != shaders.end())
        s << ",\"shader_file\":\"" << it->second << "\"";
    if (shader)
    {
        char name[1024] = {};
        UINT n = 1023;
        if (SUCCEEDED(shader->GetPrivateData(WKPDID_D3DDebugObjectName, &n, name)))
            s << ",\"shader_name\":\"" << Escape(std::string(name, n)) << "\"";
        shader->Release();
    }
    s << ",\"inputs\":[";
    ID3D11ShaderResourceView *input[128] = {};
    if (compute)
        c->CSGetShaderResources(0, 128, input);
    else
        c->PSGetShaderResources(0, 128, input);
    bool sep = false;
    for (unsigned i = 0; i < 128; i++)
        if (input[i])
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC d;
            input[i]->GetDesc(&d);
            if (sep)
                s << ",";
            sep = true;
            s << "{\"slot\":" << i << ",\"view_format\":" << (int)d.Format << ",\"resource\":" << View(input[i]) << "}";
            input[i]->Release();
        }
    s << "]";
    s << ",\"outputs\":[";
    sep = false;
    if (compute)
    {
        ID3D11UnorderedAccessView *v[8] = {};
        c->CSGetUnorderedAccessViews(0, 8, v);
        for (unsigned i = 0; i < 8; i++)
            if (v[i])
            {
                if (sep)
                    s << ",";
                sep = true;
                s << "{\"slot\":" << i << ",\"resource\":" << View(v[i]) << "}";
                v[i]->Release();
            }
    }
    else
    {
        ID3D11RenderTargetView *v[8] = {};
        ID3D11DepthStencilView *z = nullptr;
        c->OMGetRenderTargets(8, v, &z);
        for (unsigned i = 0; i < 8; i++)
            if (v[i])
            {
                D3D11_RENDER_TARGET_VIEW_DESC d;
                v[i]->GetDesc(&d);
                if (sep)
                    s << ",";
                sep = true;
                s << "{\"slot\":" << i << ",\"view_format\":" << (int)d.Format << ",\"resource\":" << View(v[i]) << "}";
                v[i]->Release();
            }
        if (z)
            z->Release();
    }
    s << "]";
    ID3D11Buffer *cb[14] = {};
    if (compute)
        c->CSGetConstantBuffers(0, 14, cb);
    else
        c->PSGetConstantBuffers(0, 14, cb);
    s << ",\"constant_buffers\":[";
    sep = false;
    for (unsigned i = 0; i < 14; i++)
        if (cb[i])
        {
            if (sep)
                s << ",";
            sep = true;
            s << "{\"slot\":" << i << ",\"resource\":" << Resource(cb[i]) << "}";
            cb[i]->Release();
        }
    s << "]";
    if (!compute)
    {
        ID3D11BlendState *b = nullptr;
        FLOAT factor[4];
        UINT mask;
        c->OMGetBlendState(&b, factor, &mask);
        if (b)
        {
            D3D11_BLEND_DESC d;
            b->GetDesc(&d);
            s << ",\"blend\":{\"independent\":" << (int)d.IndependentBlendEnable << ",\"targets\":[";
            for (unsigned i = 0; i < 8; i++)
            {
                if (i)
                    s << ",";
                auto &t = d.RenderTarget[i];
                s << "{\"enabled\":" << t.BlendEnable << ",\"write_mask\":" << (int)t.RenderTargetWriteMask
                  << ",\"source\":" << (int)t.SrcBlend << ",\"dest\":" << (int)t.DestBlend << "}";
            }
            s << "]}";
            b->Release();
        }
    }
    s << "}\n";
    auto text = s.str();
    fwrite(text.data(), 1, text.size(), trace);
    if (--remaining == 0)
    {
        fprintf(trace, "{\"type\":\"end\",\"events\":%u}\n", eventCount);
        fclose(trace);
        trace = nullptr;
        ObserverInfo("observer bounded capture completed");
    }
}
void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext *c, UINT n, UINT start, INT base)
{
    Record(c, "DrawIndexed");
    Original<decltype(&DrawIndexed)>(c, 12)(c, n, start, base);
}
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext *c, UINT n, UINT start)
{
    Record(c, "Draw");
    Original<decltype(&Draw)>(c, 13)(c, n, start);
}
void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f)
{
    Record(c, "DrawIndexedInstanced");
    Original<decltype(&DrawIndexedInstanced)>(c, 20)(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e)
{
    Record(c, "DrawInstanced");
    Original<decltype(&DrawInstanced)>(c, 21)(c, a, b, d, e);
}
void STDMETHODCALLTYPE DrawAuto(ID3D11DeviceContext *c)
{
    Record(c, "DrawAuto");
    Original<decltype(&DrawAuto)>(c, 38)(c);
}
void STDMETHODCALLTYPE DrawIndexedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off)
{
    Record(c, "DrawIndexedInstancedIndirect");
    Original<decltype(&DrawIndexedIndirect)>(c, 39)(c, b, off);
}
void STDMETHODCALLTYPE DrawIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off)
{
    Record(c, "DrawInstancedIndirect");
    Original<decltype(&DrawIndirect)>(c, 40)(c, b, off);
}
void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z)
{
    Record(c, "Dispatch", true);
    Original<decltype(&Dispatch)>(c, 41)(c, x, y, z);
}
void STDMETHODCALLTYPE DispatchIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off)
{
    Record(c, "DispatchIndirect", true);
    Original<decltype(&DispatchIndirect)>(c, 42)(c, b, off);
}
void STDMETHODCALLTYPE Execute(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL restore)
{
    Record(c, "ExecuteCommandList_CONTENTS_NOT_EXPANDED");
    Original<decltype(&Execute)>(c, 58)(c, l, restore);
}
HRESULT STDMETHODCALLTYPE CreatePS(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l,
                                   ID3D11PixelShader **out)
{
    auto hr = Original<decltype(&CreatePS)>(d, 15)(d, b, n, l, out);
    if (SUCCEEDED(hr) && out)
        SaveShader(*out, b, n, "ps");
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateCS(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l,
                                   ID3D11ComputeShader **out)
{
    auto hr = Original<decltype(&CreateCS)>(d, 18)(d, b, n, l, out);
    if (SUCCEEDED(hr) && out)
        SaveShader(*out, b, n, "cs");
    return hr;
}
void InstallContext(ID3D11DeviceContext *c);
HRESULT STDMETHODCALLTYPE CreateDeferred(ID3D11Device *d, UINT flags, ID3D11DeviceContext **out)
{
    auto hr = Original<decltype(&CreateDeferred)>(d, 27)(d, flags, out);
    if (SUCCEEDED(hr) && out && *out && InspectorMode() != L"device-only")
        InstallContext(*out);
    return hr;
}
bool Writable(void *p)
{
    MEMORY_BASIC_INFORMATION m = {};
    if (!VirtualQuery(p, &m, sizeof(m)))
        return false;
    DWORD v = m.Protect & 0xff;
    return v == PAGE_READWRITE || v == PAGE_WRITECOPY || v == PAGE_EXECUTE_READWRITE || v == PAGE_EXECUTE_WRITECOPY;
}
bool InstallTable(void *obj, unsigned count, const std::vector<std::pair<unsigned, void *>> &replacements)
{
    std::lock_guard<std::mutex> g(tableMutex);
    auto current = *reinterpret_cast<void ***>(obj);
    auto existing = tables.find(obj);
    if (existing != tables.end() && existing->second->shadow.get() == current)
        return true;
    if (!Writable(obj))
    {
        ObserverInfo("observer refused non-writable object");
        return false;
    }
    MEMORY_BASIC_INFORMATION debugMemory = {};
    VirtualQuery(current[37], &debugMemory, sizeof(debugMemory));
    char debugMessage[300];
    sprintf_s(debugMessage, "vtable=%p slot37=%p module=%p rva=%llx", current, current[37], debugMemory.AllocationBase,
              (unsigned long long)((char *)current[37] - (char *)debugMemory.AllocationBase));
    ObserverInfo(
        debugMessage); // The runtime uses private virtual methods beyond the public COM interface (GetFeatureLevel
                       // dispatches through slot 69). Preserve the contiguous executable tail.
    unsigned publicCount = count;
    for (; count < 512; count++)
    {
        MEMORY_BASIC_INFORMATION tablePage = {}, codePage = {};
        if (!VirtualQuery(current + count, &tablePage, sizeof(tablePage)) || tablePage.State != MEM_COMMIT ||
            (tablePage.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            break;
        void *entry = current[count];
        if (!VirtualQuery(entry, &codePage, sizeof(codePage)))
            break;
        DWORD protection = codePage.Protect & 0xff;
        if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY)
            break;
    }
    if (count == 512)
    {
        ObserverInfo("observer refused unbounded vtable");
        return false;
    }
    sprintf_s(debugMessage, "vtable cloned public=%u total=%u", publicCount, count);
    ObserverInfo(debugMessage);
    auto table = std::make_unique<Table>();
    table->original = current;
    table->shadow = std::make_unique<void *[]>(count);
    memcpy(table->shadow.get(), current, count * sizeof(void *));
    for (auto &e : replacements)
        table->shadow[e.first] = e.second;
    auto raw = table.get();
    lifetime.push_back(std::move(table));
    tables[obj] = raw;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(obj), raw->shadow.get());
    return true;
}
void InstallContext(ID3D11DeviceContext *c)
{
    ID3D11DeviceContext4 *latest = nullptr;
    if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&latest)))
    {
        ObserverInfo("observer skipped context without Context4");
        return;
    }
    bool same = (void *)latest == (void *)c;
    latest->Release();
    if (!same)
    {
        ObserverInfo("observer skipped aliased Context4");
        return;
    }
    if (InstallTable(c, 149,
                     {{12, (void *)&DrawIndexed},
                      {13, (void *)&Draw},
                      {20, (void *)&DrawIndexedInstanced},
                      {21, (void *)&DrawInstanced},
                      {38, (void *)&DrawAuto},
                      {39, (void *)&DrawIndexedIndirect},
                      {40, (void *)&DrawIndirect},
                      {41, (void *)&Dispatch},
                      {42, (void *)&DispatchIndirect},
                      {58, (void *)&Execute}}))
        ObserverInfo("observer Context4 methods installed");
}
} // namespace
extern "C" void InstallObservers(ID3D11Device *d, ID3D11DeviceContext *c)
{
    auto mode = InspectorMode();
    if (mode == L"forward")
    {
        ObserverInfo("observer mode=forward");
        return;
    }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&InstallObservers), &pinned);
    InitOutput();
    ID3D11Device *ownedDevice = nullptr;
    ID3D11DeviceContext *ownedContext = nullptr;
    if (!d && c)
    {
        c->GetDevice(&ownedDevice);
        d = ownedDevice;
    }
    if (d && !c)
    {
        d->GetImmediateContext(&ownedContext);
        c = ownedContext;
    }
    if (d)
    {
        ID3D11Device5 *latest = nullptr;
        if (SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device5), (void **)&latest)))
        {
            bool same = (void *)latest == (void *)d;
            latest->Release();
            if (same)
            {
                if (InstallTable(d, 69,
                                 {{15, (void *)&CreatePS}, {18, (void *)&CreateCS}, {27, (void *)&CreateDeferred}}))
                    ObserverInfo("observer Device5 methods installed");
            }
            else
                ObserverInfo("observer skipped aliased Device5");
        }
        else
            ObserverInfo("observer skipped device without Device5");
    }
    if (c && mode != L"device-only")
        InstallContext(c);
    if (mode == L"device-only")
        ObserverInfo("observer mode=device-only");
    if (ownedContext)
        ownedContext->Release();
    if (ownedDevice)
        ownedDevice->Release();
}
