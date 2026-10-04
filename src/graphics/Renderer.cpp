#include "graphics/Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <numbers>
#include <queue>
#include <random>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kWorldSize = 30000.0f;
constexpr float kWorldHalfSize = kWorldSize * 0.5f;
constexpr float kSeaLevel = -8.0f;
constexpr float kSeaWaterThreshold = 0.52f;
constexpr int kHydrologyResolution = 160;
constexpr int kRiverCount = 4;
constexpr int kRiverPathPoints = 40;
constexpr int kRiverCurveSamples = 48;

struct RiverDefinition {
    std::array<city::math::Vector3, kRiverPathPoints> path{};
    std::array<float, kRiverPathPoints> waterLevels{};
    std::array<float, kRiverPathPoints> widths{};
    int pathCount{};
};

struct WorldGenerationData {
    std::uint64_t seed{};
    std::array<RiverDefinition, kRiverCount> rivers{};
};

float SmoothStep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float HashNoise(int x, int z, std::uint64_t seed) {
    std::uint64_t h = static_cast<std::uint64_t>(x) * 0x9E3779B185EBCA87ULL;
    h ^= static_cast<std::uint64_t>(z) * 0xC2B2AE3D27D4EB4FULL;
    h ^= seed + 0x165667B19E3779F9ULL + (h << 6) + (h >> 2);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return static_cast<float>((h & 0x00FFFFFFULL) / 16777215.0) * 2.0f - 1.0f;
}

float ValueNoise(float x, float z, std::uint64_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int z0 = static_cast<int>(std::floor(z));
    const float tx = x - static_cast<float>(x0);
    const float tz = z - static_cast<float>(z0);
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sz = tz * tz * (3.0f - 2.0f * tz);
    const float n00 = HashNoise(x0, z0, seed);
    const float n10 = HashNoise(x0 + 1, z0, seed);
    const float n01 = HashNoise(x0, z0 + 1, seed);
    const float n11 = HashNoise(x0 + 1, z0 + 1, seed);
    const float nx0 = n00 + (n10 - n00) * sx;
    const float nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sz;
}

