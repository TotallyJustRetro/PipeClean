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

typedef struct {
    float view_x, view_y, view_w, view_h;
    float museum_x, museum_y, scroll;
    const int *entry_locked;
    int visible_entries;
    int draw_cubby_grid;
} M3DMuseumLayout;

static uint32_t m3d_pixels[M3D_PIXELS];
static float m3d_depth[M3D_PIXELS];
static SDL_Texture *m3d_texture;
static SDL_Renderer *m3d_renderer;
/* Temporary model-space translation used for the track and moving train.
   Applied at projection time so normals and model-local geometry stay intact. */
static float m3d_translate_x;
static float m3d_translate_z;
static int m3d_clip_active;
static int m3d_clip_left, m3d_clip_top, m3d_clip_right, m3d_clip_bottom;

static const float m3d_cam_y = 4.6f;
static const float m3d_cam_z = 10.5f;
static const float m3d_pitch_sin = 0.31f;
static const float m3d_pitch_cos = 0.9502f;
static const float m3d_focal = 754.0f;
static float m3d_focal_x = 754.0f;
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
    p.x += m3d_translate_x;
    p.z += m3d_translate_z;
    float dy = p.y - m3d_cam_y;
    float dz = p.z - m3d_cam_z;
    float view_y = dy * m3d_pitch_cos - dz * m3d_pitch_sin;
    float view_z = (m3d_cam_z - p.z) * m3d_pitch_cos - dy * m3d_pitch_sin;
    if (view_z <= M3D_NEAR) return 0;
    out->x = (float)M3D_W * 0.5f + p.x * m3d_focal_x / view_z;
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
    if (m3d_clip_active) {
        if (min_x < m3d_clip_left) min_x = m3d_clip_left;
        if (min_y < m3d_clip_top) min_y = m3d_clip_top;
        if (max_x > m3d_clip_right) max_x = m3d_clip_right;
        if (max_y > m3d_clip_bottom) max_y = m3d_clip_bottom;
    }
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

/* Draw only the cylindrical side wall. Used for tubes with an intentionally
   open end, where a filled cap would make the prop look sealed. */
static void m3d_cylinder_y_open(float cx, float base_y, float cz, float radius,
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
}

/* A horizontal annulus gives the pipe lip a visible top surface without
   filling its hole with the solid cap used by ordinary cylinders. */
static void m3d_ring_horizontal(float cx, float y, float cz,
                                float inner_radius, float outer_radius,
                                uint32_t color, int segments)
{
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 i0 = m3d_v(cx + cosf(a0) * inner_radius, y,
                           cz + sinf(a0) * inner_radius);
        M3DVec3 o0 = m3d_v(cx + cosf(a0) * outer_radius, y,
                           cz + sinf(a0) * outer_radius);
        M3DVec3 o1 = m3d_v(cx + cosf(a1) * outer_radius, y,
                           cz + sinf(a1) * outer_radius);
        M3DVec3 i1 = m3d_v(cx + cosf(a1) * inner_radius, y,
                           cz + sinf(a1) * inner_radius);
        float face_tint = 0.92f + 0.08f * (0.5f + 0.5f * cosf(a0));
        m3d_quad(i0, o0, o1, i1, m3d_tint(color, face_tint));
    }
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
    const float lip_outer = radius + 0.17f;
    const float bore_radius = radius * 0.74f;

    m3d_box(x - 0.56f, 0.0f, z - 0.52f, x + 0.56f, 0.11f, z + 0.52f,
            HEX(0x70452A));

    /* The shaft and widened mouth have open ends; their top is not a filled
       cylinder cap. A separate annulus forms the upper surface of the rim. */
    /* Extra radial segments keep the glossy pipe silhouette smooth at the
       Museum's full-screen scale instead of reading as a low-poly tube. */
    m3d_cylinder_y_open(x, base, z, radius, height, HEX(0x139B3B), 28);
    m3d_cylinder_y_open(x, base + height - 0.26f, z, lip_outer, 0.28f,
                        HEX(0x38CE51), 28);
    m3d_ring_horizontal(x, base + height + 0.02f, z,
                        bore_radius, lip_outer, HEX(0x43D65A), 28);

    /* The bore is a deep cavity with an opaque dark throat. The raised floor
       is still well down inside the shaft, but it blocks the distant railway
       from appearing to run through the pipe when the camera looks into it. */
    m3d_cylinder_y_open(x, base + 0.16f, z, bore_radius, height - 0.30f,
                        HEX(0x0B5729), 28);
    m3d_disk_horizontal(x, base + 1.42f, z, bore_radius * 0.98f,
                        HEX(0x042914), 28);

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

static void m3d_cylinder_x(float x0, float cy, float cz, float radius,
                            float length, uint32_t color, int segments)
{
    float x1 = x0 + length;
    M3DVec3 cap0 = m3d_v(x0, cy, cz);
    M3DVec3 cap1 = m3d_v(x1, cy, cz);
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 p00 = m3d_v(x0, cy + cosf(a0) * radius, cz + sinf(a0) * radius);
        M3DVec3 p01 = m3d_v(x0, cy + cosf(a1) * radius, cz + sinf(a1) * radius);
        M3DVec3 p10 = m3d_v(x1, cy + cosf(a0) * radius, cz + sinf(a0) * radius);
        M3DVec3 p11 = m3d_v(x1, cy + cosf(a1) * radius, cz + sinf(a1) * radius);
        float face_light = 0.88f + 0.12f * (0.5f + 0.5f * cosf(a0));
        m3d_quad(p00, p01, p11, p10, m3d_tint(color, face_light));
        m3d_triangle(cap0, p01, p00, m3d_tint(color, 0.74f));
        m3d_triangle(cap1, p10, p11, m3d_tint(color, 1.05f));
    }
}

