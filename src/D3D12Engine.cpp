#include "D3D12Engine.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

namespace
{
    void CheckHr(HRESULT result, const char* operation)
    {
        if (FAILED(result)) {
            std::fprintf(stderr, "%s failed (hr=0x%08X)\n", operation, static_cast<unsigned int>(result));
            assert(false && "Direct3D 12 operation failed");
        }
    }

    std::wstring FindShader(const wchar_t* fileName)
    {
        const std::wstring paths[] = {
            std::wstring(L"shaders/") + fileName,
            std::wstring(L"../shaders/") + fileName,
        };

        for (const std::wstring& path : paths) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return path;
            }
        }
        return paths[0];
    }
}

void D3D12Engine::Init(HWND hwnd, UINT width, UINT height)
{
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    m_startTime = std::chrono::steady_clock::now();

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
    CreateDepthStencil();
    CreateRootSignature();
    CreatePipelineState();
    CreateCubeGeometry();
    CreateConstantBuffer();

    CheckHr(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)), "CreateCommandAllocator");
    CheckHr(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)), "CreateCommandList");
    CheckHr(m_commandList->Close(), "Close initial command list");

    CheckHr(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    assert(m_fenceEvent != nullptr);
}

void D3D12Engine::CreateDevice()
{
    ComPtr<IDXGIFactory6> factory;
    CheckHr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description = {};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) {
            break;
        }
    }
    assert(m_device != nullptr);
}

void D3D12Engine::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC description = {};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CheckHr(m_device->CreateCommandQueue(&description, IID_PPV_ARGS(&m_commandQueue)), "CreateCommandQueue");
}

void D3D12Engine::CreateSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    DXGI_SWAP_CHAIN_DESC1 description = {};
    description.BufferCount = FrameCount;
    description.Width = m_width;
    description.Height = m_height;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    CheckHr(factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &description, nullptr, nullptr, &swapChain),
        "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CheckHr(swapChain.As(&m_swapChain), "Query IDXGISwapChain3");
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Engine::CreateRenderTargets()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription = {};
    heapDescription.NumDescriptors = FrameCount;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    CheckHr(m_device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV heap");

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < FrameCount; ++index) {
        CheckHr(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_renderTargets[index])), "Get swap-chain buffer");
        m_device->CreateRenderTargetView(m_renderTargets[index].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }
}