float FbmNoise(float x, float z, std::uint64_t seed) {
    float value = 0.0f;
    float amplitude = 0.55f;
    float frequency = 1.0f;
    float amplitudeSum = 0.0f;
    for (int octave = 0; octave < 5; ++octave) {
        value += ValueNoise(x * frequency, z * frequency, seed + static_cast<std::uint64_t>(octave * 977)) * amplitude;
        amplitudeSum += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return value / amplitudeSum;
}

float RidgedNoise(float x, float z, std::uint64_t seed) {
    return 1.0f - std::abs(FbmNoise(x, z, seed));
}

float SeaMask(float x, float z) {
    // The sea occupies a rounded corner basin rather than a rectangular patch.
    // A smooth ellipse gives the coastline a continuous curved boundary.
    constexpr float seaCenterX = -15000.0f;
    constexpr float seaCenterZ = 15000.0f;
    constexpr float seaRadiusX = 9600.0f;
    constexpr float seaRadiusZ = 9600.0f;

    const float nx = (x - seaCenterX) / seaRadiusX;
    const float nz = (z - seaCenterZ) / seaRadiusZ;
    const float ellipseDistance = std::sqrt(nx * nx + nz * nz);

    // Low-frequency distortion breaks the perfect quarter-circle without
    // turning the coast into noisy fragments.
    const float coastlineWarp =
        0.055f * FbmNoise(x * 0.00022f, z * 0.00022f, 0x6A09E667F3BCC909ULL);
    return 1.0f - SmoothStep(0.82f, 1.02f, ellipseDistance - coastlineWarp);
}

float BaseTerrainHeight(float x, float z, std::uint64_t seed) {
    const float warpX = 850.0f * FbmNoise(x * 0.00010f + 7.0f, z * 0.00010f - 13.0f, seed + 11);
    const float warpZ = 850.0f * FbmNoise(x * 0.00010f - 17.0f, z * 0.00010f + 5.0f, seed + 29);
    const float wx = x + warpX;
    const float wz = z + warpZ;

    const float broad = FbmNoise(wx * 0.000075f, wz * 0.000075f, seed);
    const float hills = FbmNoise(wx * 0.00023f + 31.0f, wz * 0.00023f - 19.0f, seed + 101);
    const float detail = FbmNoise(wx * 0.00070f - 47.0f, wz * 0.00070f + 23.0f, seed + 211);

    float height = 28.0f + broad * 105.0f + hills * 52.0f + detail * 10.0f;

    // A compact mountain province: a few ridged peaks instead of a wall of mountains.
    const float mountainCenterX = 4300.0f + 1100.0f * FbmNoise(0.13f, 0.27f, seed + 301);
    const float mountainCenterZ = 500.0f + 1500.0f * FbmNoise(0.41f, 0.19f, seed + 307);
    const float dx = (wx - mountainCenterX) / 4300.0f;
    const float dz = (wz - mountainCenterZ) / 5600.0f;
    const float mountainMask = std::exp(-(dx * dx + dz * dz) * 1.7f);
    const float ridges = RidgedNoise(wx * 0.00038f + 13.0f, wz * 0.00038f - 7.0f, seed + 401);
    height += mountainMask * (250.0f + 520.0f * ridges);

    // A gentle continental gradient biases the drainage toward the sea corner.
    height += (x - z) * 0.0016f;

    const float sea = SeaMask(x, z);
    const float shelf = SmoothStep(0.05f, 0.75f, sea);
    const float seabed = -55.0f + 14.0f * FbmNoise(x * 0.00020f, z * 0.00020f, seed + 501);
    height = height * (1.0f - shelf) + seabed * shelf;

    // Keep the visible sea over a genuinely submerged basin. The water surface
    // is intentionally separated from the terrain so the shoreline cannot
    // shimmer from nearly coplanar depth values.
    const float submerged = SmoothStep(0.55f, 0.78f, sea);
    const float seaDepth = 8.0f + 32.0f * SmoothStep(0.55f, 1.0f, sea);
    const float guaranteedSeabed = kSeaLevel - seaDepth;
    const float submergedFloor = guaranteedSeabed + 4.0f;
    height = std::min(height, height * (1.0f - submerged) + submergedFloor * submerged);
    return height;
}

struct HydroCell {
    float elevation{};
    int index{};
    bool operator>(const HydroCell& other) const { return elevation > other.elevation; }
};

WorldGenerationData GenerateWorld() {
    std::random_device rd;
    const std::uint64_t seed =
        (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());

    constexpr int size = kHydrologyResolution;
    const float cellSize = kWorldSize / static_cast<float>(size);
    const int cellCount = size * size;
    std::vector<float> raw(cellCount);
    std::vector<float> filled(cellCount);
    std::vector<int> flow(cellCount, -1);
    std::vector<int> accumulation(cellCount, 1);

    const auto index = [size](int x, int z) { return z * size + x; };
    const auto worldX = [cellSize](int x) { return -kWorldHalfSize + (static_cast<float>(x) + 0.5f) * cellSize; };
    const auto worldZ = [cellSize](int z) { return -kWorldHalfSize + (static_cast<float>(z) + 0.5f) * cellSize; };

    for (int z = 0; z < size; ++z) {
        for (int x = 0; x < size; ++x) {
            raw[index(x, z)] = BaseTerrainHeight(worldX(x), worldZ(z), seed);
        }
    }
    filled = raw;

    // Priority-flood the DEM from the map border and the generated sea.
    // This removes closed pits before D8 routing, so extracted rivers have a
    // continuous downhill destination instead of getting trapped in noise pockets.
    std::priority_queue<HydroCell, std::vector<HydroCell>, std::greater<HydroCell>> open;
    std::vector<unsigned char> closed(cellCount, 0);
    const auto pushSeed = [&](int x, int z) {
        const int i = index(x, z);
        if (closed[i]) return;
        closed[i] = 1;
        open.push({filled[i], i});
    };

    for (int x = 0; x < size; ++x) {
        pushSeed(x, 0);
        pushSeed(x, size - 1);
    }
    for (int z = 0; z < size; ++z) {
        pushSeed(0, z);
        pushSeed(size - 1, z);
    }
    for (int z = 0; z < size; ++z) {
        for (int x = 0; x < size; ++x) {
            if (SeaMask(worldX(x), worldZ(z)) > 0.72f) pushSeed(x, z);
        }
    }

    constexpr int dx8[8]{1, 1, 0, -1, -1, -1, 0, 1};
    constexpr int dz8[8]{0, 1, 1, 1, 0, -1, -1, -1};
    while (!open.empty()) {
        const HydroCell current = open.top();
        open.pop();
        const int cx = current.index % size;
        const int cz = current.index / size;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + dx8[d];
            const int nz = cz + dz8[d];
            if (nx < 0 || nx >= size || nz < 0 || nz >= size) continue;
            const int ni = index(nx, nz);
            if (closed[ni]) continue;
            closed[ni] = 1;
            filled[ni] = std::max(filled[ni], current.elevation + 0.02f);
            open.push({filled[ni], ni});
        }
    }

    // D8 routing: each cell drains to its steepest lower neighbour.
    std::vector<int> order(cellCount);
    for (int i = 0; i < cellCount; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return filled[a] > filled[b]; });

    for (const int i : order) {
        const int cx = i % size;
        const int cz = i / size;
        float bestSlope = 0.0f;
        int best = -1;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + dx8[d];
            const int nz = cz + dz8[d];
            if (nx < 0 || nx >= size || nz < 0 || nz >= size) continue;
            const int ni = index(nx, nz);
            const float distance = (d % 2 == 0) ? 1.0f : 1.41421356f;
            const float slope = (filled[i] - filled[ni]) / distance;
            if (slope > bestSlope) {
                bestSlope = slope;
                best = ni;
            }
        }
        flow[i] = best;
        if (best >= 0) accumulation[best] += accumulation[i];
    }

    WorldGenerationData world{};
    world.seed = seed;

    // Pick high, high-catchment cells as river headwaters. Enforce large
    // separation so the four rivers represent separate watersheds.
    std::vector<int> candidates(order.begin(), order.end());
    std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
        const float scoreA = static_cast<float>(accumulation[a]) * (1.0f + std::max(raw[a], 0.0f) / 350.0f);
        const float scoreB = static_cast<float>(accumulation[b]) * (1.0f + std::max(raw[b], 0.0f) / 350.0f);
        return scoreA > scoreB;
    });

    std::vector<int> sources;
    for (const int source : candidates) {
        const int sx = source % size;
        const int sz = source / size;
        const float x = worldX(sx);
        const float z = worldZ(sz);
        if (raw[source] < 150.0f || SeaMask(x, z) > 0.05f || accumulation[source] < 12) continue;

        // Only accept headwaters whose actual D8 route reaches the generated sea.
        int probe = source;
        bool reachesSea = false;
        for (int step = 0; step < cellCount; ++step) {
            const int px = probe % size;
            const int pz = probe / size;
            if (SeaMask(worldX(px), worldZ(pz)) > kSeaWaterThreshold) {
                reachesSea = true;
                break;
            }
            const int next = flow[probe];
            if (next < 0 || next == probe) break;
            probe = next;
        }
        if (!reachesSea) continue;

        bool separated = true;
        for (const int selected : sources) {
            const float ddx = x - worldX(selected % size);
            const float ddz = z - worldZ(selected / size);
            if (ddx * ddx + ddz * ddz < 3200.0f * 3200.0f) {
                separated = false;
                break;
            }
        }
        if (!separated) continue;

        sources.push_back(source);
        if (static_cast<int>(sources.size()) == kRiverCount) break;
    }

    for (int riverIndex = 0; riverIndex < kRiverCount; ++riverIndex) {
        auto& river = world.rivers[riverIndex];
        if (riverIndex >= static_cast<int>(sources.size())) break;

        int cell = sources[riverIndex];
        std::vector<int> trace;
        trace.reserve(cellCount / 2);
        while (cell >= 0 && static_cast<int>(trace.size()) < cellCount) {
            trace.push_back(cell);
            const int cx = cell % size;
            const int cz = cell / size;
            if (SeaMask(worldX(cx), worldZ(cz)) > kSeaWaterThreshold) break;
            const int next = flow[cell];
            if (next < 0 || next == cell) break;
            cell = next;
        }

        const int count = static_cast<int>(trace.size());
        if (count < 2) continue;

        // Compress the full D8 drainage path to a compact spline-like polyline.
        // This keeps long rivers long without making TerrainHeight expensive.
        river.pathCount = std::min(kRiverPathPoints, count);
        for (int i = 0; i < river.pathCount; ++i) {
            const float t = static_cast<float>(i) /
                static_cast<float>(river.pathCount - 1);
            const int traceIndex = static_cast<int>(
                std::round(t * static_cast<float>(count - 1)));
            const int tracedCell = trace[traceIndex];
            const int cx = tracedCell % size;
            const int cz = tracedCell / size;
            river.path[i] = {worldX(cx), 0.0f, worldZ(cz)};
        }

        // Keep the traced order: source -> downstream mouth.
        // The D8 route is the hydrological truth; it is kept as control points
        // and converted to a smooth centripetal curve only when sampled.
        const float sourceHeight = BaseTerrainHeight(
            river.path[0].x, river.path[0].z, seed);
        float previousWater = sourceHeight - 10.0f;

        for (int i = 0; i < river.pathCount; ++i) {
            const float t = static_cast<float>(i) /
                static_cast<float>(river.pathCount - 1);
            const float controlGround = BaseTerrainHeight(
                river.path[i].x, river.path[i].z, seed);

            // Width grows downstream, but not so aggressively that every river
            // becomes an oversized canal.
            river.widths[i] = 18.0f + 92.0f * std::pow(t, 1.65f);

            // Start below the headwater and converge smoothly toward sea level.
            // The extra margin leaves a real water column above the carved bed.
            const float desired = (sourceHeight - 10.0f) * (1.0f - t) +
                (kSeaLevel + 1.2f) * t;
            const float water = std::min({
                desired,
                controlGround - 2.5f,
                previousWater - (i == 0 ? 0.0f : 0.28f)
            });
            river.waterLevels[i] = std::max(kSeaLevel + 1.2f, water);
            previousWater = river.waterLevels[i];
        }
        // The final river station is exactly the sea surface, so the mouth
        // cannot end on a visible vertical step or a strip of dry land.
        river.waterLevels[river.pathCount - 1] = kSeaLevel + 0.8f;
    }

    return world;
}

