#pragma once

#include <cmath>

namespace city::math {

struct Matrix4 final {
    float values[16]{};

    static Matrix4 Multiply(const Matrix4& a, const Matrix4& b) {
        Matrix4 result{};
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                for (int k = 0; k < 4; ++k) {
                    result.values[row * 4 + column] += a.values[row * 4 + k] * b.values[k * 4 + column];
                }
            }
        }
        return result;
    }

    static Matrix4 RotationY(float radians) {
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        return {{cosine, 0.0f, -sine, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                 sine, 0.0f, cosine, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
    }

    static Matrix4 RotationX(float radians) {
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        return {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cosine, sine, 0.0f,
                 0.0f, -sine, cosine, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
    }

    static Matrix4 Translation(float x, float y, float z) {
        return {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                 0.0f, 0.0f, 1.0f, 0.0f, x, y, z, 1.0f}};
    }

    static Matrix4 Perspective(float fovY, float aspectRatio, float nearPlane, float farPlane) {
        const float yScale = 1.0f / std::tan(fovY * 0.5f);
        const float xScale = yScale / aspectRatio;
        return {{xScale, 0.0f, 0.0f, 0.0f, 0.0f, yScale, 0.0f, 0.0f,
                 0.0f, 0.0f, farPlane / (farPlane - nearPlane), 1.0f,
                 0.0f, 0.0f, -nearPlane * farPlane / (farPlane - nearPlane), 0.0f}};
    }
};

} // namespace city::math
