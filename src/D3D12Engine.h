#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>

using Microsoft::WRL::ComPtr;

// A deliberately small DX12 renderer used as the engine's graphics smoke test.
// It owns all GPU objects needed to display a lit, rotating cube.
class D3D12Engine
{
public:
    void Init(HWND hwnd, UINT width, UINT height);
    void Render();
    void Shutdown();

private:
    struct SceneConstants
    {
        float worldViewProjection[16];
        float world[16];
        float lightDirection[4];
    };

    void CreateDevice();
    void CreateCommandQueue();
    void CreateSwapChain(HWND hwnd);
    void CreateRenderTargets();
    void CreateDepthStencil();
    void CreateRootSignature();
    void CreatePipelineState();
    void CreateCubeGeometry();
    void CreateConstantBuffer();
    void UpdateSceneConstants();
    void WaitForGpu();

    static const UINT FrameCount = 2;
    static const UINT ConstantBufferSize = 256; // D3D12 CBVs require 256-byte alignment.

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_depthStencil;

    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    UINT m_vertexCount = 0;

    ComPtr<ID3D12Resource> m_constantBuffer;
    SceneConstants* m_mappedConstants = nullptr;

    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    std::chrono::steady_clock::time_point m_startTime;
    UINT m_rtvDescriptorSize = 0;
    UINT m_frameIndex = 0;
};