const WorldGenerationData& GetWorldGeneration() {
    static const WorldGenerationData world = GenerateWorld();
    return world;
}

float DistanceToSegment2D(
    float x, float z,
    const city::math::Vector3& a,
    const city::math::Vector3& b,
    float& t);

city::math::Vector3 RiverCurvePoint(
    const RiverDefinition& river,
    float pathT) {
    if (river.pathCount <= 1) return river.path[0];

    const float scaled = std::clamp(pathT, 0.0f, 1.0f) *
        static_cast<float>(river.pathCount - 1);
    const int i = std::min(
        static_cast<int>(scaled),
        river.pathCount - 2);
    const float localT = scaled - static_cast<float>(i);

    const auto pointAt = [&](int index) {
        if (index < 0) {
            const auto& p0 = river.path[0];
            const auto& p1 = river.path[1];
            return city::math::Vector3{
                p0.x - (p1.x - p0.x),
                0.0f,
                p0.z - (p1.z - p0.z)
            };
        }
        if (index >= river.pathCount) {
            const auto& p0 = river.path[river.pathCount - 2];
            const auto& p1 = river.path[river.pathCount - 1];
            return city::math::Vector3{
                p1.x + (p1.x - p0.x),
                0.0f,
                p1.z + (p1.z - p0.z)
            };
        }
        return river.path[index];
    };

    const city::math::Vector3 p0 = pointAt(i - 1);
    const city::math::Vector3 p1 = pointAt(i);
    const city::math::Vector3 p2 = pointAt(i + 1);
    const city::math::Vector3 p3 = pointAt(i + 2);

    const auto knotDistance = [](const city::math::Vector3& a,
                                 const city::math::Vector3& b) {
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        return std::sqrt(std::max(dx * dx + dz * dz, 0.0001f));
    };

    const float t0 = 0.0f;
    const float t1 = t0 + std::sqrt(knotDistance(p0, p1));
    const float t2 = t1 + std::sqrt(knotDistance(p1, p2));
    const float t3 = t2 + std::sqrt(knotDistance(p2, p3));
    const float t = t1 + localT * (t2 - t1);

    const auto blend = [](const city::math::Vector3& a,
                          const city::math::Vector3& b,
                          float ta, float tb, float t) {
        const float denominator = std::max(tb - ta, 0.0001f);
        const float wa = (tb - t) / denominator;
        const float wb = (t - ta) / denominator;
        return city::math::Vector3{
            a.x * wa + b.x * wb,
            0.0f,
            a.z * wa + b.z * wb
        };
    };

    const city::math::Vector3 a1 = blend(p0, p1, t0, t1, t);
    const city::math::Vector3 a2 = blend(p1, p2, t1, t2, t);
    const city::math::Vector3 a3 = blend(p2, p3, t2, t3, t);
    const city::math::Vector3 b1 = blend(a1, a2, t0, t2, t);
    const city::math::Vector3 b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

float DistanceToRiverCurve(
    float x, float z,
    const RiverDefinition& river,
    float& pathT) {
    float bestDistance = std::numeric_limits<float>::max();
    pathT = 0.0f;

    for (int sample = 0; sample < kRiverCurveSamples; ++sample) {
        const float t0 = static_cast<float>(sample) /
            static_cast<float>(kRiverCurveSamples);
        const float t1 = static_cast<float>(sample + 1) /
            static_cast<float>(kRiverCurveSamples);
        const auto a = RiverCurvePoint(river, t0);
        const auto b = RiverCurvePoint(river, t1);

        float segmentT = 0.0f;
        const float distance = DistanceToSegment2D(x, z, a, b, segmentT);
        if (distance < bestDistance) {
            bestDistance = distance;
            pathT = t0 + (t1 - t0) * segmentT;
        }
    }
    return bestDistance;
}

float DistanceToSegment2D(
    float x, float z,
    const city::math::Vector3& a,
    const city::math::Vector3& b,
    float& t) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float lengthSquared = dx * dx + dz * dz;
    t = lengthSquared > 0.001f
        ? std::clamp(((x - a.x) * dx + (z - a.z) * dz) / lengthSquared, 0.0f, 1.0f)
        : 0.0f;
    const float px = a.x + dx * t;
    const float pz = a.z + dz * t;
    const float ddx = x - px;
    const float ddz = z - pz;
    return std::sqrt(ddx * ddx + ddz * ddz);
}