static void m3d_train_wheel(float cx, float cy, float cz, float radius,
                            float rotation)
{
    const float half_depth = 0.045f;
    /* Keep each wheel centered on its matching rail. Offsetting the far set
       toward the viewer made the wheel and rail look disconnected in motion. */
    const float z0 = cz - half_depth;
    const float z1 = cz + half_depth;
    const int segments = 20;
    M3DVec3 front_center = m3d_v(cx, cy, z1);

    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i * 6.28318530718f / (float)segments;
        float a1 = (float)(i + 1) * 6.28318530718f / (float)segments;
        M3DVec3 b0 = m3d_v(cx + cosf(a0) * radius, cy + sinf(a0) * radius, z0);
        M3DVec3 b1 = m3d_v(cx + cosf(a1) * radius, cy + sinf(a1) * radius, z0);
        M3DVec3 f0 = m3d_v(cx + cosf(a0) * radius, cy + sinf(a0) * radius, z1);
        M3DVec3 f1 = m3d_v(cx + cosf(a1) * radius, cy + sinf(a1) * radius, z1);
        m3d_quad(b0, b1, f1, f0, m3d_tint(HEX(0x34312C),
                                       0.88f + 0.12f * (0.5f + 0.5f * cosf(a0))));
        m3d_triangle(front_center, f0, f1, HEX(0x51483D));
    }

    /* Four thin spokes rotate with the wheels; the hub sits just proud of the
       wheel face, so the wheels read as solid parts instead of floating balls. */
    for (int i = 0; i < 4; ++i) {
        float a = rotation + (float)i * 1.57079632679f;
        float inner = radius * 0.19f, outer = radius * 0.72f;
        float half_width = radius * 0.075f;
        float ix = cosf(a) * inner, iy = sinf(a) * inner;
        float ox = cosf(a) * outer, oy = sinf(a) * outer;
        float px = -sinf(a) * half_width, py = cosf(a) * half_width;
        float face_z = z1 + 0.004f;
        m3d_quad(m3d_v(cx + ix + px, cy + iy + py, face_z),
                 m3d_v(cx + ox + px, cy + oy + py, face_z),
                 m3d_v(cx + ox - px, cy + oy - py, face_z),
                 m3d_v(cx + ix - px, cy + iy - py, face_z),
                 HEX(0xB5A48A));
    }
    m3d_sphere(cx, cy, z1 + 0.012f, radius * 0.20f,
               HEX(0xD0B98E), 7, 4);
}

static void m3d_train(float elapsed)
{
    /* The train moves along the full rear track. Both ends of the loop keep
       the whole model off-screen, avoiding a visible teleport at the wrap. */
    float travel = -13.0f + fmodf(elapsed * 0.65f, 26.0f);
    const float far_track_z = -5.2f;
    const float wheel_rotation = elapsed * (0.65f / 0.15f);
    m3d_translate_x = travel;
    m3d_translate_z = far_track_z;

    /* Two coupled carriages behind the locomotive. Wheel bottoms overlap the
       rail tops by a fraction of a unit to avoid a sub-pixel floating gap. */
    for (int car = 0; car < 2; ++car) {
        float cx = 0.62f + (float)car * 0.88f;
        uint32_t paint = car == 0 ? HEX(0x2D9D55) : HEX(0x3975C7);
        m3d_box(cx - 0.43f, 0.31f, 3.15f,
                cx + 0.43f, 0.73f, 3.83f, paint);
        m3d_box(cx - 0.47f, 0.67f, 3.12f,
                cx + 0.47f, 0.78f, 3.86f, HEX(0xE4B653));
        m3d_box(cx - 0.34f, 0.40f, 3.835f,
                cx + 0.34f, 0.62f, 3.852f, HEX(0x6C9DB1));
        for (int axle = 0; axle < 2; ++axle) {
            float wx = cx + (axle == 0 ? -0.25f : 0.25f);
            m3d_train_wheel(wx, 0.275f, 3.22f, 0.15f, wheel_rotation);
            m3d_train_wheel(wx, 0.275f, 3.76f, 0.15f, wheel_rotation);
        }
    }

    /* A small visible coupler joins the rear carriage to the locomotive. */
    m3d_box(1.84f, 0.35f, 3.40f, 2.16f, 0.43f, 3.58f, HEX(0x51483D));
    m3d_box(2.12f, 0.29f, 3.15f, 3.40f, 0.40f, 3.83f, HEX(0x8A3927));

    /* Rounded boiler, cab, window, smokestack, cowcatcher, and brass lamp. */
    m3d_cylinder_x(2.08f, 0.53f, 3.49f, 0.28f, 1.12f, HEX(0xC94B31), 24);
    m3d_cylinder_x(3.08f, 0.53f, 3.49f, 0.292f, 0.12f, HEX(0xE7B948), 24);
    m3d_box(3.00f, 0.57f, 3.17f, 3.66f, 1.23f, 3.82f, HEX(0xE7B948));
    m3d_box(3.10f, 0.82f, 3.832f, 3.53f, 1.08f, 3.848f, HEX(0x4D7890));
    m3d_box(3.10f, 0.82f, 3.849f, 3.53f, 0.86f, 3.856f, HEX(0xA5D0D8));
    m3d_cylinder_y(2.43f, 0.70f, 3.49f, 0.12f, 0.28f, HEX(0x8A4924), 12);
    m3d_cylinder_y(2.43f, 0.95f, 3.49f, 0.15f, 0.07f, HEX(0xD8A04A), 12);
    m3d_box(3.43f, 0.23f, 3.28f, 3.78f, 0.36f, 3.70f, HEX(0x6A3C28));
    m3d_sphere(3.73f, 0.51f, 3.49f, 0.095f, HEX(0xFFE5A0), 8, 5);

    /* Leading engine axles sit on both rails as well, not a single row of balls. */
    for (int axle = 0; axle < 3; ++axle) {
        float wx = 2.30f + (float)axle * 0.43f;
        m3d_train_wheel(wx, 0.275f, 3.22f, 0.15f, wheel_rotation);
        m3d_train_wheel(wx, 0.275f, 3.76f, 0.15f, wheel_rotation);
    }

    m3d_translate_x = 0.0f;
    m3d_translate_z = 0.0f;
}

