#include "graphics/Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kShadowMapSize = 2048;
constexpr UINT kPlatformVertexCount = 36;
constexpr UINT kCubeVertexCount = 36;
constexpr UINT kSunVertexCount = 36;

std::filesystem::path ShaderPath() {
    std::array<wchar_t, MAX_PATH> executablePath{};
    GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    return std::filesystem::path(executablePath.data()).parent_path() / L"shaders" / L"Cube.hlsl";
}

} // namespace

namespace city {

Renderer::Renderer(HWND window) : window_(window) {
    RECT clientRect{};
    GetClientRect(window_, &clientRect);
    width_ = static_cast<UINT>(clientRect.right - clientRect.left);
    height_ = static_cast<UINT>(clientRect.bottom - clientRect.top);
    CreateDeviceResources();
    CreateWindowResources(width_, height_);
    CreateAssets();
}

Renderer::~Renderer() {
    if (device_) WaitForGpu();
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

void Renderer::ThrowIfFailed(HRESULT result) const {
    if (FAILED(result)) throw std::runtime_error("A DirectX 12 operation failed.");
}

void Renderer::CreateDeviceResources() {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) debugController->EnableDebugLayer();
#endif
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)));
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(device_->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&commandQueue_)));
    ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_)));
    ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.Get(), nullptr,
        IID_PPV_ARGS(&commandList_)));
    ThrowIfFailed(commandList_->Close());
    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) throw std::runtime_error("Unable to create the GPU fence event.");
}

void Renderer::CreateWindowResources(UINT width, UINT height) {
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.BufferCount = kFrameCount;
    swapChainDescription.Width = width;
    swapChainDescription.Height = height;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory_->CreateSwapChainForHwnd(commandQueue_.Get(), window_, &swapChainDescription, nullptr, nullptr, &swapChain));
    ThrowIfFailed(factory_->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(swapChain.As(&swapChain_));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.NumDescriptors = kFrameCount;
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&rtvHeap_)));
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
    dsvHeapDescription.NumDescriptors = 1;
    dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&dsvHeapDescription, IID_PPV_ARGS(&dsvHeap_)));

    auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize_;
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depthDescription{};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = width;
    depthDescription.Height = height;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&depthStencil_)));
    device_->CreateDepthStencilView(depthStencil_.Get(), nullptr, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::CreateAssets() {
    // The shadow map is a depth texture with both DSV and SRV views: the depth pass writes it and the lighting pass reads it.
    D3D12_DESCRIPTOR_HEAP_DESC shadowDsvDescription{};
    shadowDsvDescription.NumDescriptors = 1;
    shadowDsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&shadowDsvDescription, IID_PPV_ARGS(&shadowDsvHeap_)));
    D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
    srvDescription.NumDescriptors = 1;
    srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&srvDescription, IID_PPV_ARGS(&shaderResourceHeap_)));
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC shadowDescription{};
    shadowDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDescription.Width = kShadowMapSize;
    shadowDescription.Height = kShadowMapSize;
    shadowDescription.DepthOrArraySize = 1;
    shadowDescription.MipLevels = 1;
    shadowDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    shadowDescription.SampleDesc.Count = 1;
    shadowDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    shadowDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE shadowClear{};
    shadowClear.Format = DXGI_FORMAT_D32_FLOAT;
    shadowClear.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &shadowDescription,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &shadowClear, IID_PPV_ARGS(&shadowMap_)));
    D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsv{};
    shadowDsv.Format = DXGI_FORMAT_D32_FLOAT;
    shadowDsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowMap_.Get(), &shadowDsv, shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrv{};
    shadowSrv.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowMap_.Get(), &shadowSrv, shaderResourceHeap_->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &shadowRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.ShaderRegister = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
    rootSignatureDescription.NumParameters = _countof(parameters);
    rootSignatureDescription.pParameters = parameters;
    rootSignatureDescription.NumStaticSamplers = 1;
    rootSignatureDescription.pStaticSamplers = &shadowSampler;
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serializedRootSignature, errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &errors));
    ThrowIfFailed(device_->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(), serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)));

    ComPtr<ID3DBlob> vertexShader, pixelShader, shadowVertexShader;
    const auto shaderPath = ShaderPath();
    constexpr UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &errors));
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &errors));
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowVS", "vs_5_0", compileFlags, 0, &shadowVertexShader, &errors));
    const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputLayout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = rootSignature_.Get();
    pipelineDescription.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pipelineDescription.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipelineDescription.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipelineDescription.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipelineDescription.SampleDesc.Count = 1;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&pipelineState_)));
    pipelineDescription.VS = {shadowVertexShader->GetBufferPointer(), shadowVertexShader->GetBufferSize()};
    pipelineDescription.PS = {};
    pipelineDescription.NumRenderTargets = 0;
    pipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipelineDescription.RasterizerState.DepthBias = 1000;
    pipelineDescription.RasterizerState.SlopeScaledDepthBias = 1.5f;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&shadowPipelineState_)));

    const std::array<float, 36 * 3> unitCubePositions{{
        -1,-1,-1, -1, 1,-1,  1, 1,-1, -1,-1,-1,  1, 1,-1,  1,-1,-1,  1,-1, 1,  1, 1, 1, -1, 1, 1,  1,-1, 1, -1, 1, 1, -1,-1, 1,
        -1,-1, 1, -1, 1, 1, -1, 1,-1, -1,-1, 1, -1, 1,-1, -1,-1,-1,  1,-1,-1,  1, 1,-1,  1, 1, 1,  1,-1,-1,  1, 1, 1,  1,-1, 1,
        -1, 1,-1, -1, 1, 1,  1, 1, 1, -1, 1,-1,  1, 1, 1,  1, 1,-1, -1,-1, 1, -1,-1,-1,  1,-1,-1, -1,-1, 1,  1,-1,-1,  1,-1, 1
    }};
    std::vector<Vertex> vertices;
    const auto appendBox = [&](math::Vector3 center, math::Vector3 halfSize, std::array<float, 3> color) {
        for (size_t i = 0; i < unitCubePositions.size(); i += 3) {
            const math::Vector3 p{unitCubePositions[i], unitCubePositions[i + 1], unitCubePositions[i + 2]};
            const math::Vector3 normal = math::Normalize(p); // smooth directional lighting is sufficient for these block primitives.
            vertices.push_back({{center.x + p.x * halfSize.x, center.y + p.y * halfSize.y, center.z + p.z * halfSize.z},
                                {color[0], color[1], color[2]}, {normal.x, normal.y, normal.z}});
        }
    };
    appendBox({0.0f, -0.5f, 0.0f}, {9.0f, 0.5f, 9.0f}, {0.12f, 0.52f, 0.16f}); // green platform
    appendBox({0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.82f, 0.24f, 0.12f});     // returned cube
    appendBox({-10.0f, 15.0f, 12.0f}, {0.9f, 0.9f, 0.3f}, {1.0f, 0.82f, 0.18f});  // static sun
    const UINT bufferSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width = bufferSize;
    bufferDescription.Height = 1;
    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels = 1;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer_)));
    void* vertexData{};
    ThrowIfFailed(vertexBuffer_->Map(0, nullptr, &vertexData));
    std::memcpy(vertexData, vertices.data(), bufferSize);
    vertexBuffer_->Unmap(0, nullptr);
    vertexBufferView_ = {vertexBuffer_->GetGPUVirtualAddress(), bufferSize, sizeof(Vertex)};
    bufferDescription.Width = sizeof(SceneConstants);
    ThrowIfFailed(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer_)));
    ThrowIfFailed(constantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedConstants_)));
}

