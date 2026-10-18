#include "museum3d.h"
#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <float.h>
#include <string.h>

/*
 * Tiny software 3D renderer for the Museum diorama.
 *
 * The room, pipes, blocks, coin, train, pedestals and toy figures are real
 * 3D geometry: vertices are transformed through a perspective camera, lit
 * per face, depth-tested, and rasterized into a streaming SDL texture. This
 * deliberately needs no OpenGL context or extra dependency, so the same path
 * works with PipeClean's existing SDL_Renderer on Windows, Linux and Android.
 *
 * The interactive Museum controls and typography remain SDL UI drawn over
 * this scene. The scene is rendered at a high native resolution matched to the launcher’s
 * viewport aspect, then scaled with the exact same logical-to-pixel transform.
 */

#define M3D_W 960
#define M3D_H 628
#define M3D_PIXELS (M3D_W * M3D_H)
#define M3D_NEAR 0.35f
#define HEX(rgb) (0xFF000000u | (uint32_t)(rgb))

typedef struct { float x, y, z; } M3DVec3;
typedef struct { float x, y, invz, z; } M3DProjected;

static uint32_t m3d_pixels[M3D_PIXELS];
static float m3d_depth[M3D_PIXELS];
static SDL_Texture *m3d_texture;
static SDL_Renderer *m3d_renderer;

static const float m3d_cam_y = 4.6f;
static const float m3d_cam_z = 10.5f;
static const float m3d_pitch_sin = 0.31f;
static const float m3d_pitch_cos = 0.9502f;
static const float m3d_focal = 754.0f;
static const float m3d_center_y = 270.0f;

static M3DVec3 m3d_v(float x, float y, float z)
{
    M3DVec3 v = {x, y, z};
    return v;
}

static M3DVec3 m3d_sub(M3DVec3 a, M3DVec3 b)
{
    return m3d_v(a.x - b.x, a.y - b.y, a.z - b.z);
}

static M3DVec3 m3d_cross(M3DVec3 a, M3DVec3 b)
{
    return m3d_v(a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x);
}

