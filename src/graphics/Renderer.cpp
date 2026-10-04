#include "graphics/Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

float TerrainHeight(float x, float z) {
    const float rollingHills = 7.0f * std::sin(x * 0.018f) * std::cos(z * 0.015f);
    const float ridge = 18.0f * std::sin((x + z) * 0.008f) * std::sin((x - z) * 0.006f);
    const float mountainDistance = (x - 210.0f) * (x - 210.0f) + (z - 170.0f) * (z - 170.0f);
    const float mountainBase = std::exp(-(210.0f * 210.0f + 170.0f * 170.0f) / 28000.0f);
    const float mountain = 115.0f * (std::exp(-mountainDistance / 28000.0f) - mountainBase);
    return rollingHills + ridge + mountain;
}

city::math::Vector3 TerrainNormal(float x, float z) {
    constexpr float sampleDistance = 2.0f;
    const float dx = TerrainHeight(x + sampleDistance, z) - TerrainHeight(x - sampleDistance, z);
    const float dz = TerrainHeight(x, z + sampleDistance) - TerrainHeight(x, z - sampleDistance);
    return city::math::Normalize({-dx, sampleDistance * 2.0f, -dz});
}

float TerrainNoise(float x, float z) {
    // Deterministic value noise: stable between runs and cheap enough for terrain generation.
    const float value = std::sin(x * 12.9898f + z * 78.233f) * 43758.5453f;
    return 2.0f * (value - std::floor(value)) - 1.0f;
}

city::math::Vector3 ResolveCameraTerrainCollision(const city::math::Vector3& target, const city::math::Vector3& desiredPosition) {
    constexpr float clearance = 2.0f;
    constexpr int samples = 96;

    const city::math::Vector3 direction = desiredPosition - target;

    // Treat the terrain surface between the target and camera as the dynamic
    // collision plane. This lets the camera climb with a mountain instead of
    // stopping at the first point where the old orbit intersects the terrain.
    float highestSurface = TerrainHeight(target.x, target.z);
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / samples;
        const float x = target.x + direction.x * t;
        const float z = target.z + direction.z * t;
        highestSurface = std::max(highestSurface, TerrainHeight(x, z));
    }

    const float minimumCameraHeight = highestSurface + clearance;
    city::math::Vector3 corrected = desiredPosition;
    corrected.y = std::max(corrected.y, minimumCameraHeight);

    return corrected;
}

std::filesystem::path ShaderPath() {
    std::array<wchar_t, MAX_PATH> executablePath{};
    GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    return std::filesystem::path(executablePath.data()).parent_path() / L"shaders" / L"Cube.hlsl";
}

