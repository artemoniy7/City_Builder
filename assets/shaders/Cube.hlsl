cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    float3 lightDirection;
    float padding;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 color    : COLOR;
    float3 normal   : NORMAL;
};

struct PixelInput
{
    float4 position      : SV_POSITION;
    float3 worldPosition : POSITION0;
    float3 color         : COLOR;
    float3 normal        : NORMAL;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = float4(input.position, 1.0f);
    output.position      = mul(worldPosition, viewProjection);
    output.worldPosition = input.position;
    output.color         = input.color;
    output.normal        = input.normal;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    const float3 normal      = normalize(input.normal);
    const float  directLight = saturate(dot(normal, normalize(lightDirection)));
    const float  lighting    = 0.28f + 0.72f * directLight;
    return float4(input.color * lighting, 1.0f);
}