/*
 * Physical hardwood cabinet that sits behind the interactive SDL panels.
 *
 * The UI deliberately remains a screen-space layer so every control keeps
 * its current hit-test and scroll behavior. This model supplies the actual
 * perspective, depth, crown, side cheeks, brass inlay and weighted plinth
 * that the flat panel layer cannot provide on its own.
 */
/*
 * The cabinet shell is modeled in 3D but anchored to the exact logical UI
 * rectangle. Each vertex is inverse-projected through the same camera used by
 * the shelves. This removes the old guessed world-space offset that drifted
 * when the launcher window changed aspect ratio or entered compact layout.
 */
static M3DVec3 m3d_screen_point(float logical_x, float logical_y, float z,
                                const M3DMuseumLayout *layout);
static void m3d_screen_quad(float x0, float y0, float x1, float y1, float z,
                            uint32_t color, const M3DMuseumLayout *layout);
static void m3d_screen_face(float x0, float y0, float z0,
                            float x1, float y1, float z1,
                            float x2, float y2, float z2,
                            float x3, float y3, float z3,
                            uint32_t color, const M3DMuseumLayout *layout);
static void m3d_screen_prism(float x0, float y0, float x1, float y1,
                             float front_z, float back_z, uint32_t color,
                             const M3DMuseumLayout *layout);

static void m3d_cabinet(const M3DMuseumLayout *layout)
{
    if (!layout) return;
    const float x = layout->museum_x, y = layout->museum_y;
    const float left = x - 8.0f, right = x + 816.0f;
    const float top = y - 8.0f, bottom = y + 510.0f;

    /* Dark carcass behind the interactive panels; the side returns project out
       at different depths, so the silhouette is stable under resize. */
    /* Backing board must sit behind the shelf wells in the Z buffer. */
    m3d_screen_quad(left - 10.0f, top - 10.0f, right + 11.0f, bottom + 14.0f,
                    2.35f, HEX(0x281209), layout);
    m3d_screen_face(left, top, 4.82f, right, top, 4.82f,
                    right + 11.0f, top - 10.0f, 3.55f,
                    left - 10.0f, top - 10.0f, 3.55f,
                    HEX(0xB07A45), layout);
    m3d_screen_face(right, top, 4.82f, right, bottom, 4.82f,
                    right + 11.0f, bottom + 13.0f, 3.55f,
                    right + 11.0f, top - 10.0f, 3.55f,
                    HEX(0x3B1D10), layout);
    m3d_screen_face(left, bottom, 4.82f, right, bottom, 4.82f,
                    right + 11.0f, bottom + 13.0f, 3.55f,
                    left - 10.0f, bottom + 13.0f, 3.55f,
                    HEX(0x482516), layout);
    m3d_screen_face(left, top, 4.82f, left, bottom, 4.82f,
                    left - 10.0f, bottom + 13.0f, 3.55f,
                    left - 10.0f, top - 10.0f, 3.55f,
                    HEX(0x704126), layout);

    /* Thick front stiles define the open face of the cabinet. */
    m3d_screen_prism(left, top, left + 7.0f, bottom,
                     5.06f, 3.55f, HEX(0x87532E), layout);
    m3d_screen_prism(right - 7.0f, top, right, bottom,
                     5.06f, 3.55f, HEX(0x4A2616), layout);
    m3d_screen_quad(left + 1.0f, top + 5.0f, left + 2.5f, bottom - 8.0f,
                    5.075f, HEX(0xD3A15F), layout);
    m3d_screen_quad(right - 2.5f, top + 5.0f, right - 1.0f, bottom - 8.0f,
                    5.075f, HEX(0xA8733C), layout);

    /* The top crown is a separate extrusion, rather than a frame drawn over UI. */
    m3d_screen_prism(left, top, right, top + 6.0f,
                     5.04f, 3.86f, HEX(0x925B32), layout);
    m3d_screen_face(left, top, 4.99f, right, top, 4.99f,
                    right + 5.0f, top - 5.5f, 4.62f,
                    left - 5.0f, top - 5.5f, 4.62f,
                    HEX(0xD0A15D), layout);
    m3d_screen_quad(left + 9.0f, top + 1.2f, right - 9.0f, top + 2.2f,
                    5.075f, HEX(0xF3D28A), layout);
    /* Double moulding and hairline brass inlays make the crown read as carved
       hardwood rather than a single extruded slab. */
    m3d_screen_prism(left + 4.0f, top + 5.8f, right - 4.0f, top + 9.0f,
                     4.98f, 4.28f, HEX(0x4A2615), layout);
    m3d_screen_quad(left + 9.0f, top + 6.2f, right - 9.0f, top + 7.0f,
                    5.015f, HEX(0xDDB06A), layout);
    m3d_screen_quad(left + 10.0f, top + 8.0f, right - 10.0f, top + 8.7f,
                    5.015f, HEX(0x281207), layout);

    /* A weighted plinth projects beyond the face at the bottom. */
    m3d_screen_face(left - 2.0f, bottom - 7.0f, 4.97f,
                    right + 2.0f, bottom - 7.0f, 4.97f,
                    right + 5.0f, bottom + 8.0f, 4.48f,
                    left - 5.0f, bottom + 8.0f, 4.48f,
                    HEX(0x412012), layout);
    m3d_screen_prism(left - 2.0f, bottom - 7.0f, right + 2.0f, bottom + 2.0f,
                     5.08f, 4.48f, HEX(0xB47C43), layout);
    m3d_screen_quad(left, bottom + 1.0f, right, bottom + 4.0f,
                    5.105f, HEX(0xE9C57C), layout);
    m3d_screen_quad(left + 75.0f, bottom + 7.0f, left + 130.0f, bottom + 17.0f,
                    4.75f, HEX(0x3F2114), layout);
    m3d_screen_quad(right - 130.0f, bottom + 7.0f, right - 75.0f, bottom + 17.0f,
                    4.75f, HEX(0x3F2114), layout);
    /* Recessed screws and narrow metallic linework anchor the plinth hardware. */
    m3d_screen_quad(left + 88.0f, bottom + 9.0f, left + 92.0f, bottom + 13.0f,
                    4.79f, HEX(0xB98945), layout);
    m3d_screen_quad(right - 92.0f, bottom + 9.0f, right - 88.0f, bottom + 13.0f,
                    4.79f, HEX(0xB98945), layout);
    m3d_screen_quad(left + 12.0f, bottom + 4.7f, right - 12.0f, bottom + 5.5f,
                    5.09f, HEX(0xF2D48B), layout);
}