float RiverSampleValue(const RiverDefinition& river, float pathT, const std::array<float, kRiverPathPoints>& values) {
    const float scaled = std::clamp(pathT, 0.0f, 1.0f) * static_cast<float>(river.pathCount - 1);
    const int i0 = static_cast<int>(scaled);
    const int i1 = std::min(i0 + 1, river.pathCount - 1);
    const float t = scaled - static_cast<float>(i0);
    return values[i0] * (1.0f - t) + values[i1] * t;
}

float WaterLevelAt(float x, float z) {
    // The same terrain vertex can represent land, river water, or sea water.
    // Returning a value well below the terrain for dry land makes waterDepth
    // a signed state that the two material passes can use without a second mesh.
    float waterLevel = kSeaLevel - 1000.0f;

    if (SeaMask(x, z) >= kSeaWaterThreshold)
        waterLevel = kSeaLevel + 0.8f;

    const auto& world = GetWorldGeneration();
    for (const auto& river : world.rivers) {
        if (river.pathCount < 2) continue;

        float nearestT = 0.0f;
        const float nearestDistance = DistanceToRiverCurve(
            x, z, river, nearestT);
        const float riverWidth = RiverSampleValue(
            river, nearestT, river.widths);
        const float waterFootprint = std::max(riverWidth * 1.70f, 95.0f);

        if (nearestDistance <= waterFootprint) {
            waterLevel = std::max(
                waterLevel,
                RiverSampleValue(river, nearestT, river.waterLevels));
        }
    }

    return waterLevel;
}