void Renderer::UpdateCamera(float deltaSeconds) {
    const float speed = 8.0f * deltaSeconds;
    const float turnSpeed = 1.5f * deltaSeconds;
    const float fovSpeed = 0.65f * deltaSeconds;
    if (GetAsyncKeyState('Q') & 0x8000) cameraYaw_ -= turnSpeed;
    if (GetAsyncKeyState('E') & 0x8000) cameraYaw_ += turnSpeed;
    const math::Vector3 forward{std::sin(cameraYaw_), 0.0f, std::cos(cameraYaw_)};
    const math::Vector3 right{forward.z, 0.0f, -forward.x};
    if (GetAsyncKeyState('W') & 0x8000) cameraPosition_ = cameraPosition_ + forward * speed;
    if (GetAsyncKeyState('S') & 0x8000) cameraPosition_ = cameraPosition_ - forward * speed;
    if (GetAsyncKeyState('A') & 0x8000) cameraPosition_ = cameraPosition_ - right * speed;
    if (GetAsyncKeyState('D') & 0x8000) cameraPosition_ = cameraPosition_ + right * speed;
    if (GetAsyncKeyState('R') & 0x8000) cameraPosition_.y += speed;
    if (GetAsyncKeyState('F') & 0x8000) cameraPosition_.y -= speed;
    if (GetAsyncKeyState('T') & 0x8000) fieldOfView_ = std::min(fieldOfView_ + fovSpeed, 1.75f);
    if (GetAsyncKeyState('G') & 0x8000) fieldOfView_ = std::max(fieldOfView_ - fovSpeed, 0.35f);
    const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    if (spaceDown && !spaceWasDown_) topDownView_ = !topDownView_;
    spaceWasDown_ = spaceDown;
}

