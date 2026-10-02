#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

class D3D12Engine
{
public:
    void Init(HWND hwnd, UINT width, UINT height);
    void Render();
    void Shutdown();

private:
    void CreateDevice();
    void CreateCommandQueue();
    void CreateSwapChain(HWND hwnd);
    void CreateRenderTargets();
    void CreateRootSignature();
    void CreatePipelineState();
    void CompileShaders();
    void CreateCubeGeometry();

    static const UINT FrameCount = 2;

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // Основные объекты D3D12
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];

    // Командные объекты
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // Пайплайн
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    // Геометрия
    ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    UINT m_vertexCount = 0;

    // Синхронизация
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    UINT m_rtvDescriptorSize = 0;
    UINT m_frameIndex = 0;
};