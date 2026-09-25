#include "overlay.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <DirectXPackedVector.h>

namespace crml::probe {
namespace {
using Microsoft::WRL::ComPtr;
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using Resize1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*);
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
using Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
using Enhanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*,UINT,const D3D12_BARRIER_GROUP*);
Present original_present{}; Present1 original_present1{};
Resize original_resize{}; Resize1 original_resize1{};
Execute original_execute{}; Reset original_reset{}; Barrier original_barrier{}; Enhanced original_enhanced{};
std::atomic<bool> enabled{}, visible{};
std::atomic<int> status{-1};
std::atomic<uint64_t> presents{}, frames{}, queue_matches{};
std::atomic<const char*> diagnostic{"not_started"};
thread_local bool internal{};
struct Internal { bool before=internal; Internal(){internal=true;} ~Internal(){internal=before;} };

struct Frame {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    UINT64 submitted{};
    bool ready{};
};
struct Mark { ID3D12CommandList* list{}; unsigned touched{}, present{}; };
// Hooks remain resident until process exit. Heap lifetime avoids COM teardown under loader lock.
struct State {
    std::mutex mutex;
    IDXGISwapChain* swap{}; // Identity only; do not keep the application's swapchain alive.
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>,5> uploads;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::array<Frame,8> buffers;
    std::array<Mark,512> marks{};
    UINT count{};
    std::array<std::vector<unsigned char>,5> text;
    bool failed{};
};
State& state() { static auto* value=new State; return *value; }

bool complete(Frame& f) {
    return !f.submitted || f.fence->GetCompletedValue() >= f.submitted;
}
bool release_buffers(State& s) {
    // Resize must release every back-buffer reference, after our own GPU work finishes.
    for(auto& f:s.buffers) if(!complete(f)) {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) return false;
        const bool ok=SUCCEEDED(f.fence->SetEventOnCompletion(f.submitted,event)) && WaitForSingleObject(event,2000)==WAIT_OBJECT_0;
        CloseHandle(event);
        if(!ok) { diagnostic="gpu_busy"; return false; }
    }
    s.buffers={}; s.marks={}; s.uploads={}; s.device.Reset(); s.swap=nullptr; s.count=0; s.failed=false;
    return true;
}

bool initialize(State& s, IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if(FAILED(swap->GetDesc(&desc)) || desc.BufferCount<2 || desc.BufferCount>s.buffers.size() || desc.BufferDesc.Width<640 || desc.BufferDesc.Height<200) return false;
    DWORD pid{}; GetWindowThreadProcessId(desc.OutputWindow,&pid);
    if(pid!=GetCurrentProcessId()) return false;
    ComPtr<ID3D12Device> device;
    if(FAILED(swap->GetDevice(IID_PPV_ARGS(&device)))) { diagnostic="requires_dx12"; return false; }
    if(!release_buffers(s)) return false;
    s.device=device; s.count=desc.BufferCount;
    DXGI_FORMAT format=desc.BufferDesc.Format;
    const bool wide=format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(!wide && format!=DXGI_FORMAT_R8G8B8A8_UNORM && format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
       format!=DXGI_FORMAT_B8G8R8A8_UNORM && format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && format!=DXGI_FORMAT_R10G10B10A2_UNORM) {
        diagnostic="unsupported_backbuffer_format"; return false;
    }
    D3D12_RESOURCE_DESC texture{}; texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=510; texture.Height=86; texture.DepthOrArraySize=1; texture.MipLevels=1; texture.Format=format; texture.SampleDesc.Count=1;
    UINT64 bytes{}; device->GetCopyableFootprints(&texture,0,1,0,&s.footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=bytes;
    buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES properties{}; properties.Type=D3D12_HEAP_TYPE_UPLOAD;
    for(size_t n=0;n<s.uploads.size();++n) {
        if(FAILED(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.uploads[n])))) return false;
        void* mapped{}; D3D12_RANGE read{0,0}; if(FAILED(s.uploads[n]->Map(0,&read,&mapped))) return false;
        for(unsigned y=0;y<86;++y) for(unsigned x=0;x<510;++x) {
            const bool ink=s.text[n][y*510+x]!=0;
            float r=ink?.82f:.025f, g=ink?.9f:.033f, b=ink?1.f:.045f;
            auto* dest=static_cast<unsigned char*>(mapped)+y*s.footprint.Footprint.RowPitch+x*(wide?8:4);
            if(wide) {
                using DirectX::PackedVector::XMConvertFloatToHalf;
                const uint16_t pixel[]{XMConvertFloatToHalf(r),XMConvertFloatToHalf(g),XMConvertFloatToHalf(b),XMConvertFloatToHalf(1.f)};
                std::memcpy(dest,pixel,8);
            } else if(format==DXGI_FORMAT_R10G10B10A2_UNORM) {
                const uint32_t pixel=uint32_t(r*1023)|(uint32_t(g*1023)<<10)|(uint32_t(b*1023)<<20)|0xc0000000u; std::memcpy(dest,&pixel,4);
            } else {
                if(format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) std::swap(r,b);
                dest[0]=static_cast<unsigned char>(r*255); dest[1]=static_cast<unsigned char>(g*255); dest[2]=static_cast<unsigned char>(b*255); dest[3]=255;
            }
        }
        s.uploads[n]->Unmap(0,nullptr);
    }
    for(UINT i=0;i<s.count;++i) {
        auto& f=s.buffers[i];
        if(FAILED(swap->GetBuffer(i,IID_PPV_ARGS(&f.buffer))) ||
           FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator))) ||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.commands))) ||
           FAILED(f.commands->Close()) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.fence)))) return false;

    }
    s.swap=swap; diagnostic="waiting_for_backbuffer_queue";
    return true;
}

