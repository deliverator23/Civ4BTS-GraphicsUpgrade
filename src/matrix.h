// Small 4x4 helpers in "register" form: a matrix is 16 floats, row r = shader register r, and output component
// r = row r . (x, y, z, 1). That is how both games' vertex shaders consume mtxViewProj / mtxWorldViewProj, so these
// work directly on constants read back from the device.
#pragma once

#include <cmath>
#include <utility>

namespace mat
{
// out = a * b (apply b first, then a).
inline void Mul(const float* a, const float* b, float* out)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    for (int i = 0; i < 16; ++i)
        out[i] = r[i];
}

// Inverse by Gauss-Jordan elimination (double precision); false if singular.
inline bool Invert(const float* m, double* out)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c)
            a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col]))
                pivot = r;
        if (std::fabs(a[pivot][col]) < 1e-12)
            return false;
        for (int c = 0; c < 8; ++c)
            std::swap(a[col][c], a[pivot][c]);
        double d = a[col][col];
        for (int c = 0; c < 8; ++c)
            a[col][c] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != col && a[r][col] != 0.0)
            {
                double f = a[r][col];
                for (int c = 0; c < 8; ++c)
                    a[r][c] -= f * a[col][c];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = a[r][c + 4];
    return true;
}

inline bool Invert(const float* m, float* out)
{
    double d[16];
    if (!Invert(m, d))
        return false;
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<float>(d[i]);
    return true;
}

// Mirror a view-projection about the horizontal plane z = h, and flip clip-space x so the triangle winding (and so
// the cull mode) is unchanged, as Colonization's reflection pass does: in each row the z coefficient is negated and
// w gains 2h * (old z coefficient); then row 0 is negated.
inline void MirrorViewProj(const float* vp, float h, float* out)
{
    for (int r = 0; r < 4; ++r)
    {
        float z = vp[r * 4 + 2];
        out[r * 4 + 0] = vp[r * 4 + 0];
        out[r * 4 + 1] = vp[r * 4 + 1];
        out[r * 4 + 2] = -z;
        out[r * 4 + 3] = vp[r * 4 + 3] + 2.0f * h * z;
    }
    for (int c = 0; c < 4; ++c)
        out[c] = -out[c];
}

// World -> projective texture coordinates for a texture rendered with view-projection vp: u = 0.5x + 0.5w,
// v = -0.5y + 0.5w (the shader divides by w). Colonization's mtxReflection / mtxRefraction have this form.
inline void TextureProjection(const float* vp, float* out)
{
    for (int c = 0; c < 4; ++c)
    {
        out[0 * 4 + c] = 0.5f * vp[0 * 4 + c] + 0.5f * vp[3 * 4 + c];
        out[1 * 4 + c] = -0.5f * vp[1 * 4 + c] + 0.5f * vp[3 * 4 + c];
        out[2 * 4 + c] = vp[2 * 4 + c];
        out[3 * 4 + c] = vp[3 * 4 + c];
    }
}

// Camera position of a perspective view-projection: the point where clip x, y and w are all 0.
inline bool EyePosition(const float* vp, float* eye)
{
    const float* r[3] = {vp, vp + 4, vp + 12};
    double m[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            m[i][j] = r[i][j];
    // Solve m[i][0..2] . e = -m[i][3] by Cramer's rule.
    auto det3 = [](double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    double A[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            A[i][j] = m[i][j];
    double D = det3(A);
    if (std::fabs(D) < 1e-12)
        return false;
    for (int k = 0; k < 3; ++k)
    {
        double B[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                B[i][j] = j == k ? -m[i][3] : m[i][j];
        eye[k] = static_cast<float>(det3(B) / D);
    }
    return true;
}
}
