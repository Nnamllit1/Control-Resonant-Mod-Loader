#include "overlay.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("DirectX failure: "+std::to_string(static_cast<unsigned>(hr))); }
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
struct Fixture {
    HWND window{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue,unrelated;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<ComPtr<ID3D12Resource>,2> buffers;
    UINT64 serial{};
    UINT width=640,height=360,stride{};
    Fixture() {
        check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
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
        if(present1) { DXGI_PRESENT_PARAMETERS params{}; check(swap->Present1(0,0,&params)); }
        else check(swap->Present(0,0));
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
        bool background=panel ? pixel(21,21)[0]<10 : pixel(21,21)[0]==51;
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
int main() {
    try {
        Fixture f;
        f.draw(); // Graphics already initialized before the loader starts.
        auto overlay=crml::probe::overlay_create(); require(overlay!=nullptr,"Graphics hook initialization failed");
        crml::probe::overlay_update(overlay,true,0);
        UINT index{};
        for(int i=0;i<5;++i) index=f.draw(); f.verify(index,true);
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
        std::cout << "DX12 backbuffer pixels, late startup, Present/Present1, both resize paths, queue tracking, visibility and shutdown passed; " << result.frames << " rendered frames\n";
    } catch(const std::exception& e) { std::cerr << e.what() << "; overlay=" << crml::probe::overlay_diagnostics().status << '\n'; return 1; }
}