static float m3d_dot(M3DVec3 a, M3DVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static uint32_t m3d_rgb(unsigned r, unsigned g, unsigned b)
{
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t m3d_tint(uint32_t color, float factor)
{
    unsigned r = (color >> 16) & 255u;
    unsigned g = (color >> 8) & 255u;
    unsigned b = color & 255u;
    if (factor < 0.0f) factor = 0.0f;
    return m3d_rgb((unsigned)(r * factor), (unsigned)(g * factor),
                   (unsigned)(b * factor));
}

static void m3d_clear(void)
{
    for (int y = 0; y < M3D_H; ++y) {
        float t = (float)y / (float)M3D_H;
        unsigned r = (unsigned)(33.0f + 20.0f * t);
        unsigned g = (unsigned)(23.0f + 11.0f * t);
        unsigned b = (unsigned)(25.0f + 6.0f * t);
        uint32_t c = m3d_rgb(r, g, b);
        for (int x = 0; x < M3D_W; ++x)
            m3d_pixels[y * M3D_W + x] = c;
    }
    for (int i = 0; i < M3D_PIXELS; ++i)
        m3d_depth[i] = FLT_MAX;
}

static int m3d_project(M3DVec3 p, M3DProjected *out)
{
    float dy = p.y - m3d_cam_y;
    float dz = p.z - m3d_cam_z;
    float view_y = dy * m3d_pitch_cos - dz * m3d_pitch_sin;
    float view_z = (m3d_cam_z - p.z) * m3d_pitch_cos - dy * m3d_pitch_sin;
    if (view_z <= M3D_NEAR) return 0;
    out->x = (float)M3D_W * 0.5f + p.x * m3d_focal / view_z;
    out->y = m3d_center_y - view_y * m3d_focal / view_z;
    out->z = view_z;
    out->invz = 1.0f / view_z;
    return 1;
}

static float m3d_edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void m3d_triangle_raw(M3DProjected a, M3DProjected b, M3DProjected c,
                             uint32_t color)
{
    float area = m3d_edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (fabsf(area) < 0.0001f) return;

    int min_x = (int)floorf(fminf(a.x, fminf(b.x, c.x)));
    int max_x = (int)ceilf(fmaxf(a.x, fmaxf(b.x, c.x)));
    int min_y = (int)floorf(fminf(a.y, fminf(b.y, c.y)));
    int max_y = (int)ceilf(fmaxf(a.y, fmaxf(b.y, c.y)));
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= M3D_W) max_x = M3D_W - 1;
    if (max_y >= M3D_H) max_y = M3D_H - 1;
    if (min_x > max_x || min_y > max_y) return;

    for (int y = min_y; y <= max_y; ++y) {
        float py = (float)y + 0.5f;
        for (int x = min_x; x <= max_x; ++x) {
            float px = (float)x + 0.5f;
            float wa = m3d_edge(b.x, b.y, c.x, c.y, px, py) / area;
            float wb = m3d_edge(c.x, c.y, a.x, a.y, px, py) / area;
            float wc = 1.0f - wa - wb;
            if (wa < -0.0005f || wb < -0.0005f || wc < -0.0005f)
                continue;
            float invz = wa * a.invz + wb * b.invz + wc * c.invz;
            if (invz <= 0.0f) continue;
            float z = 1.0f / invz;
            int index = y * M3D_W + x;
            if (z < m3d_depth[index]) {
                m3d_depth[index] = z;
                m3d_pixels[index] = color;
            }
        }
    }
}

static void m3d_triangle(M3DVec3 a, M3DVec3 b, M3DVec3 c, uint32_t material)
{
    M3DProjected pa, pb, pc;
    if (!m3d_project(a, &pa) || !m3d_project(b, &pb) || !m3d_project(c, &pc))
        return;

    M3DVec3 n = m3d_cross(m3d_sub(b, a), m3d_sub(c, a));
    float length = sqrtf(m3d_dot(n, n));
    if (length < 0.0001f) return;
    n.x /= length; n.y /= length; n.z /= length;
    /* Two-sided, soft studio lighting: all visible faces get a little fill. */
    M3DVec3 light = m3d_v(-0.42f, 0.82f, 0.38f);
    float light_len = sqrtf(m3d_dot(light, light));
    light.x /= light_len; light.y /= light_len; light.z /= light_len;
    float diffuse = fabsf(m3d_dot(n, light));
    float illumination = 0.38f + 0.72f * diffuse;
    if (illumination > 1.18f) illumination = 1.18f;
    m3d_triangle_raw(pa, pb, pc, m3d_tint(material, illumination));
}

static void m3d_quad(M3DVec3 a, M3DVec3 b, M3DVec3 c, M3DVec3 d, uint32_t color)
{
    m3d_triangle(a, b, c, color);
    m3d_triangle(a, c, d, color);
}

static void m3d_box(float x0, float y0, float z0, float x1, float y1, float z1,
                    uint32_t color)
{
    M3DVec3 p000 = m3d_v(x0, y0, z0), p100 = m3d_v(x1, y0, z0);
    M3DVec3 p010 = m3d_v(x0, y1, z0), p110 = m3d_v(x1, y1, z0);
    M3DVec3 p001 = m3d_v(x0, y0, z1), p101 = m3d_v(x1, y0, z1);
    M3DVec3 p011 = m3d_v(x0, y1, z1), p111 = m3d_v(x1, y1, z1);
    m3d_quad(p001, p101, p111, p011, color);
    m3d_quad(p000, p010, p110, p100, m3d_tint(color, 0.54f));
    m3d_quad(p011, p111, p110, p010, m3d_tint(color, 1.16f));
    m3d_quad(p000, p100, p101, p001, m3d_tint(color, 0.48f));
    m3d_quad(p000, p001, p011, p010, m3d_tint(color, 0.72f));
    m3d_quad(p100, p110, p111, p101, m3d_tint(color, 0.82f));
}

static void m3d_disk_horizontal(float cx, float y, float cz, float radius,
                                uint32_t color, int segments)
{
    M3DVec3 center = m3d_v(cx, y, cz);
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 a = m3d_v(cx + cosf(a0) * radius, y,
                          cz + sinf(a0) * radius);
        M3DVec3 b = m3d_v(cx + cosf(a1) * radius, y,
                          cz + sinf(a1) * radius);
        m3d_triangle(center, a, b, color);
    }
}