void D3D12Engine::CreateDepthStencil()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription = {};
    heapDescription.NumDescriptors = 1;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CheckHr(m_device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_dsvHeap)), "Create DSV heap");

    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture = {};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = m_width;
    texture.Height = m_height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_D32_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    CheckHr(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&m_depthStencil)), "Create depth buffer");
    m_device->CreateDepthStencilView(m_depthStencil.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Engine::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description = {};
    description.NumParameters = 1;
    description.pParameters = &parameter;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
    if (FAILED(result) && errors) {
        OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    }
    CheckHr(result, "D3D12SerializeRootSignature");
    CheckHr(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");
}

void D3D12Engine::CreatePipelineState()
{
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3DCompileFromFile(FindShader(L"basic.vs.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain", "vs_5_1", 0, 0, &vertexShader, &errors);
    if (FAILED(result) && errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    CheckHr(result, "Compile vertex shader");
    errors.Reset();
    result = D3DCompileFromFile(FindShader(L"basic.ps.hlsl").c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain", "ps_5_1", 0, 0, &pixelShader, &errors);
    if (FAILED(result) && errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    CheckHr(result, "Compile pixel shader");

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description = {};
    description.InputLayout = { inputLayout, _countof(inputLayout) };
    description.pRootSignature = m_rootSignature.Get();
    description.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    description.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.SampleMask = UINT_MAX;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    CheckHr(m_device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&m_pipelineState)), "CreateGraphicsPipelineState");
}

void D3D12Engine::CreateCubeGeometry()
{
    struct Vertex { float position[3]; float normal[3]; float color[4]; };
    const Vertex vertices[] = {
        // Each face has its own normal and colour; two triangles per face.
        {{-1,-1, 1},{ 0, 0, 1},{1,0.22f,0.18f,1}}, {{-1, 1, 1},{ 0, 0, 1},{1,0.22f,0.18f,1}}, {{ 1, 1, 1},{ 0, 0, 1},{1,0.22f,0.18f,1}}, {{-1,-1, 1},{ 0, 0, 1},{1,0.22f,0.18f,1}}, {{ 1, 1, 1},{ 0, 0, 1},{1,0.22f,0.18f,1}}, {{ 1,-1, 1},{ 0, 0, 1},{1,0.22f,0.18f,1}},
        {{ 1,-1,-1},{ 0, 0,-1},{0.18f,0.7f,0.32f,1}}, {{ 1, 1,-1},{ 0, 0,-1},{0.18f,0.7f,0.32f,1}}, {{-1, 1,-1},{ 0, 0,-1},{0.18f,0.7f,0.32f,1}}, {{ 1,-1,-1},{ 0, 0,-1},{0.18f,0.7f,0.32f,1}}, {{-1, 1,-1},{ 0, 0,-1},{0.18f,0.7f,0.32f,1}}, {{-1,-1,-1},{ 0, 0,-1},{0.18f,0.7f,0.32f,1}},
        {{-1,-1,-1},{-1, 0, 0},{0.2f,0.45f,1,1}}, {{-1, 1,-1},{-1, 0, 0},{0.2f,0.45f,1,1}}, {{-1, 1, 1},{-1, 0, 0},{0.2f,0.45f,1,1}}, {{-1,-1,-1},{-1, 0, 0},{0.2f,0.45f,1,1}}, {{-1, 1, 1},{-1, 0, 0},{0.2f,0.45f,1,1}}, {{-1,-1, 1},{-1, 0, 0},{0.2f,0.45f,1,1}},
        {{ 1,-1, 1},{ 1, 0, 0},{1,0.76f,0.16f,1}}, {{ 1, 1, 1},{ 1, 0, 0},{1,0.76f,0.16f,1}}, {{ 1, 1,-1},{ 1, 0, 0},{1,0.76f,0.16f,1}}, {{ 1,-1, 1},{ 1, 0, 0},{1,0.76f,0.16f,1}}, {{ 1, 1,-1},{ 1, 0, 0},{1,0.76f,0.16f,1}}, {{ 1,-1,-1},{ 1, 0, 0},{1,0.76f,0.16f,1}},
        {{-1, 1, 1},{ 0, 1, 0},{0.15f,0.85f,0.85f,1}}, {{-1, 1,-1},{ 0, 1, 0},{0.15f,0.85f,0.85f,1}}, {{ 1, 1,-1},{ 0, 1, 0},{0.15f,0.85f,0.85f,1}}, {{-1, 1, 1},{ 0, 1, 0},{0.15f,0.85f,0.85f,1}}, {{ 1, 1,-1},{ 0, 1, 0},{0.15f,0.85f,0.85f,1}}, {{ 1, 1, 1},{ 0, 1, 0},{0.15f,0.85f,0.85f,1}},
        {{-1,-1,-1},{ 0,-1, 0},{0.82f,0.2f,0.85f,1}}, {{-1,-1, 1},{ 0,-1, 0},{0.82f,0.2f,0.85f,1}}, {{ 1,-1, 1},{ 0,-1, 0},{0.82f,0.2f,0.85f,1}}, {{-1,-1,-1},{ 0,-1, 0},{0.82f,0.2f,0.85f,1}}, {{ 1,-1, 1},{ 0,-1, 0},{0.82f,0.2f,0.85f,1}}, {{ 1,-1,-1},{ 0,-1, 0},{0.82f,0.2f,0.85f,1}},
    };
    m_vertexCount = _countof(vertices);
    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(vertices);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CheckHr(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vertexBuffer)), "Create vertex buffer");
    void* data = nullptr;
    CheckHr(m_vertexBuffer->Map(0, nullptr, &data), "Map vertex buffer");
    std::memcpy(data, vertices, sizeof(vertices));
    m_vertexBuffer->Unmap(0, nullptr);
    m_vertexBufferView = { m_vertexBuffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Vertex) };
}

void D3D12Engine::CreateConstantBuffer()
{
    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = ConstantBufferSize;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CheckHr(m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)), "Create constant buffer");
    CheckHr(m_constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedConstants)), "Map constant buffer");
}

void D3D12Engine::UpdateSceneConstants()
{
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_startTime).count();
    const XMMATRIX world = XMMatrixRotationY(seconds * 0.85f) * XMMatrixRotationX(seconds * 0.35f);
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 1.2f, -6.0f, 0.0f), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV4, static_cast<float>(m_width) / m_height, 0.1f, 100.0f);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(m_mappedConstants->world), world);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(m_mappedConstants->worldViewProjection), world * view * projection);
    m_mappedConstants->lightDirection[0] = -0.45f;
    m_mappedConstants->lightDirection[1] = -0.8f;
    m_mappedConstants->lightDirection[2] = 0.35f;
    m_mappedConstants->lightDirection[3] = 0.0f;
}

void D3D12Engine::Render()
{
    UpdateSceneConstants();
    CheckHr(m_commandAllocator->Reset(), "Reset command allocator");
    CheckHr(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()), "Reset command list");
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += m_frameIndex * m_rtvDescriptorSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clearColor[] = { 0.035f, 0.075f, 0.14f, 1.0f };
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->DrawInstanced(m_vertexCount, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
    CheckHr(m_commandList->Close(), "Close command list");
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    CheckHr(m_swapChain->Present(1, 0), "Present");
    WaitForGpu();
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Engine::WaitForGpu()
{
    ++m_fenceValue;
    CheckHr(m_commandQueue->Signal(m_fence.Get(), m_fenceValue), "Signal fence");
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        CheckHr(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent), "Set fence event");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12Engine::Shutdown()
{
    if (m_commandQueue && m_fence) WaitForGpu();
    if (m_constantBuffer) m_constantBuffer->Unmap(0, nullptr);
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
}