float TerrainHeight(float x, float z) {
    const auto& world = GetWorldGeneration();
    float height = BaseTerrainHeight(x, z, world.seed);

    for (const auto& river : world.rivers) {
        if (river.pathCount < 2) continue;
        float nearestT = 0.0f;
        const float nearestDistance = DistanceToRiverCurve(
            x, z, river, nearestT);

        // The visible water footprint is deliberately carved a little wider
        // than the rendered ribbon. This prevents the terrain triangulation
        // from peeking through the shoreline at any camera distance.
        const float riverWidth = RiverSampleValue(river, nearestT, river.widths);
        // The terrain grid is roughly 117 m per cell at 256x256. Keep the
        // generated river narrow in world terms, but wide enough to occupy
        // actual terrain vertices after water becomes a terrain state.
        const float waterWidth = std::max(riverWidth * 1.08f, 55.0f);
        const float outerWidth = std::max(riverWidth * 1.70f, 95.0f);
        if (nearestDistance >= outerWidth) continue;

        const float waterLevel = RiverSampleValue(
            river, nearestT, river.waterLevels);
        const float bedDepth = 12.0f + 22.0f * std::pow(nearestT, 0.85f);
        const float riverBed = waterLevel - bedDepth;

        if (nearestDistance <= waterWidth) {
            const float across = nearestDistance /
                std::max(waterWidth, 1.0f);

            // Even at the water's edge the floor stays below the surface.
            // The center gets a deeper V-shaped channel, giving the river
            // visible depth without ever exposing the bottom through water.
            const float centerDepth =
                riverBed - 2.5f * (1.0f - across * across);
            height = std::min(height, centerDepth);
        } else {
            const float bankT =
                (nearestDistance - waterWidth) /
                (outerWidth - waterWidth);
            const float smooth = bankT * bankT * (3.0f - 2.0f * bankT);
            const float bankFloor = waterLevel - 0.75f +
                1.75f * smooth;
            height = std::min(
                height,
                riverBed * (1.0f - smooth) +
                std::max(height, bankFloor) * smooth);
        }
    }

    return height;
}

city::math::Vector3 TerrainNormal(float x, float z) {
    constexpr float sampleDistance = 6.0f;
    const float dx = TerrainHeight(x + sampleDistance, z) - TerrainHeight(x - sampleDistance, z);
    const float dz = TerrainHeight(x, z + sampleDistance) - TerrainHeight(x, z - sampleDistance);
    return city::math::Normalize({-dx, sampleDistance * 2.0f, -dz});
}

float TerrainNoise(float x, float z) {
    return FbmNoise(x, z, GetWorldGeneration().seed);
}