/* Convert launcher logical coordinates back into this renderer's image plane,
   then invert the perspective camera at a chosen depth. That lets the physical
   cubbies line up exactly underneath SDL's still-interactive text and sprites. */
static M3DVec3 m3d_screen_point(float logical_x, float logical_y, float z,
                                const M3DMuseumLayout *layout)
{
    float sx = (logical_x - layout->view_x + 29.0f) *
               (float)M3D_W / (layout->view_w + 58.0f);
    float sy = (logical_y - layout->view_y + 18.0f) *
               (float)M3D_H / (layout->view_h + 36.0f);
    float ratio = (m3d_center_y - sy) / m3d_focal;
    float distance = m3d_cam_z - z;
    float dy = distance * (ratio * m3d_pitch_cos - m3d_pitch_sin) /
               (m3d_pitch_cos + ratio * m3d_pitch_sin);
    float view_z = distance * m3d_pitch_cos - dy * m3d_pitch_sin;
    float wx = (sx - (float)M3D_W * 0.5f) * view_z / m3d_focal_x;
    return m3d_v(wx, m3d_cam_y + dy, z);
}

static void m3d_set_logical_clip(float x, float y, float width, float height,
                                 const M3DMuseumLayout *layout)
{
    if (!layout || layout->view_w <= 0.0f || layout->view_h <= 0.0f ||
        width <= 0.0f || height <= 0.0f) {
        m3d_clip_active = 0;
        return;
    }
    const float sx_scale = (float)M3D_W / (layout->view_w + 58.0f);
    const float sy_scale = (float)M3D_H / (layout->view_h + 36.0f);
    const float sx0 = (x - layout->view_x + 29.0f) * sx_scale;
    const float sy0 = (y - layout->view_y + 18.0f) * sy_scale;
    const float sx1 = (x + width - layout->view_x + 29.0f) * sx_scale;
    const float sy1 = (y + height - layout->view_y + 18.0f) * sy_scale;

    /* Pixel-center bounds match m3d_triangle_raw's half-pixel sampling. */
    m3d_clip_left = (int)ceilf(sx0 - 0.5f);
    m3d_clip_top = (int)ceilf(sy0 - 0.5f);
    m3d_clip_right = (int)ceilf(sx1 - 0.5f) - 1;
    m3d_clip_bottom = (int)ceilf(sy1 - 0.5f) - 1;
    if (m3d_clip_left < 0) m3d_clip_left = 0;
    if (m3d_clip_top < 0) m3d_clip_top = 0;
    if (m3d_clip_right >= M3D_W) m3d_clip_right = M3D_W - 1;
    if (m3d_clip_bottom >= M3D_H) m3d_clip_bottom = M3D_H - 1;
    m3d_clip_active = m3d_clip_left <= m3d_clip_right &&
                      m3d_clip_top <= m3d_clip_bottom;
}

static void m3d_reset_clip(void)
{
    m3d_clip_active = 0;
}

static void m3d_screen_quad(float x0, float y0, float x1, float y1, float z,
                            uint32_t color, const M3DMuseumLayout *layout)
{
    M3DVec3 a = m3d_screen_point(x0, y0, z, layout);
    M3DVec3 b = m3d_screen_point(x1, y0, z, layout);
    M3DVec3 c = m3d_screen_point(x1, y1, z, layout);
    M3DVec3 d = m3d_screen_point(x0, y1, z, layout);
    m3d_quad(a, b, c, d, color);
}
 
static void m3d_screen_face(float x0, float y0, float z0,
                            float x1, float y1, float z1,
                            float x2, float y2, float z2,
                            float x3, float y3, float z3,
                            uint32_t color, const M3DMuseumLayout *layout)
{
    /* Inverse-project every vertex on the same front plane, then move it in
       world Z. Re-projecting every corner separately would erase the very
       perspective change that is supposed to show the cabinet's depth. */
    M3DVec3 a = m3d_screen_point(x0, y0, z0, layout);
    M3DVec3 b = m3d_screen_point(x1, y1, z0, layout);
    M3DVec3 c = m3d_screen_point(x2, y2, z0, layout);
    M3DVec3 d = m3d_screen_point(x3, y3, z0, layout);
    b.z = z1; c.z = z2; d.z = z3;
    m3d_quad(a, b, c, d, color);
}