std::filesystem::path ShadowShaderPath() {
    std::array<wchar_t, MAX_PATH> executablePath{};
    GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    return std::filesystem::path(executablePath.data()).parent_path() / L"shaders" / L"Shadow.hlsl";
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
    // ---------- Root signature ----------
    D3D12_ROOT_PARAMETER rootParameters[2]{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE shadowMapRange{};
    shadowMapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowMapRange.NumDescriptors = 1;
    shadowMapRange.BaseShaderRegister = 0;
    shadowMapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &shadowMapRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
    rootSignatureDescription.NumParameters = 2;
    rootSignatureDescription.pParameters = rootParameters;
    rootSignatureDescription.NumStaticSamplers = 1;
    rootSignatureDescription.pStaticSamplers = &shadowSampler;
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSignature, &errors));
    ThrowIfFailed(device_->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(),
        serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)));

    // ---------- Shaders ----------
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> shadowVertexShader;
    const auto shaderPath = ShaderPath();
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexShader, &errors));
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelShader, &errors));
    const auto shadowShaderPath = ShadowShaderPath();
    ThrowIfFailed(D3DCompileFromFile(shadowShaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowVS", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &shadowVertexShader, &errors));

    // ---------- Input layout ----------
    const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputLayout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    // ---------- Pipeline state ----------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = rootSignature_.Get();
    pipelineDescription.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pipelineDescription.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    pipelineDescription.SampleMask = UINT_MAX;

    pipelineDescription.BlendState.AlphaToCoverageEnable  = FALSE;
    pipelineDescription.BlendState.IndependentBlendEnable = FALSE;
    for (auto& rt : pipelineDescription.BlendState.RenderTarget) {
        rt.BlendEnable           = FALSE;
        rt.LogicOpEnable         = FALSE;
        rt.SrcBlend              = D3D12_BLEND_ONE;
        rt.DestBlend             = D3D12_BLEND_ZERO;
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rt.DestBlendAlpha        = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.LogicOp               = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pipelineDescription.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pipelineDescription.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    pipelineDescription.RasterizerState.FrontCounterClockwise = FALSE;
    pipelineDescription.RasterizerState.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipelineDescription.RasterizerState.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthClipEnable       = TRUE;
    pipelineDescription.RasterizerState.MultisampleEnable     = FALSE;
    pipelineDescription.RasterizerState.AntialiasedLineEnable = FALSE;
    pipelineDescription.RasterizerState.ForcedSampleCount     = 0;
    pipelineDescription.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pipelineDescription.DepthStencilState.DepthEnable      = TRUE;
    pipelineDescription.DepthStencilState.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDescription.DepthStencilState.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
    pipelineDescription.DepthStencilState.StencilEnable    = FALSE;
    pipelineDescription.DepthStencilState.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
    pipelineDescription.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

    pipelineDescription.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
    pipelineDescription.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipelineDescription.SampleDesc.Count   = 1;
    pipelineDescription.SampleDesc.Quality = 0;
    pipelineDescription.NodeMask = 0;
    pipelineDescription.CachedPSO = {};
    pipelineDescription.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&pipelineState_)));

    auto shadowPipelineDescription = pipelineDescription;
    shadowPipelineDescription.VS = {shadowVertexShader->GetBufferPointer(), shadowVertexShader->GetBufferSize()};
    shadowPipelineDescription.PS = {};
    shadowPipelineDescription.NumRenderTargets = 0;
    shadowPipelineDescription.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    shadowPipelineDescription.RasterizerState.DepthBias = 1000;
    shadowPipelineDescription.RasterizerState.SlopeScaledDepthBias = 1.5f;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&shadowPipelineDescription, IID_PPV_ARGS(&shadowPipelineState_)));

    D3D12_DESCRIPTOR_HEAP_DESC shadowDsvHeapDescription{};
    shadowDsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    shadowDsvHeapDescription.NumDescriptors = 1;
    ThrowIfFailed(device_->CreateDescriptorHeap(&shadowDsvHeapDescription, IID_PPV_ARGS(&shadowDsvHeap_)));
    D3D12_DESCRIPTOR_HEAP_DESC shadowSrvHeapDescription{};
    shadowSrvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shadowSrvHeapDescription.NumDescriptors = 1;
    shadowSrvHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&shadowSrvHeapDescription, IID_PPV_ARGS(&shaderResourceHeap_)));
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC shadowMapDescription{};
    shadowMapDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowMapDescription.Width = kShadowMapSize;
    shadowMapDescription.Height = kShadowMapSize;
    shadowMapDescription.DepthOrArraySize = 1;
    shadowMapDescription.MipLevels = 1;
    shadowMapDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    shadowMapDescription.SampleDesc.Count = 1;
    shadowMapDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    shadowMapDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE shadowClearValue{};
    shadowClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    shadowClearValue.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &shadowMapDescription,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &shadowClearValue, IID_PPV_ARGS(&shadowMap_)));
    D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsvDescription{};
    shadowDsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
    shadowDsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowMap_.Get(), &shadowDsvDescription, shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDescription{};
    shadowSrvDescription.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDescription.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowMap_.Get(), &shadowSrvDescription, shaderResourceHeap_->GetCPUDescriptorHandleForHeapStart());

    // ---------- Geometry ----------
    std::vector<Vertex> vertices{
        // Cube, resting on the platform at y = -1.
        {{-1,0,-1},{1,0,0},{0,0,-1}}, {{-1,2,-1},{1,0,0},{0,0,-1}}, {{ 1,2,-1},{1,0,0},{0,0,-1}},
        {{-1,0,-1},{1,0,0},{0,0,-1}}, {{ 1,2,-1},{1,0,0},{0,0,-1}}, {{ 1,0,-1},{1,0,0},{0,0,-1}},
        {{ 1,0, 1},{0,1,0},{0,0, 1}}, {{ 1,2, 1},{0,1,0},{0,0, 1}}, {{-1,2, 1},{0,1,0},{0,0, 1}},
        {{ 1,0, 1},{0,1,0},{0,0, 1}}, {{-1,2, 1},{0,1,0},{0,0, 1}}, {{-1,0, 1},{0,1,0},{0,0, 1}},
        {{-1,0, 1},{0,0,1},{-1,0,0}}, {{-1,2, 1},{0,0,1},{-1,0,0}}, {{-1,2,-1},{0,0,1},{-1,0,0}},
        {{-1,0, 1},{0,0,1},{-1,0,0}}, {{-1,2,-1},{0,0,1},{-1,0,0}}, {{-1,0,-1},{0,0,1},{-1,0,0}},
        {{ 1,0,-1},{1,1,0},{ 1,0,0}}, {{ 1,2,-1},{1,1,0},{ 1,0,0}}, {{ 1,2, 1},{1,1,0},{ 1,0,0}},
        {{ 1,0,-1},{1,1,0},{ 1,0,0}}, {{ 1,2, 1},{1,1,0},{ 1,0,0}}, {{ 1,0, 1},{1,1,0},{ 1,0,0}},
        {{-1,2,-1},{1,0,1},{0, 1,0}}, {{-1,2, 1},{1,0,1},{0, 1,0}}, {{ 1,2, 1},{1,0,1},{0, 1,0}},
        {{-1,2,-1},{1,0,1},{0, 1,0}}, {{ 1,2, 1},{1,0,1},{0, 1,0}}, {{ 1,2,-1},{1,0,1},{0, 1,0}},
        {{-1,0, 1},{0,1,1},{0,-1,0}}, {{-1,0,-1},{0,1,1},{0,-1,0}}, {{ 1,0,-1},{0,1,1},{0,-1,0}},
        {{-1,0, 1},{0,1,1},{0,-1,0}}, {{ 1,0,-1},{0,1,1},{0,-1,0}}, {{ 1,0, 1},{0,1,1},{0,-1,0}},
    };

    constexpr float terrainSize = 1000.0f;
    const float cellSize = terrainSize / kTerrainResolution;
    const auto appendTerrainVertex = [&vertices](float x, float z) {
        const float height = TerrainHeight(x, z);
        const auto normal = TerrainNormal(x, z);
        const float slope = 1.0f - normal.y;
        const std::array<float, 3> baseColor = height < -6.0f ? std::array<float, 3>{0.76f, 0.67f, 0.35f}
            : (slope > 0.22f || height > 55.0f) ? std::array<float, 3>{0.42f, 0.43f, 0.40f}
            : (slope > 0.10f) ? std::array<float, 3>{0.38f, 0.24f, 0.13f}
            : std::array<float, 3>{0.20f, 0.55f, 0.22f};

        // Add subtle, deterministic RGB variation so large terrain areas are not perfectly flat.
        const float noise = TerrainNoise(x * 0.075f, z * 0.075f);
        const float redNoise = TerrainNoise(x * 0.11f + 17.0f, z * 0.11f - 31.0f);
        const float greenNoise = TerrainNoise(x * 0.11f - 43.0f, z * 0.11f + 7.0f);
        const float blueNoise = TerrainNoise(x * 0.11f + 61.0f, z * 0.11f + 29.0f);
        std::array<float, 3> color{
            baseColor[0] * (1.0f + noise * 0.08f) + redNoise * 0.025f,
            baseColor[1] * (1.0f + noise * 0.08f) + greenNoise * 0.025f,
            baseColor[2] * (1.0f + noise * 0.08f) + blueNoise * 0.025f
        };

        // Snow gradually appears on high, flatter mountain surfaces, with a little noise
        // to keep the snow line irregular instead of producing a hard horizontal cutoff.
        const float snowHeight = std::clamp((height - 58.0f) / 14.0f, 0.0f, 1.0f);
        const float snowSlope = std::clamp(1.0f - std::max(slope - 0.12f, 0.0f) / 0.28f, 0.0f, 1.0f);
        const float snowCoverage = snowHeight * snowSlope * (0.82f + 0.18f * (noise + 1.0f) * 0.5f);
        color[0] = color[0] * (1.0f - snowCoverage) + snowCoverage;
        color[1] = color[1] * (1.0f - snowCoverage) + snowCoverage;
        color[2] = color[2] * (1.0f - snowCoverage) + snowCoverage;

        for (float& channel : color) channel = std::clamp(channel, 0.0f, 1.0f);
        vertices.push_back({{x, height, z}, {color[0], color[1], color[2]}, {normal.x, normal.y, normal.z}});
    };
    vertices.reserve(vertices.size() + kTerrainResolution * kTerrainResolution * 6);
    for (UINT z = 0; z < kTerrainResolution; ++z) {
        for (UINT x = 0; x < kTerrainResolution; ++x) {
            const float x0 = -terrainSize * 0.5f + x * cellSize;
            const float z0 = -terrainSize * 0.5f + z * cellSize;
            const float x1 = x0 + cellSize;
            const float z1 = z0 + cellSize;
            appendTerrainVertex(x0, z0); appendTerrainVertex(x0, z1); appendTerrainVertex(x1, z1);
            appendTerrainVertex(x0, z0); appendTerrainVertex(x1, z1); appendTerrainVertex(x1, z0);
        }
    }
    vertexCount_ = static_cast<UINT>(vertices.size());

    const UINT bufferSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDescription{};
    bufferDescription.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDescription.Width            = bufferSize;
    bufferDescription.Height           = 1;
    bufferDescription.DepthOrArraySize = 1;
    bufferDescription.MipLevels        = 1;
    bufferDescription.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDescription.SampleDesc.Count = 1;
    bufferDescription.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDescription.Flags            = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer_)));

    void* vertexData{};
    ThrowIfFailed(vertexBuffer_->Map(0, nullptr, &vertexData));
    std::memcpy(vertexData, vertices.data(), bufferSize);
    vertexBuffer_->Unmap(0, nullptr);

    vertexBufferView_ = {vertexBuffer_->GetGPUVirtualAddress(), bufferSize, sizeof(Vertex)};

    // ---------- Constant buffer ----------
    bufferDescription.Width = sizeof(SceneConstants);
    ThrowIfFailed(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer_)));
    ThrowIfFailed(constantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedConstants_)));
}