city::math::Vector3 ResolveCameraTerrainCollision(
    const city::math::Vector3& target,
    const city::math::Vector3& desiredPosition) {
    constexpr float clearance = 2.0f;
    constexpr int samples = 64;

    const city::math::Vector3 direction = desiredPosition - target;

    // Follow the highest terrain surface along the camera orbit path.
    // This keeps the camera above mountain slopes instead of simply stopping
    // when the desired orbit point intersects the terrain.
    float highestSurface = TerrainHeight(target.x, target.z);
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const float x = target.x + direction.x * t;
        const float z = target.z + direction.z * t;
        highestSurface = std::max(highestSurface, TerrainHeight(x, z));
    }

    city::math::Vector3 corrected = desiredPosition;
    corrected.y = std::max(corrected.y, highestSurface + clearance);
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
    ComPtr<ID3DBlob> waterVertexShader;
    ComPtr<ID3DBlob> waterPixelShader;
    ComPtr<ID3DBlob> shadowVertexShader;
    const auto shaderPath = ShaderPath();
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexShader, &errors));
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelShader, &errors));
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "WaterVS", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &waterVertexShader, &errors));
    ThrowIfFailed(D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSWater", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &waterPixelShader, &errors));
    const auto shadowShaderPath = ShadowShaderPath();
    ThrowIfFailed(D3DCompileFromFile(shadowShaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowVS", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &shadowVertexShader, &errors));

    // ---------- Input layout ----------
    const std::array<D3D12_INPUT_ELEMENT_DESC, 4> inputLayout{{
        {"POSITION",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",      0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",     0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"WATERDEPTH", 0, DXGI_FORMAT_R32_FLOAT,        0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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

    auto waterPipelineDescription = pipelineDescription;
    waterPipelineDescription.VS = {waterVertexShader->GetBufferPointer(), waterVertexShader->GetBufferSize()};
    waterPipelineDescription.PS = {waterPixelShader->GetBufferPointer(), waterPixelShader->GetBufferSize()};
    // Water is a separate transparent surface. It is drawn after opaque
    // geometry, tests the existing depth buffer, but never overwrites it.
    auto& waterBlend = waterPipelineDescription.BlendState.RenderTarget[0];
    waterBlend.BlendEnable = TRUE;
    waterBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    waterBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    waterBlend.BlendOp = D3D12_BLEND_OP_ADD;
    waterBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    waterBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    waterBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    waterPipelineDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&waterPipelineDescription, IID_PPV_ARGS(&waterPipelineState_)));

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

    constexpr float terrainSize = kWorldSize;
    const float cellSize = terrainSize / kTerrainResolution;
    const auto appendTerrainVertex = [&vertices](float x, float z) {
        const float height = TerrainHeight(x, z);
        // Water is rendered by continuous spline/shore meshes now. Keep the
        // terrain pass purely land so the water cannot inherit terrain-grid seams.
        constexpr float waterDepth = -1.0f;
        const auto normal = TerrainNormal(x, z);
        const float slope = 1.0f - normal.y;
        const bool riverSandBank = height < 4.0f && height > -18.0f;
        const std::array<float, 3> baseColor =
            (height < kSeaLevel + 7.0f || riverSandBank) ? std::array<float, 3>{0.76f, 0.67f, 0.35f}
            : (slope > 0.24f || height > 420.0f) ? std::array<float, 3>{0.42f, 0.43f, 0.40f}
            : (slope > 0.12f) ? std::array<float, 3>{0.38f, 0.30f, 0.18f}
            : std::array<float, 3>{0.20f, 0.55f, 0.22f};

        // Add subtle, deterministic RGB variation so large terrain areas are not perfectly flat.
        const float noise = TerrainNoise(x * 0.004f, z * 0.004f);
        const float redNoise = TerrainNoise(x * 0.007f + 17.0f, z * 0.007f - 31.0f);
        const float greenNoise = TerrainNoise(x * 0.007f - 43.0f, z * 0.007f + 7.0f);
        const float blueNoise = TerrainNoise(x * 0.007f + 61.0f, z * 0.007f + 29.0f);
        std::array<float, 3> color{
            baseColor[0] * (1.0f + noise * 0.08f) + redNoise * 0.025f,
            baseColor[1] * (1.0f + noise * 0.08f) + greenNoise * 0.025f,
            baseColor[2] * (1.0f + noise * 0.08f) + blueNoise * 0.025f
        };

        // Snow gradually appears on high, flatter mountain surfaces, with a little noise
        // to keep the snow line irregular instead of producing a hard horizontal cutoff.
        const float snowHeight = std::clamp((height - 650.0f) / 240.0f, 0.0f, 1.0f);
        const float snowSlope = std::clamp(1.0f - std::max(slope - 0.10f, 0.0f) / 0.34f, 0.0f, 1.0f);
        const float snowCoverage = snowHeight * snowSlope * (0.82f + 0.18f * (noise + 1.0f) * 0.5f);
        color[0] = color[0] * (1.0f - snowCoverage) + snowCoverage;
        color[1] = color[1] * (1.0f - snowCoverage) + snowCoverage;
        color[2] = color[2] * (1.0f - snowCoverage) + snowCoverage;

        for (float& channel : color) channel = std::clamp(channel, 0.0f, 1.0f);
        vertices.push_back({
            {x, height, z},
            {color[0], color[1], color[2]},
            {normal.x, normal.y, normal.z},
            waterDepth
        });
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

    // ---------- Continuous water bodies ----------
    // Rivers are swept as one shared ribbon per river. There are no terrain
    // tiles involved in the shoreline itself: every bend is sampled from the
    // same Catmull-Rom curve and neighboring stations reuse the same boundary
    // vertices, so the bank stays continuous through sharp turns.
    std::vector<Vertex> waterVertices;
    waterVertices.reserve(4 * 129 * 9 + 100);

    const auto appendWaterVertex = [&waterVertices](float x, float y, float z) {
        const float terrainHeight = TerrainHeight(x, z);
        const float waterDepth = std::max(y - terrainHeight, 0.05f);
        waterVertices.push_back({
            {x, y, z},
            {0.08f, 0.46f, 0.47f},
            {0.0f, 1.0f, 0.0f},
            waterDepth
        });
    };

    constexpr int riverSurfaceSamples = 128;
    constexpr int riverWidthSamples = 8;
    for (const auto& river : GetWorldGeneration().rivers) {
        if (river.pathCount < 2) continue;

        std::vector<Vertex> riverGrid(
            static_cast<size_t>(riverSurfaceSamples + 1) *
            static_cast<size_t>(riverWidthSamples + 1));

        for (int i = 0; i <= riverSurfaceSamples; ++i) {
            const float pathT = static_cast<float>(i) /
                static_cast<float>(riverSurfaceSamples);
            const auto center = RiverCurvePoint(river, pathT);

            const float tangentStep = 1.0f / static_cast<float>(riverSurfaceSamples);
            const auto before = RiverCurvePoint(
                river, std::max(0.0f, pathT - tangentStep));
            const auto after = RiverCurvePoint(
                river, std::min(1.0f, pathT + tangentStep));
            const float tangentX = after.x - before.x;
            const float tangentZ = after.z - before.z;
            const float tangentLength = std::max(
                std::sqrt(tangentX * tangentX + tangentZ * tangentZ), 0.001f);
            const float sideX = tangentZ / tangentLength;
            const float sideZ = -tangentX / tangentLength;

            const float riverWidth = RiverSampleValue(
                river, pathT, river.widths);
            const float waterLevel = RiverSampleValue(
                river, pathT, river.waterLevels) + 0.04f;

            for (int j = 0; j <= riverWidthSamples; ++j) {
                const float across = static_cast<float>(j) /
                    static_cast<float>(riverWidthSamples) * 2.0f - 1.0f;
                const float x = center.x + sideX * riverWidth * across;
                const float z = center.z + sideZ * riverWidth * across;
                const size_t gridIndex =
                    static_cast<size_t>(i) * static_cast<size_t>(riverWidthSamples + 1) +
                    static_cast<size_t>(j);
                const float terrainHeight = TerrainHeight(x, z);
                riverGrid[gridIndex] = {
                    {x, waterLevel, z},
                    {0.08f, 0.46f, 0.47f},
                    {0.0f, 1.0f, 0.0f},
                    std::max(waterLevel - terrainHeight, 0.05f)
                };
            }
        }

        for (int i = 0; i < riverSurfaceSamples; ++i) {
            for (int j = 0; j < riverWidthSamples; ++j) {
                const Vertex& a = riverGrid[
                    static_cast<size_t>(i) * (riverWidthSamples + 1) + j];
                const Vertex& b = riverGrid[
                    static_cast<size_t>(i + 1) * (riverWidthSamples + 1) + j];
                const Vertex& c = riverGrid[
                    static_cast<size_t>(i + 1) * (riverWidthSamples + 1) + j + 1];
                const Vertex& d = riverGrid[
                    static_cast<size_t>(i) * (riverWidthSamples + 1) + j + 1];
                // The water buffer is a non-indexed triangle list. Both
                // triangles use the exact same grid coordinates, so adjacent
                // quads cannot open a visible crack.
                waterVertices.push_back(a);
                waterVertices.push_back(b);
                waterVertices.push_back(c);
                waterVertices.push_back(a);
                waterVertices.push_back(c);
                waterVertices.push_back(d);
            }
        }
    }

    // The sea is a single quarter-ellipse fan attached to the world corner,
    // rather than a rectangular grid. A low-frequency radial warp makes the
    // coastline organic while keeping the surface topologically continuous.
    constexpr int seaArcSamples = 128;
    constexpr float seaCenterX = -15000.0f;
    constexpr float seaCenterZ = 15000.0f;
    const float seaLevel = kSeaLevel + 0.8f + 0.04f;
    const size_t seaCenterIndex = waterVertices.size();
    appendWaterVertex(seaCenterX, seaLevel, seaCenterZ);
    std::array<UINT, seaArcSamples + 1> seaArc{};
    for (int i = 0; i <= seaArcSamples; ++i) {
        const float angle = (std::numbers::pi_v<float> * 0.5f) *
            static_cast<float>(i) / static_cast<float>(seaArcSamples);
        const float dirX = std::cos(angle);
        const float dirZ = -std::sin(angle);

        // Find the exact shoreline used by the terrain's SeaMask instead of
        // approximating it with a second, slightly different ellipse. A few
        // cheap bisection steps give us a smooth continuous coast that matches
        // the generated seabed all the way around the mouth region.
        float lowRadius = 0.0f;
        float highRadius = 14000.0f;
        for (int iteration = 0; iteration < 14; ++iteration) {
            const float radius = (lowRadius + highRadius) * 0.5f;
            const float x = seaCenterX + dirX * radius;
            const float z = seaCenterZ + dirZ * radius;
            if (SeaMask(x, z) >= kSeaWaterThreshold)
                lowRadius = radius;
            else
                highRadius = radius;
        }
        const float radius = (lowRadius + highRadius) * 0.5f;
        const float x = seaCenterX + dirX * radius;
        const float z = seaCenterZ + dirZ * radius;
        seaArc[i] = static_cast<UINT>(waterVertices.size());
        appendWaterVertex(x, seaLevel, z);
    }
    for (int i = 0; i < seaArcSamples; ++i) {
        const Vertex center = waterVertices[seaCenterIndex];
        const Vertex a = waterVertices[seaArc[i]];
        const Vertex b = waterVertices[seaArc[i + 1]];
        waterVertices.push_back(center);
        waterVertices.push_back(a);
        waterVertices.push_back(b);
    }

    const UINT waterBufferSize = static_cast<UINT>(waterVertices.size() * sizeof(Vertex));
    bufferDescription.Width = waterBufferSize;
    ThrowIfFailed(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&waterVertexBuffer_)));
    void* waterVertexData{};
    ThrowIfFailed(waterVertexBuffer_->Map(0, nullptr, &waterVertexData));
    std::memcpy(waterVertexData, waterVertices.data(), waterBufferSize);
    waterVertexBuffer_->Unmap(0, nullptr);
    waterVertexBufferView_ = {
        waterVertexBuffer_->GetGPUVirtualAddress(),
        waterBufferSize,
        sizeof(Vertex)
    };
    waterVertexCount_ = static_cast<UINT>(waterVertices.size());

    // ---------- Constant buffer ----------
    bufferDescription.Width = sizeof(SceneConstants);
    ThrowIfFailed(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer_)));
    ThrowIfFailed(constantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedConstants_)));
}