void render(IDXGISwapChain* swap, UINT flags) noexcept {
    if(internal || !enabled || (flags&DXGI_PRESENT_TEST)) return;
    Internal guard;
    ++presents;
    auto& s=state(); std::lock_guard lock(s.mutex);
    if(s.swap!=swap && !initialize(s,swap)) return;
    if(s.failed) return;
    ComPtr<IDXGISwapChain3> swap3;
    if(FAILED(swap->QueryInterface(IID_PPV_ARGS(&swap3)))) return;
    const UINT index=swap3->GetCurrentBackBufferIndex();
    if(index>=s.count) return;
    auto& f=s.buffers[index];
    const bool ready=f.ready; f.ready=false;
    // Never guess a queue from unrelated ExecuteCommandLists calls. Require an observed
    // transition of THIS back buffer to PRESENT on a DIRECT queue in this frame.
    if(!ready || !f.queue) { diagnostic="waiting_for_backbuffer_queue"; return; }
    if(!visible) { diagnostic="hidden"; return; }
    if(!complete(f)) { diagnostic="gpu_busy"; return; }
    if(FAILED(f.allocator->Reset()) || FAILED(original_reset(f.commands.Get(),f.allocator.Get(),nullptr))) { s.failed=true; diagnostic="reset_failed"; return; }
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={f.buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST};
    original_barrier(f.commands.Get(),1,&barrier);
    const int value=status.load();
    D3D12_TEXTURE_COPY_LOCATION source{},destination{};
    source.pResource=s.uploads[value>=0 && value<=3?value+1:0].Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint=s.footprint;
    destination.pResource=f.buffer.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    f.commands->CopyTextureRegion(&destination,20,20,0,&source,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    original_barrier(f.commands.Get(),1,&barrier);
    if(FAILED(f.commands->Close())) { s.failed=true; diagnostic="close_failed"; return; }
    ID3D12CommandList* commands[]{f.commands.Get()};
    original_execute(f.queue.Get(),1,commands);
    if(FAILED(f.queue->Signal(f.fence.Get(),++f.submitted))) { s.failed=true; diagnostic="signal_failed"; return; }
    ++frames; diagnostic="rendering";
}

void mark(State& s, ID3D12CommandList* list, ID3D12Resource* resource, bool present) {
    unsigned bit{};
    for(UINT i=0;i<s.count;++i) if(s.buffers[i].buffer.Get()==resource) bit=1u<<i;
    if(!bit) return;
    auto found=std::find_if(s.marks.begin(),s.marks.end(),[&](const Mark& m){return m.list==list;});
    if(found==s.marks.end()) found=std::find_if(s.marks.begin(),s.marks.end(),[](const Mark& m){return !m.list;});
    if(found==s.marks.end()) { diagnostic="tracking_capacity"; return; }
    found->list=list; found->touched|=bit;
    found->present=present ? found->present|bit : found->present&~bit;
}
void STDMETHODCALLTYPE barrier_hook(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers) {
    if(!internal && enabled) {
        auto& s=state(); std::lock_guard lock(s.mutex);
        for(UINT i=0;i<count;++i) if(barriers[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            const auto& b=barriers[i];
            mark(s,list,b.Transition.pResource,b.Transition.StateAfter==D3D12_RESOURCE_STATE_PRESENT && b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
        }
    }
    original_barrier(list,count,barriers);
}
void STDMETHODCALLTYPE enhanced_hook(ID3D12GraphicsCommandList7* list,UINT count,const D3D12_BARRIER_GROUP* groups) {
    if(!internal && enabled) {
        auto& s=state(); std::lock_guard lock(s.mutex);
        for(UINT i=0;i<count;++i) if(groups[i].Type==D3D12_BARRIER_TYPE_TEXTURE)
            for(UINT j=0;j<groups[i].NumBarriers;++j) {
                const auto& b=groups[i].pTextureBarriers[j];
                mark(s,list,b.pResource,b.LayoutAfter==D3D12_BARRIER_LAYOUT_PRESENT && b.SyncAfter!=D3D12_BARRIER_SYNC_SPLIT);
            }
    }
    original_enhanced(list,count,groups);
}
HRESULT STDMETHODCALLTYPE reset_hook(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) {
    const auto result=original_reset(list,allocator,pipeline);
    if(SUCCEEDED(result) && !internal && enabled) {
        auto& s=state(); std::lock_guard lock(s.mutex);
        for(auto& m:s.marks) if(m.list==list) m={};
    }
    return result;
}
void STDMETHODCALLTYPE execute_hook(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    original_execute(queue,count,lists);
    if(internal || !enabled) return;
    auto& s=state(); std::lock_guard lock(s.mutex);
    for(UINT i=0;i<count;++i) for(auto& m:s.marks) if(m.list==lists[i]) {
        for(UINT b=0;b<s.count;++b) if(m.touched&(1u<<b)) {
            auto& f=s.buffers[b];
            f.ready=(m.present&(1u<<b)) && queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT;
            if(f.ready) { f.queue=queue; ++queue_matches; }
        }
    }
}
HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swap,UINT sync,UINT flags) {
    render(swap,flags); Internal guard; return original_present(swap,sync,flags);
}
HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* swap,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) {
    render(swap,flags); Internal guard;
    // A full present includes the panel even when the application supplies dirty rectangles.
    DXGI_PRESENT_PARAMETERS full{};
    return original_present1(swap,sync,flags,enabled && visible && !(flags&DXGI_PRESENT_TEST)?&full:params);
}
HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) {
    if(internal) return original_resize(swap,count,width,height,format,flags);
    Internal guard;
    auto& s=state(); std::lock_guard lock(s.mutex);
    if(s.swap==swap && !release_buffers(s)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return original_resize(swap,count,width,height,format,flags);
}
HRESULT STDMETHODCALLTYPE resize1_hook(IDXGISwapChain3* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,IUnknown* const* queues) {
    if(internal) return original_resize1(swap,count,width,height,format,flags,masks,queues);
    Internal guard;
    auto& s=state(); std::lock_guard lock(s.mutex);
    if(s.swap==swap && !release_buffers(s)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return original_resize1(swap,count,width,height,format,flags,masks,queues);
}

bool rasterize(State& s) {
    // GDI creates the font mask once; only native D3D12 commands touch the game image.
    constexpr int width=510,height=86;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    auto dc=CreateCompatibleDC(nullptr); void* pixels{};
    auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    auto font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!dc || !bitmap || !font) { if(font)DeleteObject(font); if(bitmap)DeleteObject(bitmap); if(dc)DeleteDC(dc); return false; }
    auto old_bitmap=SelectObject(dc,bitmap); auto old_font=SelectObject(dc,font);
    SetTextColor(dc,RGB(255,255,255)); SetBkMode(dc,TRANSPARENT);
    constexpr const wchar_t* labels[]{
        L"EXPERIMENTAL NOCLIP: UNAVAILABLE\nWaiting for player and mod heartbeat\nDetails: crml/crml.log and movement-probe.jsonl",
        L"EXPERIMENTAL NOCLIP: OFF   [F6] on\nWASD: camera heading   Space/Ctrl: up/down\nShift: faster   Esc: off",
        L"EXPERIMENTAL NOCLIP: ON   [F6] off\nWASD: camera heading   Space/Ctrl: up/down\nShift: faster   Esc: off",
        L"EXPERIMENTAL NOCLIP: OFF   [F6] on\nCamera unavailable: vertical movement only\nSpace/Ctrl: up/down   Shift: faster   Esc: off",
        L"EXPERIMENTAL NOCLIP: ON   [F6] off\nCamera unavailable: vertical movement only\nSpace/Ctrl: up/down   Shift: faster   Esc: off"};
    for(int state_index=0;state_index<5;++state_index) {
        PatBlt(dc,0,0,width,height,BLACKNESS);
        RECT rect{12,9,width-12,height-9}; DrawTextW(dc,labels[state_index],-1,&rect,DT_LEFT|DT_NOPREFIX); GdiFlush();
        auto* data=static_cast<const unsigned*>(pixels);
        s.text[state_index].resize(width*height);
        for(int y=0;y<height;++y) for(int x=0;x<width;++x)
            s.text[state_index][y*width+x]=(data[y*width+x]&0xffffff)?1:0;
    }
    SelectObject(dc,old_font); SelectObject(dc,old_bitmap); DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
    return true;
}
}