void Renderer::UpdateCamera(float deltaSeconds) {
    // Pan speed grows with zoom distance, similar to city-builder camera controls:
    // a distant camera needs to cross much more world space per second.
    constexpr float baseMoveSpeed = 8.0f;
    constexpr float referenceOrbitDistance = 18.0f;
    constexpr float maxMoveSpeedMultiplier = 12.0f;
    const float zoomSpeedMultiplier = std::clamp(
        cameraOrbitDistance_ / referenceOrbitDistance,
        1.0f,
        maxMoveSpeedMultiplier);
    const float speed = baseMoveSpeed * zoomSpeedMultiplier * deltaSeconds;
    const float turnSpeed = 1.5f * deltaSeconds;

    // Q/E orbit around the point the camera is looking at instead of rotating in place.
    if (GetAsyncKeyState('Q') & 0x8000) cameraYaw_ -= turnSpeed;
    if (GetAsyncKeyState('E') & 0x8000) cameraYaw_ += turnSpeed;

    const math::Vector3 forward{std::sin(cameraYaw_), 0.0f, std::cos(cameraYaw_)};
    const math::Vector3 right{forward.z, 0.0f, -forward.x};
    if (GetAsyncKeyState('W') & 0x8000) cameraTarget_ = cameraTarget_ + forward * speed;
    if (GetAsyncKeyState('S') & 0x8000) cameraTarget_ = cameraTarget_ - forward * speed;
    if (GetAsyncKeyState('A') & 0x8000) cameraTarget_ = cameraTarget_ - right * speed;
    if (GetAsyncKeyState('D') & 0x8000) cameraTarget_ = cameraTarget_ + right * speed;
    // R/F control camera tilt (pitch). Vertical target movement is intentionally disabled.
    const float pitchSpeed = 1.5f * deltaSeconds;
    if (GetAsyncKeyState('R') & 0x8000) cameraPitch_ += pitchSpeed;
    if (GetAsyncKeyState('F') & 0x8000) cameraPitch_ -= pitchSpeed;

    // T/G control zoom through the orbit radius.
    // Zooming moves the camera along its orbit and therefore naturally rises/falls with the current pitch.
    constexpr float zoomKeySpeed = 24.0f;
    if (GetAsyncKeyState('T') & 0x8000) cameraOrbitDistance_ -= zoomKeySpeed * deltaSeconds;
    if (GetAsyncKeyState('G') & 0x8000) cameraOrbitDistance_ += zoomKeySpeed * deltaSeconds;
    cameraOrbitDistance_ = std::clamp(cameraOrbitDistance_, 4.0f, 160.0f);

    // Keep the view between horizontal and straight down; the camera cannot look above the horizon.
    cameraPitch_ = std::clamp(cameraPitch_, -std::numbers::pi_v<float> * 0.5f, 0.0f);

    const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    if (spaceDown && !spaceWasDown_) {
        topDownView_ = !topDownView_;
        if (topDownView_) cameraPitch_ = -std::numbers::pi_v<float> * 0.5f;
        else cameraPitch_ = -0.576f;
    }
    spaceWasDown_ = spaceDown;

    const float horizontalScale = std::cos(cameraPitch_);
    const math::Vector3 offset{
        -std::sin(cameraYaw_) * horizontalScale * cameraOrbitDistance_,
        -std::sin(cameraPitch_) * cameraOrbitDistance_,
        -std::cos(cameraYaw_) * horizontalScale * cameraOrbitDistance_
    };
    const math::Vector3 desiredPosition = cameraTarget_ + offset;
    cameraPosition_ = ResolveCameraTerrainCollision(cameraTarget_, desiredPosition);
}

