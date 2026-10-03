cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    float3 lightDirection;
    float padding;
};

Texture2D<float> shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION0;
    float3 color : COLOR;
    float3 normal : NORMAL;
    float4 shadowPosition : TEXCOORD0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = float4(input.position, 1.0f);
    output.position = mul(worldPosition, viewProjection);
    output.worldPosition = input.position;
    output.color = input.color;
    output.normal = input.normal;
    output.shadowPosition = mul(worldPosition, lightViewProjection);
    return output;
}

float4 ShadowVS(VertexInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0f), lightViewProjection);
}

float ShadowFactor(float4 shadowPosition)
{
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float2 uv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z <= 0.0f || projected.z >= 1.0f) return 1.0f;
    const float texel = 1.0f / 2048.0f;
    float visibility = 0.0f;
    [unroll] for (int x = -1; x <= 1; ++x)
        [unroll] for (int y = -1; y <= 1; ++y)
            visibility += shadowMap.SampleCmpLevelZero(shadowSampler, uv + float2(x, y) * texel, projected.z - 0.0015f);
    return visibility / 9.0f;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    const float3 normal = normalize(input.normal);
    const float directLight = saturate(dot(normal, normalize(lightDirection)));
    const float lighting = 0.28f + 0.72f * directLight * ShadowFactor(input.shadowPosition);
    return float4(input.color * lighting, 1.0f);
}