static void m3d_cylinder_y(float cx, float base_y, float cz, float radius,
                           float height, uint32_t color, int segments)
{
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 b0 = m3d_v(cx + cosf(a0) * radius, base_y,
                           cz + sinf(a0) * radius);
        M3DVec3 b1 = m3d_v(cx + cosf(a1) * radius, base_y,
                           cz + sinf(a1) * radius);
        M3DVec3 t1 = m3d_v(b1.x, base_y + height, b1.z);
        M3DVec3 t0 = m3d_v(b0.x, base_y + height, b0.z);
        float face_tint = 0.82f + 0.18f * (0.5f + 0.5f * cosf(a0));
        m3d_quad(b0, b1, t1, t0, m3d_tint(color, face_tint));
    }
    m3d_disk_horizontal(cx, base_y + height, cz, radius, m3d_tint(color, 1.08f), segments);
}

static void m3d_sphere(float cx, float cy, float cz, float radius,
                       uint32_t color, int longitude_steps, int latitude_steps)
{
    for (int lat = 0; lat < latitude_steps; ++lat) {
        float v0 = -1.57079632679f + (float)lat * 3.14159265359f / (float)latitude_steps;
        float v1 = -1.57079632679f + (float)(lat + 1) * 3.14159265359f / (float)latitude_steps;
        for (int lon = 0; lon < longitude_steps; ++lon) {
            float u0 = (float)lon * 6.28318530718f / (float)longitude_steps;
            float u1 = (float)(lon + 1) * 6.28318530718f / (float)longitude_steps;
            M3DVec3 p00 = m3d_v(cx + radius * cosf(v0) * cosf(u0),
                                cy + radius * sinf(v0),
                                cz + radius * cosf(v0) * sinf(u0));
            M3DVec3 p01 = m3d_v(cx + radius * cosf(v0) * cosf(u1),
                                cy + radius * sinf(v0),
                                cz + radius * cosf(v0) * sinf(u1));
            M3DVec3 p10 = m3d_v(cx + radius * cosf(v1) * cosf(u0),
                                cy + radius * sinf(v1),
                                cz + radius * cosf(v1) * sinf(u0));
            M3DVec3 p11 = m3d_v(cx + radius * cosf(v1) * cosf(u1),
                                cy + radius * sinf(v1),
                                cz + radius * cosf(v1) * sinf(u1));
            m3d_quad(p00, p01, p11, p10, color);
        }
    }
}

static void m3d_coin(float cx, float cy, float cz, float radius, float angle)
{
    const int segments = 18;
    const float thickness = 0.07f;
    float sn = sinf(angle), cs = cosf(angle);
    M3DVec3 front_center = m3d_v(cx - sn * thickness * 0.5f, cy, cz + cs * thickness * 0.5f);
    M3DVec3 back_center  = m3d_v(cx + sn * thickness * 0.5f, cy, cz - cs * thickness * 0.5f);
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 f0 = m3d_v(cx + radius * cosf(a0) * cs - sn * thickness * 0.5f,
                           cy + radius * sinf(a0),
                           cz + radius * cosf(a0) * sn + cs * thickness * 0.5f);
        M3DVec3 f1 = m3d_v(cx + radius * cosf(a1) * cs - sn * thickness * 0.5f,
                           cy + radius * sinf(a1),
                           cz + radius * cosf(a1) * sn + cs * thickness * 0.5f);
        M3DVec3 b0 = m3d_v(cx + radius * cosf(a0) * cs + sn * thickness * 0.5f,
                           cy + radius * sinf(a0),
                           cz + radius * cosf(a0) * sn - cs * thickness * 0.5f);
        M3DVec3 b1 = m3d_v(cx + radius * cosf(a1) * cs + sn * thickness * 0.5f,
                           cy + radius * sinf(a1),
                           cz + radius * cosf(a1) * sn - cs * thickness * 0.5f);
        m3d_triangle(front_center, f0, f1, HEX(0xF6C64E));
        m3d_triangle(back_center, b1, b0, HEX(0x99601B));
        m3d_quad(f0, b0, b1, f1, HEX(0xBA7A20));
    }
    /* A raised polygonal center ring and center boss catch the spotlights. */
    M3DVec3 medallion_center = m3d_v(cx - sn * (thickness * 0.5f + 0.004f),
                                     cy, cz + cs * (thickness * 0.5f + 0.004f));
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 p0 = m3d_v(cx + radius * 0.66f * cosf(a0) * cs - sn * (thickness * 0.5f + 0.005f),
                           cy + radius * 0.66f * sinf(a0),
                           cz + radius * 0.66f * cosf(a0) * sn + cs * (thickness * 0.5f + 0.005f));
        M3DVec3 p1 = m3d_v(cx + radius * 0.66f * cosf(a1) * cs - sn * (thickness * 0.5f + 0.005f),
                           cy + radius * 0.66f * sinf(a1),
                           cz + radius * 0.66f * cosf(a1) * sn + cs * (thickness * 0.5f + 0.005f));
        m3d_triangle(medallion_center, p0, p1, HEX(0xFFE38A));
    }
    m3d_sphere(cx - sn * (thickness * 0.5f + 0.012f), cy,
               cz + cs * (thickness * 0.5f + 0.012f),
               radius * 0.16f, HEX(0xFFF1B3), 8, 4);
}