void Renderer::UpdateCamera(float deltaSeconds) {
    // Pan speed grows non-linearly with zoom distance. At close range the
    // camera stays controllable, while at the far 30 km view it can cross
    // the whole map in only a few seconds.
    constexpr float baseMoveSpeed = 8.0f;
    constexpr float referenceOrbitDistance = 18.0f;
    constexpr float distantSpeedExponent = 1.10f;
    constexpr float maxMoveSpeedMultiplier = 750.0f;
    const float normalizedDistance = std::max(
        cameraOrbitDistance_ / referenceOrbitDistance,
        1.0f);
    const float zoomSpeedMultiplier = std::clamp(
        std::pow(normalizedDistance, distantSpeedExponent),
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
    constexpr float zoomKeySpeed = 600.0f;
    if (GetAsyncKeyState('T') & 0x8000) cameraOrbitDistance_ -= zoomKeySpeed * deltaSeconds;
    if (GetAsyncKeyState('G') & 0x8000) cameraOrbitDistance_ += zoomKeySpeed * deltaSeconds;
    cameraOrbitDistance_ = std::clamp(cameraOrbitDistance_, 4.0f, 15000.0f);

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
    cameraOrbitDistance_ = std::clamp(cameraOrbitDistance_, 4.0f, 15000.0f);

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
        math::Matrix4::Perspective(fieldOfView_, static_cast<float>(width_) / height_, 1.0f, 40000.0f));

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
    mappedConstants_->timeSeconds += deltaSeconds;
    mappedConstants_->cameraPosition[0] = cameraPosition_.x;
    mappedConstants_->cameraPosition[1] = cameraPosition_.y;
    mappedConstants_->cameraPosition[2] = cameraPosition_.z;
    mappedConstants_->cameraOrbitDistance = cameraOrbitDistance_;

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

    // Water is one continuous set of spline/shore surfaces, not a copy of
    // the terrain grid. This keeps the shoreline smooth even though the
    // landscape itself remains a relatively coarse 256x256 height field.
    commandList_->SetPipelineState(waterPipelineState_.Get());
    commandList_->IASetVertexBuffers(0, 1, &waterVertexBufferView_);
    commandList_->DrawInstanced(waterVertexCount_, 1, 0, 0);

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
