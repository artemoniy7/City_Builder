cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    float3 lightDirection;
    float padding;
    float timeSeconds;
    float waterPadding;
    float3 cameraPosition;
    float cameraPadding;
    float cameraOrbitDistance;
    float waterLodPadding;
};

Texture2D shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 color      : COLOR;
    float3 normal     : NORMAL;
    float waterDepth  : WATERDEPTH;
};

struct PixelInput
{
    float4 position      : SV_POSITION;
    float3 worldPosition : POSITION0;
    float3 color         : COLOR;
    float3 normal        : NORMAL;
    float waterDepth     : WATERDEPTH;
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
    output.waterDepth     = input.waterDepth;
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

float3 DisplaceWater(float3 position, float waveScale)
{
    const float2 directionA = normalize(float2(0.86f, 0.51f));
    const float2 directionB = normalize(float2(-0.42f, 0.91f));
    const float2 directionC = normalize(float2(0.18f, -0.98f));

    WaveResult waveA = ApplyWave(position, directionA, 10.0f, 0.48f * waveScale, 0.48f, 1.05f);
    WaveResult waveB = ApplyWave(waveA.position, directionB, 5.2f, 0.22f * waveScale, 0.35f, 0.78f);
    WaveResult waveC = ApplyWave(waveB.position, directionC, 2.8f, 0.09f * waveScale, 0.22f, 1.35f);
    return waveC.position;
}

PixelInput WaterVS(VertexInput input)
{
    PixelInput output;
    const float cameraDistance = distance(
        float3(cameraPosition[0], cameraPosition[1], cameraPosition[2]),
        input.position);

    const bool useStaticWater = cameraOrbitDistance >= 5500.0f;
    const float shoreWaveFade = smoothstep(0.15f, 2.5f, input.waterDepth);
    const float3 world = useStaticWater
        ? input.position
        : DisplaceWater(input.position, shoreWaveFade);

    float3 surfaceNormal = input.normal;
    if (!useStaticWater)
    {
        const float sampleOffset = 0.12f;
        const float3 offsetX = DisplaceWater(
            input.position + float3(sampleOffset, 0.0f, 0.0f), shoreWaveFade);
        const float3 offsetZ = DisplaceWater(
            input.position + float3(0.0f, 0.0f, sampleOffset), shoreWaveFade);
        surfaceNormal = normalize(cross(offsetZ - world, offsetX - world));
    }

    output.position = mul(float4(world, 1.0f), viewProjection);
    output.worldPosition = world;
    output.color = input.color;
    output.normal = surfaceNormal;
    output.waterDepth = input.waterDepth;
    output.shadowPosition = mul(float4(world, 1.0f), lightViewProjection);
    return output;
}

float4 PSWater(PixelInput input) : SV_TARGET
{
    const float cameraDistance = distance(
        float3(cameraPosition[0], cameraPosition[1], cameraPosition[2]),
        input.worldPosition);

    const float3 skyColor = float3(0.23f, 0.58f, 0.92f);
    const float3 shallowColor = float3(0.08f, 0.46f, 0.47f);
    const float3 deepColor = float3(0.008f, 0.095f, 0.15f);
    const float3 sunColor = float3(1.0f, 0.86f, 0.58f);

    // TerrainHeight is baked into the water mesh as waterDepth. This avoids a
    // second terrain texture while still giving every water pixel a local depth.
    const float depthFactor = smoothstep(0.5f, 18.0f, input.waterDepth);
    const float3 waterColor = lerp(shallowColor, deepColor, depthFactor);

    const float3 baseNormal = normalize(input.normal);
    float3 normal = baseNormal;

    if (cameraOrbitDistance < 5500.0f)
    {
        // Two small, independent normal ripples keep the surface alive even
        // when the geometric Gerstner displacement is subtle.
        const float rippleA =
            sin(dot(input.worldPosition.xz, float2(0.62f, 0.31f)) + timeSeconds * 1.85f);
        const float rippleB =
            sin(dot(input.worldPosition.xz, float2(-0.27f, 0.71f)) - timeSeconds * 1.25f);
        const float waveNormalStrength = 0.075f * smoothstep(0.15f, 2.5f, input.waterDepth);
        normal.x += rippleA * waveNormalStrength;
        normal.z += rippleB * waveNormalStrength;
        normal = normalize(normal);
    }

    const float3 viewDirection = normalize(
        float3(cameraPosition[0], cameraPosition[1], cameraPosition[2]) - input.worldPosition);
    const float3 sunDirection = normalize(lightDirection);
    const float3 halfVector = normalize(sunDirection + viewDirection);

    // Fresnel makes the surface reflect more sky at grazing viewing angles.
    const float fresnel = pow(1.0f - saturate(dot(normal, viewDirection)), 5.0f);
    const float3 reflectedColor = lerp(waterColor, skyColor, 0.10f + fresnel * 0.72f);

    const float shadow = CalculateShadow(input.shadowPosition);
    const float diffuse = 0.12f + 0.34f * saturate(dot(normal, sunDirection));

    // Blinn-Phong sun highlight. Shadowing suppresses the highlight just like
    // the terrain's direct-light term.
    const float nDotL = saturate(dot(normal, sunDirection));
    const float specularPower = 96.0f;
    const float specular = pow(saturate(dot(normal, halfVector)), specularPower)
        * nDotL * shadow;
    const float3 sunReflection = sunColor * specular * 1.35f;

    float3 finalColor = reflectedColor * diffuse + sunReflection;

    // Shallow water receives a soft foam tint where the submerged terrain is
    // close to the surface. It fades away naturally with depth.
    const float foam = 1.0f - smoothstep(0.25f, 1.5f, input.waterDepth);
    finalColor = lerp(finalColor, float3(0.88f, 0.95f, 0.92f), foam * 0.60f);

    // At strategic-map distance, remove the expensive animated terms while
    // keeping the same depth-aware water material.
    const float alpha = smoothstep(0.05f, 2.0f, input.waterDepth);
    const float distantAlpha = cameraOrbitDistance >= 5500.0f ? 1.0f : alpha;
    return float4(finalColor, distantAlpha);
}
