#include "D3D12Engine.h"
#include <d3dcompiler.h>
#include <cassert>
#include <cstring>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ============================================================
// Init
// ============================================================
void D3D12Engine::Init(HWND hwnd, UINT width, UINT height)
{
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif

    CreateDevice();
    CreateCommandQueue();
    CreateSwapChain(hwnd);
    CreateRenderTargets();
    CreateRootSignature();
    CompileShaders();
    CreatePipelineState();
    CreateCubeGeometry();   // ← добавили

    m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator));

    m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), m_pipelineState.Get(),
        IID_PPV_ARGS(&m_commandList));
    m_commandList->Close();

    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    m_fenceValue = 0;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

// ============================================================
// Создание устройства
// ============================================================
void D3D12Engine::CreateDevice()
{
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&m_device))))
            break;
    }
    assert(m_device != nullptr);
}

// ============================================================
// Командная очередь
// ============================================================
void D3D12Engine::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue));
}

// ============================================================
// Swap Chain
// ============================================================
void D3D12Engine::CreateSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount = FrameCount;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), hwnd, &desc,
        nullptr, nullptr, &swapChain);

    swapChain.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ============================================================
// Render Targets
// ============================================================
void D3D12Engine::CreateRenderTargets()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = FrameCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap));

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < FrameCount; i++)
    {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

// ============================================================
// Root Signature
// ============================================================
void D3D12Engine::CreateRootSignature()
{
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 0;
    desc.pParameters = nullptr;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signature, &error);
    if (FAILED(hr)) {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        assert(false);
    }
    m_device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
}

// ============================================================
// CompileShaders (заглушка)
// ============================================================
void D3D12Engine::CompileShaders()
{
}

// ============================================================
// Pipeline State Object
// ============================================================
void D3D12Engine::CreatePipelineState()
{
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompileFromFile(
        L"../shaders/basic.vs.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_1", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            fprintf(stderr, "VS compile error:\n%s\n", (char*)errorBlob->GetBufferPointer());
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        } else {
            fprintf(stderr, "VS file not found (hr=0x%08X)\n", hr);
        }
        assert(false && "Vertex shader compilation failed");
    }

    hr = D3DCompileFromFile(
        L"../shaders/basic.ps.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_1", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            fprintf(stderr, "PS compile error:\n%s\n", (char*)errorBlob->GetBufferPointer());
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        } else {
            fprintf(stderr, "PS file not found (hr=0x%08X)\n", hr);
        }
        assert(false && "Pixel shader compilation failed");
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rastDesc.CullMode = D3D12_CULL_MODE_BACK;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rastDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rastDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rastDesc.DepthClipEnable = TRUE;
    rastDesc.AntialiasedLineEnable = FALSE;
    rastDesc.ForcedSampleCount = 0;
    rastDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].LogicOpEnable = FALSE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = rastDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
}

// ============================================================
// Геометрия куба
// ============================================================
void D3D12Engine::CreateCubeGeometry()
{
    struct Vertex
    {
        float position[3];
        float color[4];
    };

    // 8 вершин куба, но у каждой грани свой цвет.
    // Чтобы каждая грань имела однотонный цвет, вершины дублируются по граням — 24 вершины.
    // Порядок обхода каждой грани — против часовой стрелки (лицевая сторона для D3D12).

    Vertex vertices[] =
    {
        // Передняя грань (z = +0.5) — красный
        { { -0.5f, -0.5f,  0.5f }, { 1, 0, 0, 1 } },
        { { -0.5f,  0.5f,  0.5f }, { 1, 0, 0, 1 } },
        { {  0.5f,  0.5f,  0.5f }, { 1, 0, 0, 1 } },
        { {  0.5f, -0.5f,  0.5f }, { 1, 0, 0, 1 } },

        // Задняя грань (z = -0.5) — зелёный
        { {  0.5f, -0.5f, -0.5f }, { 0, 1, 0, 1 } },
        { {  0.5f,  0.5f, -0.5f }, { 0, 1, 0, 1 } },
        { { -0.5f,  0.5f, -0.5f }, { 0, 1, 0, 1 } },
        { { -0.5f, -0.5f, -0.5f }, { 0, 1, 0, 1 } },

        // Левая грань (x = -0.5) — синий
        { { -0.5f, -0.5f, -0.5f }, { 0, 0, 1, 1 } },
        { { -0.5f,  0.5f, -0.5f }, { 0, 0, 1, 1 } },
        { { -0.5f,  0.5f,  0.5f }, { 0, 0, 1, 1 } },
        { { -0.5f, -0.5f,  0.5f }, { 0, 0, 1, 1 } },

        // Правая грань (x = +0.5) — жёлтый
        { {  0.5f, -0.5f,  0.5f }, { 1, 1, 0, 1 } },
        { {  0.5f,  0.5f,  0.5f }, { 1, 1, 0, 1 } },
        { {  0.5f,  0.5f, -0.5f }, { 1, 1, 0, 1 } },
        { {  0.5f, -0.5f, -0.5f }, { 1, 1, 0, 1 } },

        // Верхняя грань (y = +0.5) — голубой
        { { -0.5f,  0.5f,  0.5f }, { 0, 1, 1, 1 } },
        { { -0.5f,  0.5f, -0.5f }, { 0, 1, 1, 1 } },
        { {  0.5f,  0.5f, -0.5f }, { 0, 1, 1, 1 } },
        { {  0.5f,  0.5f,  0.5f }, { 0, 1, 1, 1 } },

        // Нижняя грань (y = -0.5) — пурпурный
        { { -0.5f, -0.5f, -0.5f }, { 1, 0, 1, 1 } },
        { { -0.5f, -0.5f,  0.5f }, { 1, 0, 1, 1 } },
        { {  0.5f, -0.5f,  0.5f }, { 1, 0, 1, 1 } },
        { {  0.5f, -0.5f, -0.5f }, { 1, 0, 1, 1 } },
    };

    m_vertexCount = _countof(vertices);
    const UINT vertexBufferSize = sizeof(vertices);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0;
    bufferDesc.Width = vertexBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.SampleDesc.Quality = 0;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_vertexBuffer));

    void* mappedData = nullptr;
    m_vertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices, vertexBufferSize);
    m_vertexBuffer->Unmap(0, nullptr);

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    m_vertexBufferView.SizeInBytes = vertexBufferSize;
}

// ============================================================
// Render — рисуем кадр
// ============================================================
void D3D12Engine::Render()
{
    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_frameIndex * m_rtvDescriptorSize;

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    const float clearColor[] = { 0.1f, 0.2f, 0.4f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissorRect);

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->DrawInstanced(m_vertexCount, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();

    ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, cmdLists);

    m_swapChain->Present(1, 0);

    m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ============================================================
// Shutdown
// ============================================================
void D3D12Engine::Shutdown()
{
    m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    CloseHandle(m_fenceEvent);
}