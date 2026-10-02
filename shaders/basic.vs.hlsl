// Lightweight per-vertex transform and normal preparation for the engine smoke test.
cbuffer SceneConstants : register(b0)
{
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 lightDirection;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), worldViewProjection);
    output.normal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
    output.color = input.color;
    return output;
}