static void m3d_screen_prism(float x0, float y0, float x1, float y1,
                             float front_z, float back_z, uint32_t color,
                             const M3DMuseumLayout *layout)
{
    /* A screen-anchored front face is extruded into world space. The rear
       vertices keep their world X/Y, so perspective visibly pulls them
       toward the vanishing point instead of keeping a 2D rectangle outline. */
    M3DVec3 ft_l = m3d_screen_point(x0, y0, front_z, layout);
    M3DVec3 ft_r = m3d_screen_point(x1, y0, front_z, layout);
    M3DVec3 fb_r = m3d_screen_point(x1, y1, front_z, layout);
    M3DVec3 fb_l = m3d_screen_point(x0, y1, front_z, layout);
    M3DVec3 rt_l = m3d_v(ft_l.x, ft_l.y, back_z);
    M3DVec3 rt_r = m3d_v(ft_r.x, ft_r.y, back_z);
    M3DVec3 rb_r = m3d_v(fb_r.x, fb_r.y, back_z);
    M3DVec3 rb_l = m3d_v(fb_l.x, fb_l.y, back_z);

    m3d_quad(rt_l, rt_r, rb_r, rb_l, m3d_tint(color, 0.48f));
    m3d_quad(ft_l, ft_r, rt_r, rt_l, m3d_tint(color, 1.18f));
    m3d_quad(ft_r, fb_r, rb_r, rt_r, m3d_tint(color, 0.70f));
    m3d_quad(fb_l, rb_l, rb_r, fb_r, m3d_tint(color, 0.52f));
    m3d_quad(ft_l, rt_l, rb_l, fb_l, m3d_tint(color, 0.78f));
    m3d_quad(ft_l, ft_r, fb_r, fb_l, color);
}


static void m3d_screen_gold_plate(float x, float y, float width, float height,
                                  float front_z, float body_depth,
                                  const M3DMuseumLayout *layout)
{
    if (!layout || width < 12.0f || height < 7.0f) return;
    const float right = x + width, bottom = y + height;
    const float highlight_z = front_z + 0.055f;

    /* Dark reveal, raised bronze bezel, and inset gold face are three separate
       extrusions. The slim light bands simulate a broad studio reflection. */
    m3d_screen_prism(x - 1.0f, y - 1.0f, right + 1.0f, bottom + 1.0f,
                     front_z - 0.015f, front_z - body_depth,
                     HEX(0x462511), layout);
    m3d_screen_prism(x, y, right, bottom,
                     front_z + 0.015f, front_z - body_depth + 0.04f,
                     HEX(0xA56D27), layout);
    m3d_screen_prism(x + 1.6f, y + 1.3f, right - 1.6f, bottom - 1.3f,
                     front_z + 0.035f, front_z - body_depth + 0.11f,
                     HEX(0xD8B45C), layout);
    m3d_screen_quad(x + 4.0f, y + 1.9f, right - 4.0f, y + 2.7f,
                    highlight_z, HEX(0xFFF1B9), layout);
    m3d_screen_quad(x + 4.0f, bottom - 2.7f, right - 4.0f, bottom - 1.9f,
                    highlight_z, HEX(0x79501D), layout);
    m3d_screen_quad(x + 5.0f, y + 3.3f, right - 5.0f, y + 4.0f,
                    highlight_z, HEX(0xF3D17B), layout);
    m3d_screen_face(x + width * 0.28f, y + 2.5f, highlight_z,
                    x + width * 0.68f, y + 2.5f, highlight_z,
                    x + width * 0.57f, y + height * 0.47f, highlight_z,
                    x + width * 0.22f, y + height * 0.47f, highlight_z,
                    HEX(0xFFF4CF), layout);

    /* Flush square-headed brass fasteners with a tiny reflected edge. */
    for (int side = 0; side < 2; ++side) {
        const float bx = side ? right - 5.0f : x + 3.0f;
        for (int row = 0; row < 2; ++row) {
            const float by = row ? bottom - 5.0f : y + 3.1f;
            m3d_screen_quad(bx, by, bx + 1.9f, by + 1.9f,
                            highlight_z + 0.004f, HEX(0x76501F), layout);
            m3d_screen_quad(bx + 0.35f, by + 0.25f, bx + 1.15f, by + 0.75f,
                            highlight_z + 0.007f, HEX(0xFFF1B2), layout);
        }
    }
}