static void m3d_toy_figure(float x, float z, float bob, uint32_t body, uint32_t hat)
{
    m3d_box(x - 0.46f, 0.02f, z - 0.38f, x + 0.46f, 0.26f, z + 0.38f,
            HEX(0x8F5B2F));
    m3d_box(x - 0.39f, 0.26f, z - 0.31f, x + 0.39f, 0.34f, z + 0.31f,
            HEX(0xE6C17A));
    m3d_sphere(x, 0.73f + bob, z, 0.36f, body, 12, 7);
    m3d_sphere(x, 1.28f + bob, z + 0.02f, 0.31f, HEX(0xEAC99A), 12, 7);
    m3d_sphere(x, 1.57f + bob, z + 0.01f, 0.34f, hat, 12, 5);
    m3d_sphere(x - 0.11f, 1.30f + bob, z + 0.29f, 0.035f, HEX(0x28170F), 6, 3);
    m3d_sphere(x + 0.11f, 1.30f + bob, z + 0.29f, 0.035f, HEX(0x28170F), 6, 3);
}

static void m3d_pipe(float x, float z)
{
    const float base = 0.02f, height = 4.6f, radius = 0.48f;
    m3d_box(x - 0.56f, 0.0f, z - 0.52f, x + 0.56f, 0.11f, z + 0.52f,
            HEX(0x70452A));
    m3d_cylinder_y(x, base, z, radius, height, HEX(0x139B3B), 18);
    m3d_cylinder_y(x, base + height - 0.26f, z, radius + 0.17f, 0.28f,
                   HEX(0x38CE51), 18);
    m3d_disk_horizontal(x, base + height + 0.285f, z, radius * 0.70f,
                        HEX(0x063E1E), 18);
    /* Dark inner throat sits below the bright rim to show an actual opening. */
    m3d_disk_horizontal(x, base + height + 0.19f, z, radius * 0.66f,
                        HEX(0x042914), 18);
    m3d_cylinder_y(x - 0.12f, 0.11f, z + 0.46f, 0.025f, 3.95f,
                   HEX(0xA4F47E), 6);
}

static void m3d_question_block(float cx, float y, float cz, float size, float bob)
{
    float h = size * 0.5f;
    y += bob;
    m3d_box(cx - h, y - h, cz - h, cx + h, y + h, cz + h, HEX(0xEFAE22));
    m3d_box(cx - h + 0.04f, y + h - 0.035f, cz - h + 0.035f,
            cx + h - 0.04f, y + h + 0.006f, cz + h - 0.035f, HEX(0xFFE77A));
    /* The question mark is deliberately represented by raised tiny geometry. */
    m3d_box(cx - 0.07f, y - 0.06f, cz + h + 0.012f,
            cx + 0.07f, y + 0.16f, cz + h + 0.028f, HEX(0xFFF4C8));
    m3d_box(cx + 0.015f, y + 0.07f, cz + h + 0.034f,
            cx + 0.12f, y + 0.15f, cz + h + 0.048f, HEX(0xFFF4C8));
    m3d_sphere(cx, y - 0.16f, cz + h + 0.03f, 0.045f, HEX(0xFFF4C8), 7, 4);
}

