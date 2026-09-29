#include "overlay.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>

using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("DirectX failure: "+std::to_string(static_cast<unsigned>(hr))); }
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
decltype(&D3D12CreateDevice) create_device_original{};
decltype(&CreateDXGIFactory1) create_factory_original{};
std::atomic<unsigned> devices_created{}, factories_created{}, engine_presents{};
HRESULT WINAPI count_device(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** result) {
    ++devices_created; return create_device_original(adapter,level,iid,result);
}
HRESULT WINAPI count_factory(REFIID iid,void** result) {
    ++factories_created; return create_factory_original(iid,result);
}
struct EngineImplementation { uintptr_t reserved{}; IDXGISwapChain* swap{}; };
struct EngineOwner { EngineImplementation* implementation; bool present1; };
__declspec(noinline) void engine_present(void* address) {
    auto& owner=*static_cast<EngineOwner*>(address);
    ++engine_presents;
    if(owner.present1) {
        ComPtr<IDXGISwapChain1> swap; check(owner.implementation->swap->QueryInterface(IID_PPV_ARGS(&swap)));
        DXGI_PRESENT_PARAMETERS params{}; check(swap->Present1(0,0,&params));
    } else check(owner.implementation->swap->Present(0,0));
}
// A proxy that exposes the documented Streamline native-interface query. Its
// graphics methods deliberately refuse use by the overlay; only engine Present
// forwards. This checks attachment/rendering against the underlying real chain.
class NativeSwapProxy final : public IDXGISwapChain {
    ComPtr<IDXGISwapChain> target_;
    std::atomic<ULONG> references_{1};
public:
    unsigned native_queries{}, unexpected_calls{};
    explicit NativeSwapProxy(IDXGISwapChain* target):target_(target){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER;
        *out=nullptr;
        const GUID native_id{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
        if(iid==native_id) { ++native_queries; return target_->QueryInterface(__uuidof(IDXGISwapChain),out); }
        if(iid==__uuidof(IUnknown) || iid==__uuidof(IDXGISwapChain)) { *out=static_cast<IDXGISwapChain*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=--references_; if(!n) delete this; return n; }
    HRESULT reject() { ++unexpected_calls; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID,void**) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override { return reject(); }
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override { return target_->Present(sync,flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT,REFIID,void**) override { return reject(); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL,IDXGIOutput*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*,IDXGIOutput**) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT,UINT,UINT,DXGI_FORMAT,UINT) override { return reject(); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return reject(); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return reject(); }
};
struct Fixture {
    HWND window{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue,unrelated;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<NativeSwapProxy> proxy;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<ComPtr<ID3D12Resource>,2> buffers;
    UINT64 serial{};
    UINT width=640,height=360,stride{};
    Fixture() {
        if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) {
            ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
            check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        }
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&unrelated)));
        check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list))); check(list->Close());
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"CRML render test",WS_POPUP,0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"Test window creation failed");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=width; desc.Height=height; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> first; check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&first)); check(first.As(&swap));
        D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; h.NumDescriptors=2;
        check(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap))); stride=device->GetDescriptorHandleIncrementSize(h.Type);
        get_buffers();
    }
    void get_buffers() {
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<2;++i) { check(swap->GetBuffer(i,IID_PPV_ARGS(&buffers[i]))); device->CreateRenderTargetView(buffers[i].Get(),nullptr,handle); handle.ptr+=stride; }
    }
    void wait() {
        check(queue->Signal(fence.Get(),++serial));
        if(fence->GetCompletedValue()<serial) {
            auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"Fence event failed");
            check(fence->SetEventOnCompletion(serial,event)); auto result=WaitForSingleObject(event,5000); CloseHandle(event);
            require(result==WAIT_OBJECT_0,"GPU did not complete");
        }
    }
    void begin() { wait(); check(allocator->Reset()); check(list->Reset(allocator.Get(),nullptr)); }
    void submit() { check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists); }
    void transition(ID3D12Resource* buffer,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={buffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
    }
    UINT draw(bool enhanced=false,bool present1=false) {
        const auto index=swap->GetCurrentBackBufferIndex(); begin();
        ComPtr<ID3D12GraphicsCommandList7> list7;
        if(enhanced) check(list.As(&list7));
        auto barrier=[&](bool finish) {
            if(!enhanced) { transition(buffers[index].Get(),finish?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_PRESENT,finish?D3D12_RESOURCE_STATE_PRESENT:D3D12_RESOURCE_STATE_RENDER_TARGET); return; }
            D3D12_TEXTURE_BARRIER b{}; b.pResource=buffers[index].Get(); b.Subresources.IndexOrFirstMipLevel=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.SyncBefore=finish?D3D12_BARRIER_SYNC_RENDER_TARGET:D3D12_BARRIER_SYNC_NONE;
            b.SyncAfter=finish?D3D12_BARRIER_SYNC_NONE:D3D12_BARRIER_SYNC_RENDER_TARGET;
            b.AccessBefore=finish?D3D12_BARRIER_ACCESS_RENDER_TARGET:D3D12_BARRIER_ACCESS_NO_ACCESS;
            b.AccessAfter=finish?D3D12_BARRIER_ACCESS_NO_ACCESS:D3D12_BARRIER_ACCESS_RENDER_TARGET;
            b.LayoutBefore=finish?D3D12_BARRIER_LAYOUT_RENDER_TARGET:D3D12_BARRIER_LAYOUT_PRESENT;
            b.LayoutAfter=finish?D3D12_BARRIER_LAYOUT_PRESENT:D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            D3D12_BARRIER_GROUP group{}; group.Type=D3D12_BARRIER_TYPE_TEXTURE; group.NumBarriers=1; group.pTextureBarriers=&b; list7->Barrier(1,&group);
        };
        barrier(false); auto rtv=heap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=index*stride;
        const float color[]{.2f,.1f,.05f,1}; list->ClearRenderTargetView(rtv,color,0,nullptr); barrier(true); submit();
        // Unrelated queue submissions must not replace the queue associated with our buffer.
        ComPtr<ID3D12CommandAllocator> other_allocator;
        ComPtr<ID3D12GraphicsCommandList> other_list;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&other_allocator)));
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,other_allocator.Get(),nullptr,IID_PPV_ARGS(&other_list)));
        check(other_list->Close());
        ID3D12CommandList* other_lists[]{other_list.Get()}; unrelated->ExecuteCommandLists(1,other_lists);
        check(unrelated->Signal(fence.Get(),++serial)); check(queue->Wait(fence.Get(),serial));
        EngineImplementation implementation{0,proxy && !present1?static_cast<IDXGISwapChain*>(proxy.Get()):swap.Get()}; EngineOwner owner{&implementation,present1};
        const auto calls=engine_presents.load();
        engine_present(&owner);
        require(engine_presents==calls+1,"Overlay did not forward exactly one engine present");
        wait(); return index;
    }
    void verify(UINT index,bool panel) {
        auto desc=buffers[index]->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES properties{}; properties.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC resource{}; resource.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; resource.Width=bytes;
        resource.Height=1; resource.DepthOrArraySize=1; resource.MipLevels=1; resource.SampleDesc.Count=1; resource.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback; check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        begin(); transition(buffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=buffers[index].Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); transition(buffers[index].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT); submit(); wait();
        void* mapped{}; D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)}; check(readback->Map(0,&range,&mapped));
        auto* data=static_cast<const unsigned char*>(mapped);
        auto pixel=[&](UINT x,UINT y){return data+y*footprint.Footprint.RowPitch+x*4;};
        bool outside=pixel(0,0)[0]==51;
        bool background=panel ? pixel(24,24)[0]==24 : pixel(24,24)[0]==51;
        unsigned text_pixels=0;
        for(UINT y=30;y<95;++y) for(UINT x=32;x<520;++x) if(pixel(x,y)[0]>180 && pixel(x,y)[2]>180) ++text_pixels;
        if(panel) {
            std::ofstream ppm("overlay-test.ppm",std::ios::binary); ppm << "P6\n" << width << ' ' << height << "\n255\n";
            for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) ppm.write(reinterpret_cast<const char*>(pixel(x,y)),3);
        }
        D3D12_RANGE written{0,0}; readback->Unmap(0,&written);
        require(outside,"Overlay modified pixels outside its panel"); require(background,"Panel background missing from actual swapchain image");
        require(panel?text_pixels>300:text_pixels==0,"Text mask visibility does not match overlay state");
    }
    void resize(bool version1) {
        wait(); buffers={}; width=800;height=450;
        if(version1) { const UINT masks[]{1,1}; IUnknown* queues[]{queue.Get(),queue.Get()}; check(swap->ResizeBuffers1(2,width,height,DXGI_FORMAT_UNKNOWN,0,masks,queues)); }
        else check(swap->ResizeBuffers(2,width,height,DXGI_FORMAT_UNKNOWN,0));
        get_buffers();
    }
    ~Fixture() { if(window)DestroyWindow(window); }
};
int main(int argc,char** argv) {
    try {
        bool early=false, physics=false, wasm=false, movement=false, proxy=false;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            early|=arg=="--early"; physics|=arg=="--physics-trial";
            wasm|=arg=="--wasm"; movement|=arg=="--movement";
            proxy|=arg=="--native-proxy";
        }
        require(MH_Initialize()==MH_OK,"Hook setup failed");
        require(MH_CreateHook(reinterpret_cast<void*>(&D3D12CreateDevice),reinterpret_cast<void*>(&count_device),reinterpret_cast<void**>(&create_device_original))==MH_OK,"Device counter hook failed");
        require(MH_CreateHook(reinterpret_cast<void*>(&CreateDXGIFactory1),reinterpret_cast<void*>(&count_factory),reinterpret_cast<void**>(&create_factory_original))==MH_OK,"Factory counter hook failed");
        require(MH_EnableHook(reinterpret_cast<void*>(&D3D12CreateDevice))==MH_OK && MH_EnableHook(reinterpret_cast<void*>(&CreateDXGIFactory1))==MH_OK,"Counter enable failed");
        // Reject an unauthenticated/unmapped engine entry without touching graphics.
        require(crml::probe::overlay_create(1)==nullptr,"Invalid engine entry accepted");
        require(!devices_created && !factories_created,"Overlay startup created a graphics device or factory");
        void* overlay{};
        if(early) {
            overlay=crml::probe::overlay_create(0,physics,wasm,movement);
            require(overlay!=nullptr,"Early overlay setup failed");
            require(!devices_created && !factories_created,"Early startup initialized graphics");
            require(std::string(crml::probe::overlay_diagnostics().status)=="waiting_for_game_swapchain","Early overlay did not wait for graphics");
        }
        Fixture f;
        if(proxy) f.proxy.Attach(new NativeSwapProxy(f.swap.Get()));
        f.draw(); // Attachment also works after frames have already been presented.
        const auto device_count=devices_created.load(), factory_count=factories_created.load();
        if(!overlay) overlay=crml::probe::overlay_create(0,physics,wasm,movement);
        require(overlay!=nullptr,"Overlay setup failed");
        require(!crml::probe::overlay_attach(nullptr),"Null swapchain accepted");
        require(crml::probe::overlay_test_bind_present(reinterpret_cast<void*>(&engine_present)),"Engine present hook failed");
        crml::probe::overlay_update(overlay,true,0);
        UINT index{};
        for(int i=0;i<5;++i) index=f.draw(); f.verify(index,true);
        if(physics) for(int state=-1;state<=8;++state) {
            crml::probe::overlay_update(overlay,true,state); index=f.draw(); f.verify(index,true);
        }
        crml::probe::overlay_update(overlay,true,1,false); index=f.draw(); f.verify(index,true);
        crml::probe::overlay_update(overlay,true,0);
        // A held Insert press toggles once; subsequent gameplay updates do not
        // reopen the panel. Focus loss and refocus preserve the preference.
        crml::probe::overlay_update(overlay,true,1,true,true); index=f.draw(); f.verify(index,false);
        crml::probe::overlay_update(overlay,true,0,true,true); index=f.draw(); f.verify(index,false);
        crml::probe::overlay_update(overlay,false,0); index=f.draw(); f.verify(index,false);
        crml::probe::overlay_update(overlay,true,1); index=f.draw(); f.verify(index,false);
        crml::probe::overlay_update(overlay,true,1,true,true); index=f.draw(); f.verify(index,true);
        crml::probe::overlay_update(overlay,true,1);
        crml::probe::overlay_update(overlay,false,1,true,true);
        crml::probe::overlay_update(overlay,true,1,true,true); index=f.draw(); f.verify(index,true);
        crml::probe::overlay_update(overlay,true,0);
        const auto before=crml::probe::overlay_diagnostics().frames;
        check(f.swap->Present(0,DXGI_PRESENT_TEST));
        require(crml::probe::overlay_diagnostics().frames==before,"TEST present submitted GPU work");
        crml::probe::overlay_update(overlay,false,0); index=f.draw(); f.verify(index,false);
        crml::probe::overlay_update(overlay,true,1);
        f.resize(false); for(int i=0;i<5;++i) index=f.draw(false,true); f.verify(index,true);
        f.resize(true); for(int i=0;i<5;++i) index=f.draw(); f.verify(index,true);
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
        if(SUCCEEDED(f.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&options,sizeof(options))) && options.EnhancedBarriersSupported) {
            for(int i=0;i<5;++i) index=f.draw(true); f.verify(index,true);
        }
        crml::probe::overlay_destroy(overlay); index=f.draw(); f.verify(index,false);
        f.resize(false); // Cleanup must release overlay references even after shutdown.
        const auto result=crml::probe::overlay_diagnostics();
        require(result.frames>=10 && result.queue_matches>=10,"Expected actual GPU overlay submissions");
        require(devices_created==device_count && factories_created==factory_count,"Overlay created an additional device or factory during attachment/rendering");
        if(proxy) require(f.proxy->native_queries && !f.proxy->unexpected_calls,"Overlay used proxy graphics methods instead of the native swapchain");
        std::cout << "DX12 backbuffer pixels, engine present attachment, startup without graphics creation, Present/Present1, both resize paths, queue tracking, visibility and shutdown passed; " << result.frames << " rendered frames\n";
    } catch(const std::exception& e) { std::cerr << e.what() << "; overlay=" << crml::probe::overlay_diagnostics().status << '\n'; return 1; }
}