void Renderer::OnMouseWheel(short delta) {
    // Use the same multiplicative zoom model as city-builder cameras:
    // each wheel notch moves the camera by a consistent percentage of its
    // current distance, making both close and distant zooming predictable.
    constexpr float zoomStep = 1.15f;
    if (delta > 0) cameraOrbitDistance_ /= zoomStep;
    else if (delta < 0) cameraOrbitDistance_ *= zoomStep;
    cameraOrbitDistance_ = std::clamp(cameraOrbitDistance_, 4.0f, 160.0f);

    const float horizontalScale = std::cos(cameraPitch_);
    const math::Vector3 offset{
        -std::sin(cameraYaw_) * horizontalScale * cameraOrbitDistance_,
        -std::sin(cameraPitch_) * cameraOrbitDistance_,
        -std::cos(cameraYaw_) * horizontalScale * cameraOrbitDistance_
    };
    const math::Vector3 desiredPosition = cameraTarget_ + offset;
    cameraPosition_ = ResolveCameraTerrainCollision(cameraTarget_, desiredPosition);
}

void Renderer::Render() {
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = std::min(std::chrono::duration<float>(now - lastFrameAt_).count(), 0.1f);
    lastFrameAt_ = now;
    UpdateCamera(deltaSeconds);

    const float horizontalLookScale = std::cos(cameraPitch_);
    const math::Vector3 lookDirection{
        std::sin(cameraYaw_) * horizontalLookScale,
        std::sin(cameraPitch_),
        std::cos(cameraYaw_) * horizontalLookScale,
    };
    const math::Vector3 target = cameraTarget_;
    // A downward-facing camera needs a horizontal up vector. Deriving it from yaw keeps Q/E rotating the view.
    const bool useYawBasedUp = topDownView_ || std::abs(lookDirection.y) > 0.99f;
    const math::Vector3 yawBasedUp{-std::cos(cameraYaw_), 0.0f, std::sin(cameraYaw_)};
    const math::Vector3 up = useYawBasedUp ? yawBasedUp : math::Vector3{0.0f, 1.0f, 0.0f};
    const auto viewProjection = math::Matrix4::Multiply(
        math::Matrix4::LookAt(cameraPosition_, target, up),
        math::Matrix4::Perspective(fieldOfView_, static_cast<float>(width_) / height_, 0.1f, 10000.0f));

    const math::Vector3 lightPosition{-14.0f, 20.0f, -12.0f};
    const math::Vector3 lightTarget{0.0f, 0.0f, 0.0f};
    const auto lightViewProjection = math::Matrix4::Multiply(
        math::Matrix4::LookAt(lightPosition, lightTarget, {0.0f, 1.0f, 0.0f}),
        math::Matrix4::Perspective(1.25f, 1.0f, 1.0f, 60.0f));
    const math::Vector3 lightDirection = math::Normalize(lightPosition - lightTarget);

    std::memcpy(mappedConstants_->viewProjection, viewProjection.values, sizeof(viewProjection.values));
    std::memcpy(mappedConstants_->lightViewProjection, lightViewProjection.values, sizeof(lightViewProjection.values));
    mappedConstants_->lightDirection[0] = lightDirection.x;
    mappedConstants_->lightDirection[1] = lightDirection.y;
    mappedConstants_->lightDirection[2] = lightDirection.z;

    ThrowIfFailed(commandAllocator_->Reset());
    ThrowIfFailed(commandList_->Reset(commandAllocator_.Get(), pipelineState_.Get()));

    // First render the scene from the fixed sun position into a high-resolution depth map.
    const D3D12_VIEWPORT shadowViewport{0.0f, 0.0f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
    const D3D12_RECT shadowScissor{0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
    const auto shadowDsv = shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    commandList_->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList_->RSSetViewports(1, &shadowViewport);
    commandList_->RSSetScissorRects(1, &shadowScissor);
    commandList_->SetPipelineState(shadowPipelineState_.Get());
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->DrawInstanced(vertexCount_, 1, 0, 0);

    D3D12_RESOURCE_BARRIER shadowBarrier{};
    shadowBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    shadowBarrier.Transition.pResource = shadowMap_.Get();
    shadowBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    shadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    shadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1, &shadowBarrier);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = renderTargets_[frameIndex_].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &barrier);

    auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
    const auto dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    constexpr float skyBlue[]{0.23f, 0.58f, 0.92f, 1.0f};
    commandList_->ClearRenderTargetView(rtv, skyBlue, 0, nullptr);
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
    commandList_->DrawInstanced(vertexCount_, 1, 0, 0);

    shadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    shadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    commandList_->ResourceBarrier(1, &shadowBarrier);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barrier);

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
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void Renderer::WaitForGpu() {
    const UINT64 signalValue = ++fenceValue_;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), signalValue));
    if (fence_->GetCompletedValue() < signalValue) {
        ThrowIfFailed(fence_->SetEventOnCompletion(signalValue, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void Renderer::MoveToNextFrame() {
    const UINT64 signalValue = ++fenceValue_;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), signalValue));
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < signalValue) {
        ThrowIfFailed(fence_->SetEventOnCompletion(signalValue, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

} // namespace city
