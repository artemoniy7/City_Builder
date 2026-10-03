#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace city {

class Renderer final {
public:
    explicit Renderer(HWND window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Render();
    void Resize(UINT width, UINT height);

private:
    static constexpr UINT kFrameCount = 2;

    struct Vertex { float position[3]; float color[3]; };
    struct alignas(256) SceneConstants { float mvp[16]; };

    void CreateDeviceResources();
    void CreateWindowResources(UINT width, UINT height);
    void CreateAssets();
    void WaitForGpu();
    void MoveToNextFrame();
    void ThrowIfFailed(HRESULT result) const;

    HWND window_{};
    UINT width_{1280};
    UINT height_{720};
    UINT frameIndex_{};
    UINT rtvDescriptorSize_{};
    UINT64 fenceValue_{};
    std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> renderTargets_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    SceneConstants* mappedConstants_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_{};
};

} // namespace city