static void m3d_train(float elapsed)
{
    float bob = sinf(elapsed * 1.6f) * 0.025f;
    /* Engine and cab: multi-part solid geometry with wheels and a brass lamp. */
    m3d_box(4.6f, 0.28f + bob, 2.7f, 5.65f, 0.78f + bob, 3.25f, HEX(0xC94B31));
    m3d_cylinder_y(4.98f, 0.65f + bob, 2.97f, 0.34f, 0.38f, HEX(0xE3A33A), 12);
    m3d_box(5.34f, 0.58f + bob, 2.78f, 5.69f, 1.25f + bob, 3.17f, HEX(0xE7B948));
    m3d_box(5.42f, 0.84f + bob, 3.18f, 5.61f, 1.10f + bob, 3.205f, HEX(0x4D7890));
    m3d_cylinder_y(5.45f, 1.24f + bob, 2.97f, 0.12f, 0.28f, HEX(0x8A4924), 10);
    m3d_sphere(5.65f, 0.50f + bob, 3.27f, 0.095f, HEX(0xFFE5A0), 8, 4);
    for (int i = 0; i < 3; ++i) {
        float wx = 4.85f + i * 0.35f;
        m3d_sphere(wx, 0.29f + bob, 3.29f, 0.17f, HEX(0x34302A), 10, 6);
        m3d_sphere(wx, 0.29f + bob, 3.32f, 0.075f, HEX(0xB3A28A), 8, 5);
    }
    /* Two little carriages behind the engine, each with a raised rim. */
    for (int car = 0; car < 2; ++car) {
        float cx = 3.45f - car * 1.10f;
        uint32_t paint = car == 0 ? HEX(0x2D9D55) : HEX(0x3975C7);
        m3d_box(cx - 0.43f, 0.32f + bob, 2.78f,
                cx + 0.43f, 0.75f + bob, 3.20f, paint);
        m3d_box(cx - 0.47f, 0.70f + bob, 2.74f,
                cx + 0.47f, 0.80f + bob, 3.24f, HEX(0xE4B653));
        m3d_sphere(cx - 0.22f, 0.29f + bob, 3.24f, 0.14f, HEX(0x34302A), 9, 5);
        m3d_sphere(cx + 0.22f, 0.29f + bob, 3.24f, 0.14f, HEX(0x34302A), 9, 5);
    }
}

static void m3d_scene(float elapsed)
{
    m3d_clear();

    /* Back wall and its panelled timber frame. */
    m3d_box(-8.5f, -0.1f, -4.55f, 8.5f, 8.2f, -4.0f, HEX(0x573523));
    m3d_box(-8.4f, 6.9f, -3.98f, 8.4f, 7.35f, -3.82f, HEX(0x98643C));
    m3d_box(-8.4f, 0.0f, -3.98f, 8.4f, 0.34f, -3.82f, HEX(0x8A572F));
    for (int i = 0; i < 13; ++i) {
        float x = -8.0f + (float)i * 1.33f;
        m3d_box(x, 0.38f, -3.96f, x + 0.045f, 6.87f, -3.84f,
                HEX(0x8B5B37));
    }
    /* A raised brass-edged arch frames the cabinet from behind. */
    m3d_box(-5.5f, 1.0f, -3.72f, -5.17f, 6.0f, -3.47f, HEX(0xBF8E4F));
    m3d_box(5.17f, 1.0f, -3.72f, 5.5f, 6.0f, -3.47f, HEX(0xBF8E4F));
    m3d_box(-5.5f, 5.68f, -3.72f, 5.5f, 6.0f, -3.47f, HEX(0xD8AC65));
    m3d_box(-5.05f, 1.15f, -3.44f, -4.88f, 5.60f, -3.30f, HEX(0x5B3624));
    m3d_box(4.88f, 1.15f, -3.44f, 5.05f, 5.60f, -3.30f, HEX(0x5B3624));

    /* Floor slab and separate boards with real perspective and depth testing. */
    m3d_box(-9.0f, -0.35f, -4.6f, 9.0f, -0.01f, 8.5f, HEX(0x55321F));
    for (int i = 0; i < 17; ++i) {
        float x = -8.4f + (float)i * 1.05f;
        uint32_t wood = (i % 3 == 0) ? HEX(0x7E5332) :
                        (i % 3 == 1) ? HEX(0x684329) : HEX(0x72492C);
        m3d_box(x, 0.002f, -4.4f, x + 0.96f, 0.025f, 8.4f, wood);
        m3d_box(x + 0.96f, 0.003f, -1.5f, x + 0.99f, 0.026f, 8.4f,
                HEX(0x392116));
    }
    m3d_box(-8.8f, 0.03f, 7.5f, 8.8f, 0.10f, 7.7f, HEX(0xA36C3D));

    /* Physical green pipe models sit beside the display housing. */
    m3d_pipe(-6.65f, 0.80f);
    m3d_pipe(6.65f, 0.80f);

    /* Soft-lit museum plinths, then two original stylised toy figures. */
    m3d_box(-7.3f, 0.03f, 1.2f, -5.45f, 0.48f, 2.7f, HEX(0x8C6038));
    m3d_box(5.45f, 0.03f, 1.2f, 7.3f, 0.48f, 2.7f, HEX(0x8C6038));
    m3d_toy_figure(-6.38f, 1.82f, sinf(elapsed * 1.25f) * 0.035f,
                   HEX(0x398BC1), HEX(0x49A85E));
    m3d_toy_figure(6.38f, 1.82f, sinf(elapsed * 1.25f + 1.0f) * 0.035f,
                   HEX(0xB53C31), HEX(0xE5B64A));

    /* Floating question blocks are bevelled solids, not flat UI glyphs. */
    float block_bob = sinf(elapsed * 1.25f) * 0.09f;
    m3d_question_block(-4.5f, 4.55f, -0.75f, 0.77f, block_bob);
    m3d_question_block(4.45f, 4.30f, -0.80f, 0.88f, -block_bob * 0.72f);

    /* A faceted, rotating gold coin is a thin extruded 3D disk. */
    m3d_coin(-3.6f, 3.72f + sinf(elapsed * 1.7f) * 0.08f,
             0.2f, 0.47f, elapsed * 1.15f);

    /* Track, sleeper bars, and moving multi-part toy train. */
    m3d_box(3.0f, 0.025f, 3.38f, 8.5f, 0.09f, 3.65f, HEX(0x8A572F));
    for (int i = 0; i < 24; ++i) {
        float x = 2.8f + (float)i * 0.25f;
        m3d_box(x, 0.04f, 3.19f, x + 0.06f, 0.15f, 3.84f, HEX(0xB17A49));
    }
    m3d_box(2.8f, 0.09f, 3.22f, 8.6f, 0.14f, 3.28f, HEX(0xBFC0B8));
    m3d_box(2.8f, 0.09f, 3.76f, 8.6f, 0.14f, 3.82f, HEX(0x6E7477));
    m3d_train(elapsed);

    /* Small brass exhibit lights hang high on each side of the room. */
    m3d_box(-6.6f, 5.7f, -3.4f, -5.9f, 5.85f, -3.05f, HEX(0xB5803D));
    m3d_sphere(-6.25f, 5.48f, -3.15f, 0.24f, HEX(0xFFD88A), 10, 5);
    m3d_box(5.9f, 5.7f, -3.4f, 6.6f, 5.85f, -3.05f, HEX(0xB5803D));
    m3d_sphere(6.25f, 5.48f, -3.15f, 0.24f, HEX(0xFFD88A), 10, 5);
}

