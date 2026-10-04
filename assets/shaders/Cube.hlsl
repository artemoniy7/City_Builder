cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    float3 lightDirection;
    float padding;
    float timeSeconds;
    float3 waterPadding;
};

Texture2D shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

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
    float4 shadowPosition : POSITION1;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = float4(input.position, 1.0f);
    output.position      = mul(worldPosition, viewProjection);
    output.worldPosition = input.position;
    output.color         = input.color;
    output.normal        = input.normal;
    output.shadowPosition = mul(worldPosition, lightViewProjection);
    return output;
}

float CalculateShadow(float4 shadowPosition)
{
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float2 uv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || projected.z <= 0.0f || projected.z >= 1.0f)
        return 1.0f;

    uint width, height;
    shadowMap.GetDimensions(width, height);
    const float2 texelSize = 1.0f / float2(width, height);
    float visibility = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
            visibility += shadowMap.SampleCmpLevelZero(shadowSampler, uv + float2(x, y) * texelSize, projected.z - 0.0015f);
    }
    return visibility / 25.0f;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    const float3 normal      = normalize(input.normal);
    const float  directLight = saturate(dot(normal, normalize(lightDirection)));
    const float  shadow      = CalculateShadow(input.shadowPosition);
    const float  lighting    = 0.16f + 0.84f * directLight * shadow;
    return float4(input.color * lighting, 1.0f);
}


PixelInput WaterVS(VertexInput input)
{
    PixelInput output;
    float2 positionXZ = input.position.xz;
    float waveA = sin(dot(positionXZ, float2(0.085f, 0.052f)) + timeSeconds * 1.35f) * 0.10f;
    float waveB = sin(dot(positionXZ, float2(-0.041f, 0.097f)) - timeSeconds * 0.95f) * 0.06f;
    float waveC = sin(dot(positionXZ, float2(0.17f, -0.12f)) + timeSeconds * 1.8f) * 0.025f;
    float3 world = input.position;
    world.y += waveA + waveB + waveC;

    output.position = mul(float4(world, 1.0f), viewProjection);
    output.worldPosition = world;
    output.color = input.color;
    output.normal = input.normal;
    output.shadowPosition = 0.0f;
    return output;
}

float4 PSWater(PixelInput input) : SV_TARGET
{
    float waveLightA = sin(dot(input.worldPosition.xz, float2(0.13f, 0.08f)) + timeSeconds * 1.5f);
    float waveLightB = sin(dot(input.worldPosition.xz, float2(-0.08f, 0.16f)) - timeSeconds * 1.1f);
    float ripple = saturate(0.5f + 0.5f * (waveLightA * 0.65f + waveLightB * 0.35f));

    const float3 deepWater = float3(0.035f, 0.30f, 0.38f);
    const float3 shallowWater = float3(0.08f, 0.50f, 0.56f);
    const float3 baseColor = lerp(deepWater, shallowWater, ripple * 0.55f);

    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(float3(0.0f, 1.0f, 0.0f));
    float specular = pow(saturate(dot(normal, viewDirection)), 24.0f);
    float foam = smoothstep(0.72f, 0.98f, ripple) * 0.12f;
    float3 color = baseColor + specular * 0.30f + foam;
    return float4(color, 0.82f);
}