void Renderer::Render() {
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = std::min(std::chrono::duration<float>(now - lastFrameAt_).count(), 0.1f);
    lastFrameAt_ = now;
    UpdateCamera(deltaSeconds);
    const math::Vector3 flatForward{std::sin(cameraYaw_), 0.0f, std::cos(cameraYaw_)};
    const math::Vector3 target = topDownView_ ? cameraPosition_ + math::Vector3{0.0f, -1.0f, 0.0f} : cameraPosition_ + flatForward;
    const math::Vector3 up = topDownView_ ? math::Vector3{0.0f, 0.0f, 1.0f} : math::Vector3{0.0f, 1.0f, 0.0f};
    const auto viewProjection = math::Matrix4::Multiply(math::Matrix4::LookAt(cameraPosition_, target, up), math::Matrix4::Perspective(fieldOfView_, static_cast<float>(width_) / height_, 0.1f, 100.0f));
    const math::Vector3 lightPosition{-14.0f, 20.0f, -12.0f};
    const math::Vector3 lightTarget{0.0f, 0.0f, 0.0f};
    const auto lightViewProjection = math::Matrix4::Multiply(math::Matrix4::LookAt(lightPosition, lightTarget, {0.0f, 1.0f, 0.0f}), math::Matrix4::Perspective(1.25f, 1.0f, 1.0f, 60.0f));
    const math::Vector3 lightDirection = math::Normalize(lightPosition - lightTarget);
    std::memcpy(mappedConstants_->viewProjection, viewProjection.values, sizeof(viewProjection.values));
    std::memcpy(mappedConstants_->lightViewProjection, lightViewProjection.values, sizeof(lightViewProjection.values));
    mappedConstants_->lightDirection[0] = lightDirection.x;
    mappedConstants_->lightDirection[1] = lightDirection.y;
    mappedConstants_->lightDirection[2] = lightDirection.z;

    ThrowIfFailed(commandAllocator_->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocator_.Get(), shadowPipelineState_.Get()));
    D3D12_RESOURCE_BARRIER shadowBarrier{};
    shadowBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    shadowBarrier.Transition.pResource = shadowMap_.Get();
    shadowBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    shadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    shadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    commandList_->ResourceBarrier(1, &shadowBarrier);
    const auto shadowDsv = shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    commandList_->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT shadowViewport{0.0f, 0.0f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
    const D3D12_RECT shadowScissor{0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
    commandList_->RSSetViewports(1, &shadowViewport);
    commandList_->RSSetScissorRects(1, &shadowScissor);
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->DrawInstanced(kPlatformVertexCount + kCubeVertexCount, 1, 0, 0);
    shadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    shadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1, &shadowBarrier);

    D3D12_RESOURCE_BARRIER backBufferBarrier{};
    backBufferBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBufferBarrier.Transition.pResource = renderTargets_[frameIndex_].Get();
    backBufferBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    backBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    backBufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &backBufferBarrier);
    auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
    const auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    constexpr float skyBlue[]{0.23f, 0.58f, 0.92f, 1.0f};
    commandList_->ClearRenderTargetView(rtv, skyBlue, 0, nullptr); // blue skybox background
    commandList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->SetPipelineState(pipelineState_.Get());
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* descriptorHeaps[]{shaderResourceHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, descriptorHeaps);
    commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(1, shaderResourceHeap_->GetGPUDescriptorHandleForHeapStart());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->DrawInstanced(kPlatformVertexCount + kCubeVertexCount + kSunVertexCount, 1, 0, 0);
    backBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    backBufferBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &backBufferBarrier);
    ThrowIfFailed(commandList_->Close());
    ID3D12CommandList* commandLists[]{commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    ThrowIfFailed(swapChain_->Present(1, 0));
    MoveToNextFrame();
}

void Renderer::Resize(UINT width, UINT height) {
    WaitForGpu();
    width_ = width;
    height_ = height;
    for (auto& target : renderTargets_) target.Reset();
    depthStencil_.Reset();
    DXGI_SWAP_CHAIN_DESC description{};
    ThrowIfFailed(swapChain_->GetDesc(&description));
    ThrowIfFailed(swapChain_->ResizeBuffers(kFrameCount, width, height, description.BufferDesc.Format, description.Flags));
    auto handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize_;
    }
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depthDescription{};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = width; depthDescription.Height = height; depthDescription.DepthOrArraySize = 1; depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT; depthDescription.SampleDesc.Count = 1; depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue{}; clearValue.Format = DXGI_FORMAT_D32_FLOAT; clearValue.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&depthStencil_)));
    device_->CreateDepthStencilView(depthStencil_.Get(), nullptr, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void Renderer::WaitForGpu() {
    const UINT64 signalValue = ++fenceValue_;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), signalValue));
    if (fence_->GetCompletedValue() < signalValue) { ThrowIfFailed(fence_->SetEventOnCompletion(signalValue, fenceEvent_)); WaitForSingleObject(fenceEvent_, INFINITE); }
}

void Renderer::MoveToNextFrame() {
    const UINT64 signalValue = ++fenceValue_;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), signalValue));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < signalValue) { ThrowIfFailed(fence_->SetEventOnCompletion(signalValue, fenceEvent_)); WaitForSingleObject(fenceEvent_, INFINITE); }
}

} // namespace city