static void m3d_screen_cubby(float x, float y, float w, float h, int locked,
                             const M3DMuseumLayout *layout)
{
    const float front_z = 5.22f;
    const float rear_z = 3.18f;
    const float opening_left = x + 5.0f, opening_right = x + w - 5.0f;
    const float opening_top = y + 5.0f, opening_bottom = y + h - 25.0f;

    /* These front points line up with the real UI tile. Rear points are NOT
       projected back to screen coordinates: we move the same world vertices
       away from camera so the cavity genuinely shrinks with perspective. */
    M3DVec3 ft_l = m3d_screen_point(opening_left, opening_top, front_z, layout);
    M3DVec3 ft_r = m3d_screen_point(opening_right, opening_top, front_z, layout);
    M3DVec3 fb_r = m3d_screen_point(opening_right, opening_bottom, front_z, layout);
    M3DVec3 fb_l = m3d_screen_point(opening_left, opening_bottom, front_z, layout);
    M3DVec3 rt_l = m3d_v(ft_l.x, ft_l.y, rear_z);
    M3DVec3 rt_r = m3d_v(ft_r.x, ft_r.y, rear_z);
    M3DVec3 rb_r = m3d_v(fb_r.x, fb_r.y, rear_z);
    M3DVec3 rb_l = m3d_v(fb_l.x, fb_l.y, rear_z);

    /* Close the sightline behind the whole opening first. The inset core below
       still shrinks in world space to sell depth, but this rear apron is sized
       on the camera plane so the moving room/train cannot leak around it. */
    m3d_screen_quad(opening_left, opening_top, opening_right, opening_bottom,
                    2.92f, HEX(0x342012), layout);

    /* Dark recessed core and four solid interior walls: the opening has depth. */
    m3d_quad(rt_l, rt_r, rb_r, rb_l, HEX(0x110704));
    m3d_quad(ft_l, ft_r, rt_r, rt_l, HEX(0xB7824C));
    m3d_quad(ft_l, rt_l, rb_l, fb_l, HEX(0x88552F));
    m3d_quad(ft_r, fb_r, rb_r, rt_r, HEX(0x3D1F11));
    m3d_quad(fb_l, rb_l, rb_r, fb_r, HEX(0x684125));

    /* Four independently extruded rails leave an actual hole in the front. */
    m3d_screen_prism(x, y, x + w, y + 5.0f,
                     5.28f, 4.26f, HEX(0xB47D43), layout);
    m3d_screen_prism(x, y + 5.0f, x + 5.0f, y + h - 25.0f,
                     5.28f, 4.26f, HEX(0x8D592F), layout);
    m3d_screen_prism(x + w - 5.0f, y + 5.0f, x + w, y + h - 25.0f,
                     5.28f, 4.26f, HEX(0x452414), layout);
    m3d_screen_prism(x + 5.0f, y + h - 25.0f, x + w - 5.0f, y + h - 21.0f,
                     5.30f, 4.32f, HEX(0x754725), layout);

    /* Bright wood edge on the front is an inlay; the bulk behind it is a solid beam. */
    m3d_screen_quad(x + 2.0f, y + 1.0f, x + w - 2.0f, y + 2.2f,
                    5.31f, HEX(0xE7BC78), layout);
    m3d_screen_quad(x + 2.0f, y + 7.0f, x + 3.0f, y + h - 28.0f,
                    5.31f, HEX(0xD5A164), layout);
    m3d_screen_quad(x + w - 3.0f, y + 7.0f, x + w - 2.0f, y + h - 28.0f,
                    5.31f, HEX(0x2C1509), layout);
    m3d_screen_quad(x + 7.0f, y + h - 27.0f, x + w - 7.0f, y + h - 26.0f,
                    5.31f, HEX(0x3A200F), layout);

    /* Closed compartments use chunky, separately extruded metal shutter slats. */
    if (locked) {
        const float shutter_x = x + 15.0f, shutter_w = w - 30.0f;
        for (int i = 0; i < 5; ++i) {
            float sy = y + 15.0f + i * 6.1f;
            uint32_t face = i == 4 ? HEX(0x454945) : HEX(0x777B75);
            m3d_screen_prism(shutter_x, sy, shutter_x + shutter_w, sy + 5.2f,
                             5.20f, 4.65f, face, layout);
            m3d_screen_quad(shutter_x + 1.2f, sy + 0.3f,
                            shutter_x + shutter_w - 1.2f, sy + 1.0f,
                            5.235f, HEX(0xD9D8CE), layout);
            m3d_screen_quad(shutter_x + 1.2f, sy + 4.1f,
                            shutter_x + shutter_w - 1.2f, sy + 4.8f,
                            5.235f, HEX(0x303530), layout);
            m3d_screen_quad(shutter_x + 3.0f, sy + 1.7f,
                            shutter_x + 4.5f, sy + 3.0f,
                            5.24f, HEX(0xE4C98E), layout);
            m3d_screen_quad(shutter_x + shutter_w - 4.5f, sy + 1.7f,
                            shutter_x + shutter_w - 3.0f, sy + 3.0f,
                            5.24f, HEX(0xE4C98E), layout);
        }
    }

    /* Heavy shelf nose and raised brass plate add a second, readable depth step. */
    m3d_screen_prism(x + 4.0f, y + h - 25.0f, x + w - 4.0f, y + h - 18.0f,
                     5.30f, 4.65f, HEX(0xB67C41), layout);
    m3d_screen_quad(x + 5.0f, y + h - 25.0f, x + w - 5.0f, y + h - 23.8f,
                    5.34f, HEX(0xF0C986), layout);
    const float plaque_w = w * 0.78f;
    const float plaque_x = x + (w - plaque_w) * 0.5f;
    const float plaque_y = y + h - 17.0f;
    m3d_screen_gold_plate(plaque_x - 1.0f, plaque_y, plaque_w + 2.0f, 14.0f,
                          5.30f, 0.38f, layout);
}