void* overlay_create() noexcept {
    try {
        Internal guard;
        auto& s=state();
        if(enabled) return &s;
        diagnostic="hook_initialization_failed";
        if(!rasterize(s)) return nullptr;
        const auto init=MH_Initialize(); if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return nullptr;
        ComPtr<ID3D12Device> device; ComPtr<IDXGIFactory4> factory; ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
        if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
        D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue))) ||
           FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))) return nullptr;
        list->Close();
        auto window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"CRML graphics probe",WS_POPUP,0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window) return nullptr;
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=8; desc.Height=8; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swap;
        const auto created=factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swap);
        DestroyWindow(window);
        if(FAILED(created)) return nullptr;
        ComPtr<IDXGISwapChain3> swap3; if(FAILED(swap.As(&swap3))) return nullptr;
        auto** sv=*reinterpret_cast<void***>(swap3.Get()); auto** qv=*reinterpret_cast<void***>(queue.Get()); auto** lv=*reinterpret_cast<void***>(list.Get());
        struct Hook {void* target; void* hook; void** original;};
        std::vector<Hook> hooks{
            {sv[8],reinterpret_cast<void*>(&present_hook),reinterpret_cast<void**>(&original_present)},
            {sv[22],reinterpret_cast<void*>(&present1_hook),reinterpret_cast<void**>(&original_present1)},
            {sv[13],reinterpret_cast<void*>(&resize_hook),reinterpret_cast<void**>(&original_resize)},
            {sv[39],reinterpret_cast<void*>(&resize1_hook),reinterpret_cast<void**>(&original_resize1)},
            {qv[10],reinterpret_cast<void*>(&execute_hook),reinterpret_cast<void**>(&original_execute)},
            {lv[10],reinterpret_cast<void*>(&reset_hook),reinterpret_cast<void**>(&original_reset)},
            {lv[26],reinterpret_cast<void*>(&barrier_hook),reinterpret_cast<void**>(&original_barrier)}};
        ComPtr<ID3D12GraphicsCommandList7> list7;
        if(SUCCEEDED(list.As(&list7))) hooks.push_back({(*reinterpret_cast<void***>(list7.Get()))[80],reinterpret_cast<void*>(&enhanced_hook),reinterpret_cast<void**>(&original_enhanced)});
        size_t installed=0;
        for(const auto& h:hooks) {
            if(MH_CreateHook(h.target,h.hook,h.original)!=MH_OK) break;
            ++installed;
        }
        if(installed!=hooks.size()) { for(size_t i=0;i<installed;++i) MH_RemoveHook(hooks[i].target); return nullptr; }
        for(const auto& h:hooks) MH_QueueEnableHook(h.target);
        if(MH_ApplyQueued()!=MH_OK) {
            for(const auto& h:hooks) MH_DisableHook(h.target);
            for(const auto& h:hooks) MH_RemoveHook(h.target);
            return nullptr;
        }
        enabled=true; diagnostic="waiting_for_present"; return &s;
    } catch(...) { diagnostic="initialization_exception"; return nullptr; }
}
void overlay_update(void*,bool show,int value,bool camera_valid) noexcept { status=!camera_valid && (value==0 || value==1)?value+2:value; visible=show; }
void overlay_destroy(void*) noexcept { visible=false; enabled=false; }
OverlayDiagnostics overlay_diagnostics() noexcept { return {presents.load(),frames.load(),queue_matches.load(),diagnostic.load()}; }
}
