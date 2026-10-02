// Simple ambient + Lambert light.  This is intentionally inexpensive but makes
// the cube's rotation and face orientation visible.
cbuffer SceneConstants : register(b0)
{
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 lightDirection;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    const float ambient = 0.28f;
    const float diffuse = saturate(dot(normalize(input.normal), normalize(-lightDirection.xyz)));
    return float4(input.color.rgb * (ambient + diffuse * 0.72f), input.color.a);
}