static void m3d_museum_grid(const M3DMuseumLayout *layout)
{
    const float x = layout->museum_x, y = layout->museum_y;
    const float body_y = y + 63.0f;
    const float panel_x = x + 458.0f, panel_y = body_y;
    const float panel_w = 350.0f, body_h = 413.0f;
    const float grid_x = x + 467.0f, grid_y = y + 134.0f;
    const float grid_w = 323.0f, grid_h = 330.0f;
    const float tile_w = 103.0f, tile_h = 75.0f, gap_x = 7.0f, row_step = 82.0f;

    /* Deep cabinet bay behind the shelves. The board is farther away than the
       rear walls, not another front-facing gradient placed over the cubbies. */
    m3d_screen_prism(panel_x + 4.0f, panel_y + 9.0f,
                     panel_x + panel_w - 12.0f, panel_y + body_h - 9.0f,
                     3.02f, 2.36f, HEX(0x4D2E1A), layout);
    m3d_screen_prism(panel_x + 7.0f, panel_y + 11.0f,
                     panel_x + panel_w - 15.0f, panel_y + body_h - 12.0f,
                     3.08f, 2.42f, HEX(0x241107), layout);

    /* Side and top/bottom pieces are extruded beams with visible returns. */
    m3d_screen_prism(panel_x, panel_y + 5.0f, panel_x + panel_w - 5.0f, panel_y + 9.0f,
                     5.04f, 3.70f, HEX(0xA16D3E), layout);
    m3d_screen_prism(panel_x, panel_y + 9.0f, panel_x + 4.0f, panel_y + body_h - 5.0f,
                     5.04f, 3.70f, HEX(0x8A572F), layout);
    m3d_screen_prism(panel_x + panel_w - 9.0f, panel_y + 9.0f,
                     panel_x + panel_w - 5.0f, panel_y + body_h - 5.0f,
                     5.04f, 3.70f, HEX(0x3B1E10), layout);
    m3d_screen_prism(panel_x + 4.0f, panel_y + body_h - 9.0f,
                     panel_x + panel_w - 9.0f, panel_y + body_h - 5.0f,
                     5.04f, 3.70f, HEX(0x4A2716), layout);
    m3d_screen_prism(panel_x + 7.0f, panel_y + 6.0f, panel_x + panel_w - 8.0f,
                     panel_y + 7.0f, 5.08f, 4.88f, HEX(0xD8A866), layout);

    /* Physical wood stiles connect the field guide to the open collection bay. */
    m3d_screen_prism(x - 6.0f, body_y + 6.0f, x + 6.0f, body_y + body_h - 6.0f,
                     4.95f, 3.80f, HEX(0x76482A), layout);
    m3d_screen_prism(x + 450.0f, body_y + 6.0f, x + 458.0f, body_y + body_h - 6.0f,
                     5.03f, 3.80f, HEX(0x2A1409), layout);

    if (!layout->draw_cubby_grid) return;

    /* A single physical fascia supports the shelf title and progress counter.
       The inset brass sign and filter keys have their own extrusion, not a
       flat gradient that floats over the cabinet. Text remains in the UI layer. */
    const float fascia_x = panel_x + 4.0f;
    const float fascia_y = panel_y + 8.0f;
    const float fascia_w = panel_w - 8.0f;
    const float fascia_h = 28.0f;
    m3d_screen_prism(fascia_x, fascia_y, fascia_x + fascia_w, fascia_y + fascia_h,
                     5.13f, 4.22f, HEX(0x6B3E21), layout);
    m3d_screen_quad(fascia_x + 4.0f, fascia_y + 1.4f,
                    fascia_x + fascia_w - 4.0f, fascia_y + 2.4f,
                    5.155f, HEX(0xD9A15E), layout);
    m3d_screen_quad(fascia_x + 4.0f, fascia_y + fascia_h - 2.0f,
                    fascia_x + fascia_w - 4.0f, fascia_y + fascia_h - 1.0f,
                    5.155f, HEX(0x3A1E10), layout);

    const float plaque_w = 154.0f;
    const float plaque_x = panel_x + (panel_w - plaque_w) * 0.5f;
    const float plaque_y = panel_y + 10.0f;
    m3d_screen_gold_plate(plaque_x - 3.0f, plaque_y, plaque_w + 6.0f, 24.0f,
                          5.19f, 0.67f, layout);

    /* Three raised wooden filter keys sit exactly behind their clickable labels. */
    const float filter_x = panel_x + 9.0f;
    const float filter_y = panel_y + 40.0f;
    const float filter_w = (panel_w - 35.0f) / 3.0f;
    for (int filter = 0; filter < 3; ++filter) {
        const float key_x = filter_x + (filter_w + 4.0f) * (float)filter;
        m3d_screen_prism(key_x, filter_y, key_x + filter_w, filter_y + 24.0f,
                         5.16f, 4.40f,
                         filter == 0 ? HEX(0x79502E) : HEX(0x57331E), layout);
        m3d_screen_quad(key_x + 3.0f, filter_y + 1.4f,
                        key_x + filter_w - 3.0f, filter_y + 2.3f,
                        5.185f, HEX(0xD1A06A), layout);
        m3d_screen_quad(key_x + 3.0f, filter_y + 21.7f,
                        key_x + filter_w - 3.0f, filter_y + 22.6f,
                        5.185f, HEX(0x31190D), layout);
    }

    /* Outer shelf carcass: actual top/side/bottom returns and a recessed backboard. */
    m3d_screen_prism(grid_x - 4.0f, grid_y - 4.0f, grid_x + grid_w + 4.0f, grid_y,
                     5.04f, 3.76f, HEX(0xA77643), layout);
    m3d_screen_prism(grid_x - 4.0f, grid_y, grid_x, grid_y + grid_h + 4.0f,
                     5.04f, 3.76f, HEX(0x86542F), layout);
    m3d_screen_prism(grid_x + grid_w, grid_y, grid_x + grid_w + 4.0f, grid_y + grid_h + 4.0f,
                     5.04f, 3.76f, HEX(0x3A1D10), layout);
    m3d_screen_prism(grid_x - 4.0f, grid_y + grid_h, grid_x + grid_w + 4.0f, grid_y + grid_h + 4.0f,
                     5.04f, 3.76f, HEX(0x5A351E), layout);
    /* Narrow double highlights define the bevel transitions on the shelf lip. */
    m3d_screen_quad(grid_x - 1.0f, grid_y - 2.5f, grid_x + grid_w + 1.0f, grid_y - 1.5f,
                    5.075f, HEX(0xE5BF7D), layout);
    m3d_screen_quad(grid_x - 2.5f, grid_y + 3.0f, grid_x - 1.5f, grid_y + grid_h - 3.0f,
                    5.075f, HEX(0xD09A58), layout);
    m3d_screen_quad(grid_x + grid_w + 1.5f, grid_y + 3.0f, grid_x + grid_w + 2.5f,
                    grid_y + grid_h - 3.0f, 5.075f, HEX(0x2A1409), layout);
    m3d_screen_prism(grid_x - 1.0f, grid_y, grid_x + grid_w + 1.0f,
                     grid_y + grid_h, 3.08f, 2.54f,
                     HEX(0x1D0D06), layout);

    int total_rows = (layout->visible_entries + 2) / 3;
    for (int row = 0; row < total_rows; ++row) {
        float sy = grid_y + row * row_step + tile_h - layout->scroll;
        if (sy < grid_y - 8.0f || sy > grid_y + grid_h) continue;
        m3d_screen_prism(grid_x - 1.0f, sy, grid_x + grid_w + 1.0f, sy + 7.0f,
                         5.11f, 3.92f, HEX(0x9A6537), layout);
        m3d_screen_prism(grid_x, sy, grid_x + grid_w, sy + 1.0f,
                         5.14f, 4.95f, HEX(0xE2B775), layout);
        m3d_screen_prism(grid_x, sy + 5.5f, grid_x + grid_w, sy + 7.0f,
                         5.13f, 4.95f, HEX(0x2A1309), layout);
    }

    /* Clip the shelf internals to the same viewport as the SDL tile layer.
       This keeps partially scrolled cubbies and rails from spilling across the
       outer bezel, while still drawing the visible portion of each edge tile. */
    m3d_set_logical_clip(grid_x, grid_y, grid_w, grid_h, layout);

    /* Each divider is a narrow wooden beam reaching back into the display. */
    for (int divider = 0; divider < 2; ++divider) {
        float dx = grid_x + (divider + 1) * tile_w + divider * gap_x;
        m3d_screen_prism(dx, grid_y - 1.0f, dx + gap_x, grid_y + grid_h + 1.0f,
                         5.10f, 3.90f, HEX(0x986438), layout);
        m3d_screen_prism(dx, grid_y, dx + 1.4f, grid_y + grid_h,
                         5.13f, 4.94f, HEX(0xD3A067), layout);
        m3d_screen_prism(dx + gap_x - 1.4f, grid_y, dx + gap_x, grid_y + grid_h,
                         5.13f, 4.94f, HEX(0x211006), layout);
    }

    if (layout->visible_entries > 0) {
        int first_row = (int)floorf(layout->scroll / row_step);
        if (first_row < 0) first_row = 0;
        for (int row = first_row; row < first_row + 6 && row < total_rows; ++row) {
            float ty = grid_y + row * row_step - layout->scroll;
            if (ty + tile_h < grid_y || ty > grid_y + grid_h) continue;
            for (int col = 0; col < 3; ++col) {
                int index = row * 3 + col;
                if (index >= layout->visible_entries) break;
                float tx = grid_x + col * (tile_w + gap_x);
                int locked = layout->entry_locked ? layout->entry_locked[index] : 0;
                m3d_screen_cubby(tx, ty, tile_w, tile_h, locked, layout);
            }
        }
    }
    m3d_reset_clip();
}

