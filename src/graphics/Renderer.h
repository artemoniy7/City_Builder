#pragma once

#include "math/Math.h"

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
    void OnMouseWheel(short delta);

private:
    static constexpr UINT kFrameCount = 2;
    static constexpr UINT kShadowMapSize = 2048;
    static constexpr UINT kTerrainResolution = 256;

    struct Vertex { float position[3]; float color[3]; float normal[3]; };
    struct alignas(256) SceneConstants {
        float viewProjection[16];
        float lightViewProjection[16];
        float lightDirection[3];
        float padding{};
        float timeSeconds{};
        float waterPadding{};
        float cameraPosition[3]{};
        float cameraPadding{};
    };

    void CreateDeviceResources();
    void CreateWindowResources(UINT width, UINT height);
    void CreateAssets();
    void UpdateCamera(float deltaSeconds);
    void WaitForGpu();
    void MoveToNextFrame();
    void ThrowIfFailed(HRESULT result) const;

    HWND window_{};
    UINT width_{1280};
    UINT height_{720};
    UINT frameIndex_{};
    UINT rtvDescriptorSize_{};
    UINT64 fenceValue_{};
    std::chrono::steady_clock::time_point lastFrameAt_{std::chrono::steady_clock::now()};
    math::Vector3 cameraPosition_{0.0f, 10.0f, -16.0f};
    math::Vector3 cameraTarget_{0.0f, 0.0f, 0.0f};
    float cameraOrbitDistance_{1200.0f};
    float cameraYaw_{0.0f};
    float cameraPitch_{-0.576f};
    float fieldOfView_{0.95f};
    bool topDownView_{};
    bool spaceWasDown_{};

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shadowDsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shaderResourceHeap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> renderTargets_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> waterPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> waterVertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> shadowMap_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    D3D12_VERTEX_BUFFER_VIEW waterVertexBufferView_{};
    UINT vertexCount_{};
    UINT waterVertexCount_{};
    SceneConstants* mappedConstants_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_{};
};

} // namespace city
