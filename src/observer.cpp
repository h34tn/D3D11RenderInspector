#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <d3d11_4.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// Optional readback, enabled per request and only while a bounded capture is active. Reads are ordered copies
// on the immediate context followed by a blocking Map, so they show the state at that point in the command stream.
bool readConstants = false, readTextures = false;
std::vector<std::string> nameFilters;
std::wstring dataDirectory;
std::string dataPrefix;
std::unordered_set<unsigned long long> savedBlobs;
std::unordered_map<void *, std::string> savedTextures;
unsigned long long savedTextureBytes = 0;
ID3D11Buffer *bufferStaging = nullptr;
constexpr UINT kBufferReadLimit = 65536;
constexpr unsigned long long kTextureFileLimit = 64ull << 20, kTextureCaptureLimit = 1ull << 30;
constexpr size_t kTextureCountLimit = 1024;
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
    // Line 1: event count. Optional lines: constants=1, textures=1, filter=Name|Other (debug-name substrings).
    std::string request;
    if (_wfopen_s(&f, requestPath.c_str(), L"rb") == 0)
    {
        char buffer[2048];
        size_t got = fread(buffer, 1, sizeof(buffer) - 1, f);
        request.assign(buffer, got);
        fclose(f);
    }
    if (sscanf_s(request.c_str(), "%u", &n) != 1)
        n = 2000;
    n = n < 1 ? 1 : (n > 10000 ? 10000 : n);
    readConstants = request.find("\nconstants=1") != std::string::npos;
    readTextures = request.find("\ntextures=1") != std::string::npos;
    nameFilters.clear();
    auto at = request.find("\nfilter=");
    if (at != std::string::npos)
    {
        auto end = request.find_first_of("\r\n", at + 8);
        std::string list = request.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
        size_t start = 0;
        while (start <= list.size())
        {
            auto bar = list.find('|', start);
            auto item = list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!item.empty())
                nameFilters.push_back(item);
            if (bar == std::string::npos)
                break;
            start = bar + 1;
        }
    }
    DeleteFileW(requestPath.c_str());
    wchar_t name[80];
    swprintf_s(name, L"\\capture_%u.jsonl", ++captureCount);
    if (_wfopen_s(&trace, (outputPath + name).c_str(), L"wb") != 0)
        return;
    setvbuf(trace, nullptr, _IOFBF, 65536);
    savedBlobs.clear();
    savedTextures.clear();
    savedTextureBytes = 0;
    dataDirectory.clear();
    dataPrefix.clear();
    if (readConstants || readTextures)
    {
        char folder[64];
        sprintf_s(folder, "capture_%u_data", captureCount);
        dataPrefix = std::string(folder) + "/";
        dataDirectory = outputPath + L"\\" + std::wstring(folder, folder + strlen(folder));
        CreateDirectoryW(dataDirectory.c_str(), nullptr);
    }
    std::ostringstream header;
    header << "{\"type\":\"header\",\"version\":4,\"scope\":\"bounded immediate-context state snapshots, no "
              "Present/frame boundary; optional constant/vertex buffer and texture mip-0 readback for matching "
              "events only; indirect argument buffer contents not captured\",\"options\":{\"constants\":"
           << (int)readConstants << ",\"textures\":" << (int)readTextures << ",\"filter\":[";
    for (size_t i = 0; i < nameFilters.size(); i++)
        header << (i ? "," : "") << "\"" << Escape(nameFilters[i]) << "\"";
    header << "],\"data\":\"" << dataPrefix << "\"},\"pid\":" << GetCurrentProcessId() << "}\n";
    auto text = header.str();
    fwrite(text.data(), 1, text.size(), trace);
    eventCount = 0;
    remaining = n;
    ObserverInfo("observer bounded capture started");
}
// Raw call parameters, kept on the stack for every call; formatted only while a capture is active.
// Indirect calls only identify the argument buffer; its contents are not read back.
struct CallArgs
{
    const char *names[5] = {};
    long long values[5] = {};
    unsigned count = 0;
    ID3D11Buffer *indirect = nullptr;
    UINT offset = 0;
    CallArgs(std::initializer_list<std::pair<const char *, long long>> list)
    {
        for (auto &v : list)
            if (count < 5)
            {
                names[count] = v.first;
                values[count++] = v.second;
            }
    }
    CallArgs(ID3D11Buffer *buffer, UINT bufferOffset) : indirect(buffer), offset(bufferOffset)
    {
    }
};
std::string FormatArgs(const CallArgs &a)
{
    std::ostringstream s;
    if (a.indirect)
    {
        s << "{\"buffer\":" << Resource(a.indirect) << ",\"offset\":" << a.offset << "}";
        return s.str();
    }
    s << "{";
    for (unsigned i = 0; i < a.count; i++)
        s << (i ? "," : "") << "\"" << a.names[i] << "\":" << a.values[i];
    s << "}";
    return s.str();
}
std::string DepthState(ID3D11DeviceContext *c)
{
    ID3D11DepthStencilState *state = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&state, &ref);
    D3D11_DEPTH_STENCIL_DESC d = {};
    bool isDefault = state == nullptr;
    if (state)
    {
        state->GetDesc(&d);
        state->Release();
    }
    else
    {
        // A null state is the D3D11 default: depth test LESS with writes, stencil off.
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilEnable = FALSE;
    }
    std::ostringstream s;
    s << "{\"default\":" << (int)isDefault << ",\"depth_enable\":" << (int)d.DepthEnable
      << ",\"depth_write\":" << (int)d.DepthWriteMask << ",\"depth_func\":" << (int)d.DepthFunc
      << ",\"stencil_enable\":" << (int)d.StencilEnable << ",\"stencil_ref\":" << ref << "}";
    return s.str();
}
unsigned long long Fnv(const void *data, size_t size)
{
    unsigned long long h = 1469598103934665603ull;
    auto p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
bool WriteData(const std::string &file, const void *head, size_t headSize, const void *body, size_t bodySize)
{
    FILE *f = nullptr;
    std::wstring path = dataDirectory + L"\\" + std::wstring(file.begin(), file.end());
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0)
        return false;
    bool ok = (!headSize || fwrite(head, 1, headSize, f) == headSize) && fwrite(body, 1, bodySize, f) == bodySize;
    fclose(f);
    return ok;
}
// Copies [offset, offset+limit) of a buffer (clamped to its size and 64 KiB) into a content-addressed blob.
std::string ReadBuffer(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT offset, UINT limit)
{
    D3D11_BUFFER_DESC desc;
    b->GetDesc(&desc);
    if (offset >= desc.ByteWidth)
        return ",\"data_error\":\"offset beyond buffer\"";
    UINT size = desc.ByteWidth - offset;
    if (limit && limit < size)
        size = limit;
    bool truncated = size > kBufferReadLimit;
    if (truncated)
        size = kBufferReadLimit;
    if (!bufferStaging)
    {
        ID3D11Device *d = nullptr;
        c->GetDevice(&d);
        D3D11_BUFFER_DESC s = {};
        s.ByteWidth = kBufferReadLimit;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = d ? d->CreateBuffer(&s, nullptr, &bufferStaging) : E_FAIL;
        if (d)
            d->Release();
        if (FAILED(hr))
            return ",\"data_error\":\"staging buffer\"";
    }
    D3D11_BOX box = {offset, 0, 0, offset + size, 1, 1};
    c->CopySubresourceRegion(bufferStaging, 0, 0, 0, 0, b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(c->Map(bufferStaging, 0, D3D11_MAP_READ, 0, &m)))
        return ",\"data_error\":\"map\"";
    auto hash = Fnv(m.pData, size);
    char file[48];
    sprintf_s(file, "buf_%016llx.bin", hash);
    bool ok = true;
    if (savedBlobs.insert(hash).second && !WriteData(file, nullptr, 0, m.pData, size))
    {
        savedBlobs.erase(hash);
        ok = false;
    }
    c->Unmap(bufferStaging, 0);
    if (!ok)
        return ",\"data_error\":\"write\"";
    std::ostringstream s;
    s << ",\"data\":\"" << dataPrefix << file << "\",\"data_offset\":" << offset << ",\"data_bytes\":" << size;
    if (truncated)
        s << ",\"data_truncated\":1";
    return s.str();
}
// Bytes per pixel, or per 4x4 block when *block is set; 0 for formats the dump does not lay out.
UINT FormatBytes(DXGI_FORMAT f, bool *block)
{
    *block = false;
    int v = (int)f;
    if (v >= 1 && v <= 4)
        return 16;
    if (v >= 5 && v <= 8)
        return 12;
    if (v >= 9 && v <= 22)
        return 8;
    if ((v >= 23 && v <= 47) || v == 67 || (v >= 87 && v <= 93))
        return 4;
    if ((v >= 48 && v <= 59) || v == 85 || v == 86 || v == 115)
        return 2;
    if (v >= 60 && v <= 65)
        return 1;
    *block = true;
    if ((v >= 70 && v <= 72) || (v >= 79 && v <= 81))
        return 8;
    if ((v >= 73 && v <= 78) || (v >= 82 && v <= 84) || (v >= 94 && v <= 99))
        return 16;
    *block = false;
    return 0;
}
// Dumps mip 0 / slice 0 of a 2D texture as DDS (DX10 header) once per capture; later references reuse it.
std::string DumpTexture(ID3D11DeviceContext *c, ID3D11Resource *r, DXGI_FORMAT viewFormat)
{
    D3D11_RESOURCE_DIMENSION kind;
    r->GetType(&kind);
    if (kind != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return ",\"dump_skipped\":\"not texture2d\"";
    auto found = savedTextures.find(r);
    if (found != savedTextures.end())
        return found->second;
    std::string result;
    ID3D11Texture2D *t = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t)))
        return ",\"dump_skipped\":\"query\"";
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    bool block = false;
    UINT unit = FormatBytes(d.Format, &block);
    UINT rows = block ? (d.Height + 3) / 4 : d.Height;
    UINT rowBytes = block ? ((d.Width + 3) / 4) * unit : d.Width * unit;
    unsigned long long total = (unsigned long long)rows * rowBytes;
    if (d.SampleDesc.Count > 1)
        result = ",\"dump_skipped\":\"multisampled\"";
    else if (!unit)
        result = ",\"dump_skipped\":\"format\"";
    else if (total > kTextureFileLimit)
        result = ",\"dump_skipped\":\"size\"";
    else if (savedTextures.size() >= kTextureCountLimit || savedTextureBytes + total > kTextureCaptureLimit)
        result = ",\"dump_skipped\":\"budget\"";
    else
    {
        D3D11_TEXTURE2D_DESC s = d;
        s.MipLevels = s.ArraySize = 1;
        s.SampleDesc = {1, 0};
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = s.MiscFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device *device = nullptr;
        ID3D11Texture2D *staging = nullptr;
        c->GetDevice(&device);
        HRESULT hr = device ? device->CreateTexture2D(&s, nullptr, &staging) : E_FAIL;
        if (device)
            device->Release();
        if (FAILED(hr))
            result = ",\"dump_skipped\":\"staging\"";
        else
        {
            // Whole-subresource copy (null box) is also the only form allowed for depth-stencil sources.
            c->CopySubresourceRegion(staging, 0, 0, 0, 0, t, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (FAILED(c->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
                result = ",\"dump_skipped\":\"map\"";
            else
            {
                std::vector<unsigned char> body(total);
                for (UINT y = 0; y < rows; y++)
                    memcpy(body.data() + (size_t)y * rowBytes, (char *)m.pData + (size_t)y * m.RowPitch, rowBytes);
                c->Unmap(staging, 0);
                DWORD head[37] = {};
                head[0] = 0x20534444; // "DDS "
                head[1] = 124;
                head[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (block ? 0x80000 : 0x8);
                head[3] = d.Height;
                head[4] = d.Width;
                head[5] = block ? (DWORD)total : rowBytes;
                head[7] = 1;
                head[19] = 32;
                head[20] = 0x4;        // DDPF_FOURCC
                head[21] = 0x30315844; // "DX10"
                head[27] = 0x1000;     // DDSCAPS_TEXTURE
                // Typeless resources take the view's typed format so viewers can decode them.
                head[32] = (DWORD)(viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : d.Format);
                head[33] = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
                head[35] = 1;
                char file[48];
                sprintf_s(file, "tex_%p.dds", (void *)r);
                if (WriteData(file, head, sizeof(head), body.data(), body.size()))
                {
                    savedTextureBytes += total;
                    std::ostringstream o;
                    o << ",\"dump\":\"" << dataPrefix << file << "\"";
                    result = o.str();
                }
                else
                    result = ",\"dump_skipped\":\"write\"";
            }
            staging->Release();
        }
    }
    t->Release();
    savedTextures[r] = result;
    return result;
}
std::string ShaderName(ID3D11DeviceChild *shader)
{
    char name[1024] = {};
    UINT n = 1023;
    if (shader && SUCCEEDED(shader->GetPrivateData(WKPDID_D3DDebugObjectName, &n, name)))
        return std::string(name, n);
    return std::string();
}
std::string ShaderFields(const char *prefix, ID3D11DeviceChild *shader, const std::string &name)
{
    std::ostringstream s;
    s << ",\"" << prefix << "shader\":\"" << Ptr(shader) << "\"";
    auto it = shaders.find(shader);
    if (it != shaders.end())
        s << ",\"" << prefix << "shader_file\":\"" << it->second << "\"";
    if (!name.empty())
        s << ",\"" << prefix << "shader_name\":\"" << Escape(name) << "\"";
    return s.str();
}
bool MatchesFilter(const std::string &a, const std::string &b)
{
    if (nameFilters.empty())
        return true;
    for (auto &f : nameFilters)
        if (a.find(f) != std::string::npos || b.find(f) != std::string::npos)
            return true;
    return false;
}
enum class Stage
{
    Vertex,
    Pixel,
    Compute
};
std::string ShaderInputs(ID3D11DeviceContext *c, Stage stage, bool dump)
{
    ID3D11ShaderResourceView *input[128] = {};
    if (stage == Stage::Compute)
        c->CSGetShaderResources(0, 128, input);
    else if (stage == Stage::Pixel)
        c->PSGetShaderResources(0, 128, input);
    else
        c->VSGetShaderResources(0, 128, input);
    std::ostringstream s;
    s << "[";
    bool sep = false;
    for (unsigned i = 0; i < 128; i++)
        if (input[i])
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC d;
            input[i]->GetDesc(&d);
            s << (sep ? "," : "") << "{\"slot\":" << i << ",\"view_format\":" << (int)d.Format
              << ",\"resource\":" << View(input[i]);
            sep = true;
            if (dump)
            {
                ID3D11Resource *r = nullptr;
                input[i]->GetResource(&r);
                if (r)
                {
                    s << DumpTexture(c, r, d.Format);
                    r->Release();
                }
            }
            s << "}";
            input[i]->Release();
        }
    s << "]";
    return s.str();
}
// Uses the D3D11.1 getters when available so offset bindings (first/num constants) are reported and read exactly.
std::string ConstantBuffers(ID3D11DeviceContext *c, Stage stage, bool read)
{
    ID3D11Buffer *cb[14] = {};
    UINT first[14] = {}, count[14] = {};
    ID3D11DeviceContext1 *c1 = nullptr;
    bool ranges = SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c1));
    if (ranges)
    {
        if (stage == Stage::Compute)
            c1->CSGetConstantBuffers1(0, 14, cb, first, count);
        else if (stage == Stage::Pixel)
            c1->PSGetConstantBuffers1(0, 14, cb, first, count);
        else
            c1->VSGetConstantBuffers1(0, 14, cb, first, count);
        c1->Release();
    }
    else if (stage == Stage::Compute)
        c->CSGetConstantBuffers(0, 14, cb);
    else if (stage == Stage::Pixel)
        c->PSGetConstantBuffers(0, 14, cb);
    else
        c->VSGetConstantBuffers(0, 14, cb);
    std::ostringstream s;
    s << "[";
    bool sep = false;
    for (unsigned i = 0; i < 14; i++)
        if (cb[i])
        {
            s << (sep ? "," : "") << "{\"slot\":" << i << ",\"resource\":" << Resource(cb[i]);
            sep = true;
            if (ranges)
                s << ",\"first_constant\":" << first[i] << ",\"num_constants\":" << count[i];
            if (read)
                s << ReadBuffer(c, cb[i], first[i] * 16, ranges ? count[i] * 16 : 0);
            s << "}";
            cb[i]->Release();
        }
    s << "]";
    return s.str();
}
std::string VertexBuffers(ID3D11DeviceContext *c, bool read)
{
    ID3D11Buffer *vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT stride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {}, offset[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vb, stride, offset);
    std::ostringstream s;
    s << "[";
    bool sep = false;
    for (unsigned i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++)
        if (vb[i])
        {
            s << (sep ? "," : "") << "{\"slot\":" << i << ",\"stride\":" << stride[i] << ",\"offset\":" << offset[i]
              << ",\"resource\":" << Resource(vb[i]);
            sep = true;
            if (read)
                s << ReadBuffer(c, vb[i], offset[i], 0);
            s << "}";
            vb[i]->Release();
        }
    s << "]";
    ID3D11Buffer *ib = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    c->IAGetIndexBuffer(&ib, &format, &ibOffset);
    s << ",\"index_buffer\":";
    if (ib)
    {
        s << "{\"format\":" << (int)format << ",\"offset\":" << ibOffset << ",\"resource\":" << Resource(ib);
        if (read)
            s << ReadBuffer(c, ib, ibOffset, 0);
        s << "}";
        ib->Release();
    }
    else
        s << "null";
    return s.str();
}
void Record(ID3D11DeviceContext *c, const char *kind, const CallArgs &args, bool compute = false)
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
      << ",\"context\":\"" << Ptr(c) << "\",\"context_type\":" << (int)c->GetType() << ",\"call\":\"" << kind << "\""
      << ",\"args\":" << FormatArgs(args);
    ID3D11DeviceChild *shader = nullptr, *vertex = nullptr;
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
        ID3D11VertexShader *v = nullptr;
        c->VSGetShader(&v, nullptr, nullptr);
        vertex = v;
    }
    auto shaderName = ShaderName(shader), vertexName = ShaderName(vertex);
    s << ShaderFields("", shader, shaderName);
    if (!compute)
    {
        s << ShaderFields("vs_", vertex, vertexName);
        ID3D11GeometryShader *gs = nullptr;
        ID3D11HullShader *hs = nullptr;
        ID3D11DomainShader *ds = nullptr;
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        if (gs)
            s << ShaderFields("gs_", gs, ShaderName(gs));
        if (hs)
            s << ShaderFields("hs_", hs, ShaderName(hs));
        if (ds)
            s << ShaderFields("ds_", ds, ShaderName(ds));
        for (ID3D11DeviceChild *x : {(ID3D11DeviceChild *)gs, (ID3D11DeviceChild *)hs, (ID3D11DeviceChild *)ds})
            if (x)
                x->Release();
    }
    if (shader)
        shader->Release();
    if (vertex)
        vertex->Release();
    bool matched = (readConstants || readTextures) && MatchesFilter(shaderName, vertexName);
    if (matched)
        s << ",\"readback\":1";
    s << ",\"inputs\":" << ShaderInputs(c, compute ? Stage::Compute : Stage::Pixel, matched && readTextures);
    if (!compute)
    {
        s << ",\"vs_inputs\":" << ShaderInputs(c, Stage::Vertex, matched && readTextures);
        s << ",\"vertex_buffers\":" << VertexBuffers(c, matched && readConstants);
    }
    s << ",\"outputs\":[";
    bool sep = false;
    ID3D11DepthStencilView *z = nullptr;
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
    }
    s << "]";
    if (!compute)
    {
        s << ",\"depth_target\":";
        if (z)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC d;
            z->GetDesc(&d);
            s << "{\"view_format\":" << (int)d.Format << ",\"flags\":" << d.Flags << ",\"resource\":" << View(z)
              << "}";
            z->Release();
        }
        else
            s << "null";
        s << ",\"depth_state\":" << DepthState(c);
    }
    s << ",\"constant_buffers\":"
      << ConstantBuffers(c, compute ? Stage::Compute : Stage::Pixel, matched && readConstants);
    if (!compute)
        s << ",\"vs_constant_buffers\":" << ConstantBuffers(c, Stage::Vertex, matched && readConstants);
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
        if (bufferStaging)
        {
            bufferStaging->Release();
            bufferStaging = nullptr;
        }
        readConstants = readTextures = false;
        savedTextures.clear();
        ObserverInfo("observer bounded capture completed");
    }
}
void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext *c, UINT n, UINT start, INT base)
{
    Record(c, "DrawIndexed", {{"index_count", n}, {"start_index", start}, {"base_vertex", base}});
    Original<decltype(&DrawIndexed)>(c, 12)(c, n, start, base);
}
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext *c, UINT n, UINT start)
{
    Record(c, "Draw", {{"vertex_count", n}, {"start_vertex", start}});
    Original<decltype(&Draw)>(c, 13)(c, n, start);
}
void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f)
{
    Record(c, "DrawIndexedInstanced",
           {{"index_count", a}, {"instance_count", b}, {"start_index", d}, {"base_vertex", e}, {"start_instance", f}});
    Original<decltype(&DrawIndexedInstanced)>(c, 20)(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e)
{
    Record(c, "DrawInstanced", {{"vertex_count", a}, {"instance_count", b}, {"start_vertex", d}, {"start_instance", e}});
    Original<decltype(&DrawInstanced)>(c, 21)(c, a, b, d, e);
}
void STDMETHODCALLTYPE DrawAuto(ID3D11DeviceContext *c)
{
    Record(c, "DrawAuto", CallArgs{});
    Original<decltype(&DrawAuto)>(c, 38)(c);
}
void STDMETHODCALLTYPE DrawIndexedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off)
{
    Record(c, "DrawIndexedInstancedIndirect", CallArgs(b, off));
    Original<decltype(&DrawIndexedIndirect)>(c, 39)(c, b, off);
}
void STDMETHODCALLTYPE DrawIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off)
{
    Record(c, "DrawInstancedIndirect", CallArgs(b, off));
    Original<decltype(&DrawIndirect)>(c, 40)(c, b, off);
}
void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z)
{
    Record(c, "Dispatch", {{"x", x}, {"y", y}, {"z", z}}, true);
    Original<decltype(&Dispatch)>(c, 41)(c, x, y, z);
}
void STDMETHODCALLTYPE DispatchIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off)
{
    Record(c, "DispatchIndirect", CallArgs(b, off), true);
    Original<decltype(&DispatchIndirect)>(c, 42)(c, b, off);
}
void STDMETHODCALLTYPE Execute(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL restore)
{
    Record(c, "ExecuteCommandList_CONTENTS_NOT_EXPANDED", {{"restore_state", restore}});
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
HRESULT STDMETHODCALLTYPE CreateVS(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l,
                                   ID3D11VertexShader **out)
{
    auto hr = Original<decltype(&CreateVS)>(d, 12)(d, b, n, l, out);
    if (SUCCEEDED(hr) && out)
        SaveShader(*out, b, n, "vs");
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateGS(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l,
                                   ID3D11GeometryShader **out)
{
    auto hr = Original<decltype(&CreateGS)>(d, 13)(d, b, n, l, out);
    if (SUCCEEDED(hr) && out)
        SaveShader(*out, b, n, "gs");
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateHS(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l,
                                   ID3D11HullShader **out)
{
    auto hr = Original<decltype(&CreateHS)>(d, 16)(d, b, n, l, out);
    if (SUCCEEDED(hr) && out)
        SaveShader(*out, b, n, "hs");
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateDS(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l,
                                   ID3D11DomainShader **out)
{
    auto hr = Original<decltype(&CreateDS)>(d, 17)(d, b, n, l, out);
    if (SUCCEEDED(hr) && out)
        SaveShader(*out, b, n, "ds");
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
                                 {{12, (void *)&CreateVS},
                                  {13, (void *)&CreateGS},
                                  {15, (void *)&CreatePS},
                                  {16, (void *)&CreateHS},
                                  {17, (void *)&CreateDS},
                                  {18, (void *)&CreateCS},
                                  {27, (void *)&CreateDeferred}}))
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
