#pragma once

#include <cmath>

namespace city::math {

struct Vector3 final {
    float x{};
    float y{};
    float z{};

    Vector3 operator+(const Vector3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vector3 operator-(const Vector3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vector3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
};

inline float Dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vector3 Cross(const Vector3& a, const Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vector3 Normalize(const Vector3& value) {
    const float length = std::sqrt(Dot(value, value));
    return length > 0.0001f ? value * (1.0f / length) : Vector3{};
}

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

    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up) {
        const Vector3 forward = Normalize(target - eye);
        const Vector3 right = Normalize(Cross(up, forward));
        const Vector3 correctedUp = Cross(forward, right);
        return {{right.x, correctedUp.x, forward.x, 0.0f,
                 right.y, correctedUp.y, forward.y, 0.0f,
                 right.z, correctedUp.z, forward.z, 0.0f,
                 -Dot(right, eye), -Dot(correctedUp, eye), -Dot(forward, eye), 1.0f}};
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