static void m3d_scene(float elapsed, const M3DMuseumLayout *layout)
{
    m3d_clip_active = 0;
    m3d_translate_x = 0.0f;
    m3d_translate_z = 0.0f;
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

    /* The museum furniture is real perspective geometry; controls are layered later. */
    m3d_cabinet(layout);
    if (layout) m3d_museum_grid(layout);

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

    /* Full-width rear railway. It shares the train's world depth and sits
       behind the display backboard and the green pipes, so the foreground
       geometry naturally occludes it instead of letting it cut through props. */
    m3d_translate_x = 0.0f;
    m3d_translate_z = -5.2f;
    for (int i = 0; i < 42; ++i) {
        float x = -10.4f + (float)i * 0.5f;
        m3d_box(x, -0.015f, 3.12f, x + 0.09f, 0.105f, 3.88f,
                HEX(0xA17143));
    }
    m3d_box(-10.5f, 0.075f, 3.18f, 10.5f, 0.14f, 3.28f,
            HEX(0xBFC0B8));
    m3d_box(-10.5f, 0.075f, 3.72f, 10.5f, 0.14f, 3.82f,
            HEX(0x6E7477));
    m3d_translate_z = 0.0f;

    m3d_train(elapsed);

    /* Small brass exhibit lights hang high on each side of the room. */
    m3d_box(-6.6f, 5.7f, -3.4f, -5.9f, 5.85f, -3.05f, HEX(0xB5803D));
    m3d_sphere(-6.25f, 5.48f, -3.15f, 0.24f, HEX(0xFFD88A), 10, 5);
    m3d_box(5.9f, 5.7f, -3.4f, 6.6f, 5.85f, -3.05f, HEX(0xB5803D));
    m3d_sphere(6.25f, 5.48f, -3.15f, 0.24f, HEX(0xFFD88A), 10, 5);
}

void museum3d_draw_background(SDL_Renderer *renderer,
                              float x, float y, float width, float height,
                              float elapsed_seconds,
                              float museum_x, float museum_y, float museum_scroll,
                              const int *entry_locked,
                              int visible_entries, int draw_cubby_grid)
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

    M3DMuseumLayout layout;
    layout.view_x = x; layout.view_y = y;
    layout.view_w = width; layout.view_h = height;
    layout.museum_x = museum_x; layout.museum_y = museum_y;
    layout.scroll = museum_scroll;
    layout.entry_locked = entry_locked;
    layout.visible_entries = visible_entries < 0 ? 0 : visible_entries;
    layout.draw_cubby_grid = draw_cubby_grid;
    /* The render target is stretched to the visible window viewport. Adjust horizontal
       camera FOV to cancel that stretch, so the 3D furniture keeps its proportions
       on ultrawide, narrow, maximized, and compact windows. */
    {
        float output_aspect = (width + 58.0f) / (height + 36.0f);
        float target_aspect = (float)M3D_W / (float)M3D_H;
        if (output_aspect < 0.2f) output_aspect = target_aspect;
        m3d_focal_x = m3d_focal * target_aspect / output_aspect;
    }
    m3d_scene(elapsed_seconds, &layout);
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
