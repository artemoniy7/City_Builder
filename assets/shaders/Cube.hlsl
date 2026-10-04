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


struct WaveResult
{
    float3 position;
    float3 tangentX;
    float3 tangentZ;
};

WaveResult ApplyWave(float3 position, float2 direction, float wavelength, float amplitude, float steepness, float speed)
{
    const float k = 6.2831853f / wavelength;
    const float frequency = k * speed;
    const float phase = dot(direction, position.xz) * k + timeSeconds * frequency;
    const float s = sin(phase);
    const float c = cos(phase);
    const float q = steepness / max(k * amplitude, 0.001f);

    WaveResult result;
    result.position = position;
    result.position.xz += direction * (q * amplitude * c);
    result.position.y += amplitude * s;

    result.tangentX = float3(
        1.0f - q * direction.x * direction.x * amplitude * k * s,
        direction.x * amplitude * k * c,
        -q * direction.x * direction.y * amplitude * k * s);

    result.tangentZ = float3(
        -q * direction.x * direction.y * amplitude * k * s,
        direction.y * amplitude * k * c,
        1.0f - q * direction.y * direction.y * amplitude * k * s);
    return result;
}

float3 DisplaceWater(float3 position)
{
    const float2 directionA = normalize(float2(0.86f, 0.51f));
    const float2 directionB = normalize(float2(-0.42f, 0.91f));
    const float2 directionC = normalize(float2(0.18f, -0.98f));

    WaveResult waveA = ApplyWave(position, directionA, 10.0f, 0.38f, 0.48f, 1.05f);
    WaveResult waveB = ApplyWave(waveA.position, directionB, 5.2f, 0.17f, 0.35f, 0.78f);
    WaveResult waveC = ApplyWave(waveB.position, directionC, 2.8f, 0.07f, 0.22f, 1.35f);
    return waveC.position;
}

PixelInput WaterVS(VertexInput input)
{
    PixelInput output;
    const float3 world = DisplaceWater(input.position);

    // Sample the deformed surface a tiny distance away in both axes to get the
    // true normal of the animated wave geometry.
    const float sampleOffset = 0.12f;
    const float3 offsetX = DisplaceWater(input.position + float3(sampleOffset, 0.0f, 0.0f));
    const float3 offsetZ = DisplaceWater(input.position + float3(0.0f, 0.0f, sampleOffset));
    const float3 waveNormal = normalize(cross(offsetZ - world, offsetX - world));

    output.position = mul(float4(world, 1.0f), viewProjection);
    output.worldPosition = world;
    output.color = input.color;
    output.normal = waveNormal;
    output.shadowPosition = 0.0f;
    return output;
}

float4 PSWater(PixelInput input) : SV_TARGET
{
    const float3 normal = normalize(input.normal);
    const float3 viewDirection = normalize(float3(cameraPosition[0], cameraPosition[1], cameraPosition[2]) - input.worldPosition);
    const float3 sunDirection = normalize(lightDirection);

    const float fresnel = pow(1.0f - saturate(dot(normal, viewDirection)), 4.0f);
    const float diffuse = 0.15f + 0.25f * saturate(dot(normal, sunDirection));
    const float specular = pow(saturate(dot(reflect(-sunDirection, normal), viewDirection)), 72.0f);

    // A small amount of moving surface variation breaks up the perfectly uniform water color.
    const float rippleA = sin(dot(input.worldPosition.xz, float2(0.45f, 0.23f)) + timeSeconds * 1.7f);
    const float rippleB = sin(dot(input.worldPosition.xz, float2(-0.31f, 0.52f)) - timeSeconds * 1.2f);
    const float microRipples = 0.5f + 0.5f * rippleA * rippleB;

    const float3 deepWater = float3(0.015f, 0.12f, 0.16f);
    const float3 skyReflection = float3(0.28f, 0.55f, 0.68f);
    const float3 reflectedColor = lerp(skyReflection, float3(0.65f, 0.80f, 0.84f), microRipples * 0.25f);
    const float3 baseColor = lerp(deepWater, reflectedColor, 0.18f + fresnel * 0.70f);
    const float3 finalColor = baseColor * diffuse + specular * float3(0.95f, 0.98f, 1.0f);

    // Keep it slightly transparent so the river still reads as a surface over the terrain.
    return float4(finalColor, 0.90f);
}
