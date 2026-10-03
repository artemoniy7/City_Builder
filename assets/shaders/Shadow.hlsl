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
};

float4 ShadowVS(VertexInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0f), lightViewProjection);
}