void museum3d_draw_background(SDL_Renderer *renderer,
                              float x, float y, float width, float height,
                              float elapsed_seconds)
{
    if (!renderer || width <= 0.0f || height <= 0.0f) return;

    if (m3d_renderer != renderer) {
        if (m3d_texture) SDL_DestroyTexture(m3d_texture);
        m3d_texture = NULL;
        m3d_renderer = renderer;
    }
    if (!m3d_texture) {
        m3d_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, M3D_W, M3D_H);
        if (!m3d_texture) return;
        SDL_SetTextureBlendMode(m3d_texture, SDL_BLENDMODE_NONE);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(m3d_texture, SDL_ScaleModeLinear);
#endif
    }

    m3d_scene(elapsed_seconds);
    if (SDL_UpdateTexture(m3d_texture, NULL, m3d_pixels, M3D_W * (int)sizeof(uint32_t)) != 0)
        return;

    /* UI primitives use dp/logical coordinates and transform them to renderer
     * pixels with the current ui_scale plus the centered viewport offset.
     * RenderCopy bypasses those helpers, so apply that exact transform here.
     * Without this, maximizing/resizing moves the 3D image relative to the UI. */
    const float scale = ui_scale > 0.0f ? ui_scale : 1.0f;
    const float view_x = ui_view_x0();
    const float view_y = ui_view_y0();
    SDL_Rect dst;
    dst.x = (int)lroundf((x - 29.0f - view_x) * scale);
    dst.y = (int)lroundf((y - 18.0f - view_y) * scale);
    dst.w = (int)lroundf((width + 58.0f) * scale);
    dst.h = (int)lroundf((height + 36.0f) * scale);
    SDL_RenderCopy(renderer, m3d_texture, NULL, &dst);
}
