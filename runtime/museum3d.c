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
    float cabinet_x_offset;
    float museum_x, museum_y, scroll;
    int visible_entries;
    int draw_cubby_grid;
} M3DMuseumLayout;

static uint32_t m3d_pixels[M3D_PIXELS];
static float m3d_depth[M3D_PIXELS];
static SDL_Texture *m3d_texture;
static SDL_Renderer *m3d_renderer;
static float m3d_projection_x_offset;

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
    out->x = (float)M3D_W * 0.5f + (p.x + m3d_projection_x_offset) * m3d_focal / view_z;
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


/*
 * Physical hardwood cabinet that sits behind the interactive SDL panels.
 *
 * The UI deliberately remains a screen-space layer so every control keeps
 * its current hit-test and scroll behavior. This model supplies the actual
 * perspective, depth, crown, side cheeks, brass inlay and weighted plinth
 * that the flat panel layer cannot provide on its own.
 */
static void m3d_cabinet(float horizontal_offset)
{
    m3d_projection_x_offset = horizontal_offset;
    const float front_z = 4.72f;
    const float rear_z = 2.30f;

    /* Trapezoidal front corners compensate for the perspective camera so the
       shell frames the current logical cabinet without looking like a flat box. */
    M3DVec3 front_bl = m3d_v(-2.44f, -0.10f, front_z);
    M3DVec3 front_br = m3d_v( 4.40f, -0.10f, front_z);
    M3DVec3 front_tl = m3d_v(-2.05f,  3.30f, front_z);
    M3DVec3 front_tr = m3d_v( 3.72f,  3.30f, front_z);

    M3DVec3 rear_bl = m3d_v(-2.62f, -0.20f, rear_z);
    M3DVec3 rear_br = m3d_v( 4.15f, -0.20f, rear_z);
    M3DVec3 rear_tl = m3d_v(-2.36f,  3.53f, rear_z);
    M3DVec3 rear_tr = m3d_v( 3.82f,  3.53f, rear_z);

    /* Five solid faces form the carcass: a thick top, bottom, and both sides. */
    m3d_quad(rear_bl, rear_br, front_br, front_bl, HEX(0x3C1D10));
    m3d_quad(rear_tl, rear_tr, front_tr, front_tl, HEX(0xA87745));
    m3d_quad(rear_bl, front_bl, front_tl, rear_tl, HEX(0x704126));
    m3d_quad(rear_br, front_br, front_tr, rear_tr, HEX(0x3B2014));
    m3d_quad(front_bl, front_br, front_tr, front_tl, HEX(0x63351D));

    /* Crown fascia sits proud of the front face, above the UI's top edge. */
    M3DVec3 crown_bl = m3d_v(-2.22f, 3.30f, 4.77f);
    M3DVec3 crown_br = m3d_v( 3.89f, 3.30f, 4.77f);
    M3DVec3 crown_tl = m3d_v(-2.28f, 3.53f, 4.77f);
    M3DVec3 crown_tr = m3d_v( 3.93f, 3.53f, 4.77f);
    m3d_quad(front_tl, front_tr, crown_br, crown_bl, HEX(0x89532D));
    m3d_quad(crown_bl, crown_br, crown_tr, crown_tl, HEX(0xA87540));
    m3d_quad(rear_tl, rear_tr, crown_tr, crown_tl, HEX(0x98663A));

    /* A raised brass line in the crown catches the room's warm key light. */
    m3d_box(-2.24f, 3.455f, 4.80f, 3.90f, 3.485f, 4.91f, HEX(0xD8AF68));
    m3d_box(-2.18f, 3.460f, 4.91f, 3.84f, 3.480f, 4.94f, HEX(0xF1D18B));

    /* Long, tapered front stiles give the cabinet real wood thickness. */
    m3d_quad(m3d_v(-2.58f, -0.12f, 4.80f),
             m3d_v(-2.43f, -0.12f, 4.80f),
             m3d_v(-2.03f,  3.29f, 4.80f),
             m3d_v(-2.19f,  3.29f, 4.80f), HEX(0x8C5A31));
    m3d_quad(m3d_v( 4.39f, -0.12f, 4.80f),
             m3d_v( 4.55f, -0.12f, 4.80f),
             m3d_v( 3.87f,  3.29f, 4.80f),
             m3d_v( 3.71f,  3.29f, 4.80f), HEX(0x4B2817));
    m3d_quad(m3d_v(-2.55f, -0.12f, 4.83f),
             m3d_v(-2.51f, -0.12f, 4.83f),
             m3d_v(-2.13f,  3.29f, 4.83f),
             m3d_v(-2.17f,  3.29f, 4.83f), HEX(0xD2A15B));
    m3d_quad(m3d_v( 4.48f, -0.12f, 4.83f),
             m3d_v( 4.52f, -0.12f, 4.83f),
             m3d_v( 3.81f,  3.29f, 4.83f),
             m3d_v( 3.77f,  3.29f, 4.83f), HEX(0xB47A3D));

    /* A framed maker's badge and two raised brass fasteners on the crown. */
    m3d_box(0.13f, 3.365f, 4.82f, 1.56f, 3.485f, 4.94f, HEX(0xD0A155));
    m3d_box(0.20f, 3.383f, 4.94f, 1.49f, 3.468f, 4.985f, HEX(0x5A321B));
    m3d_box(0.31f, 3.405f, 4.986f, 0.43f, 3.446f, 5.005f, HEX(0xE9C77C));
    m3d_box(0.48f, 3.405f, 4.986f, 0.60f, 3.446f, 5.005f, HEX(0xE9C77C));
    m3d_box(0.65f, 3.405f, 4.986f, 0.77f, 3.446f, 5.005f, HEX(0xE9C77C));
    m3d_box(0.82f, 3.405f, 4.986f, 0.94f, 3.446f, 5.005f, HEX(0xE9C77C));
    m3d_sphere(-2.16f, 3.40f, 4.91f, 0.055f, HEX(0xF0D18B), 8, 4);
    m3d_sphere( 3.82f, 3.40f, 4.91f, 0.055f, HEX(0xF0D18B), 8, 4);

    /* The broad bottom plinth projects below the catalog, with a bright lip. */
    m3d_box(-2.60f, -0.35f, 2.65f, 4.50f, -0.11f, 4.97f, HEX(0x57301A));
    m3d_box(-2.65f, -0.25f, 4.96f, 4.55f, -0.205f, 5.015f, HEX(0xD0A15B));
    m3d_box(-2.59f, -0.31f, 5.015f, 4.49f, -0.275f, 5.045f, HEX(0xF0CF83));
    m3d_box(-2.54f, -0.35f, 4.97f, 4.44f, -0.325f, 5.02f, HEX(0x32190D));

    /* Two sturdy feet lift the plinth away from the floor boards. */
    m3d_box(-2.03f, -0.52f, 3.45f, -1.45f, -0.32f, 4.69f, HEX(0x442516));
    m3d_box( 3.38f, -0.52f, 3.45f,  3.96f, -0.32f, 4.69f, HEX(0x442516));
    m3d_box(-1.98f, -0.49f, 4.69f, -1.50f, -0.445f, 4.75f, HEX(0x94633A));
    m3d_box( 3.43f, -0.49f, 4.69f,  3.91f, -0.445f, 4.75f, HEX(0x94633A));
    m3d_projection_x_offset = 0.0f;
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
    float wx = (sx - (float)M3D_W * 0.5f) * view_z / m3d_focal;
    return m3d_v(wx, m3d_cam_y + dy, z);
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

static void m3d_screen_cubby(float x, float y, float w, float h,
                             const M3DMuseumLayout *layout)
{
    const float front_z = 4.96f, back_z = 4.48f;
    const float opening_left = x + 4.0f, opening_right = x + w - 4.0f;
    const float opening_top = y + 4.0f, opening_bottom = y + h - 25.0f;
    const float rear_left = x + 13.0f, rear_right = x + w - 13.0f;
    const float rear_top = y + 12.0f, rear_bottom = y + h - 30.0f;

    M3DVec3 ft_l = m3d_screen_point(opening_left, opening_top, front_z, layout);
    M3DVec3 ft_r = m3d_screen_point(opening_right, opening_top, front_z, layout);
    M3DVec3 fb_r = m3d_screen_point(opening_right, opening_bottom, front_z, layout);
    M3DVec3 fb_l = m3d_screen_point(opening_left, opening_bottom, front_z, layout);
    M3DVec3 rt_l = m3d_screen_point(rear_left, rear_top, back_z, layout);
    M3DVec3 rt_r = m3d_screen_point(rear_right, rear_top, back_z, layout);
    M3DVec3 rb_r = m3d_screen_point(rear_right, rear_bottom, back_z, layout);
    M3DVec3 rb_l = m3d_screen_point(rear_left, rear_bottom, back_z, layout);

    /* Recessed rear wall first, then the four inner walls of the hollow cubby. */
    m3d_quad(rt_l, rt_r, rb_r, rb_l, HEX(0x170B06));
    m3d_quad(ft_l, ft_r, rt_r, rt_l, HEX(0xA47647));
    m3d_quad(ft_l, rt_l, rb_l, fb_l, HEX(0x86542F));
    m3d_quad(ft_r, fb_r, rb_r, rt_r, HEX(0x3C1E10));
    m3d_quad(fb_l, rb_l, rb_r, fb_r, HEX(0x684124));

    /* The front surround is a raised wooden frame, not a flat card background. */
    m3d_screen_quad(x, y, x + w, y + 4.0f, 4.985f, HEX(0xA87845), layout);
    m3d_screen_quad(x, y + 4.0f, x + 4.0f, y + h - 25.0f,
                    4.985f, HEX(0x8B592F), layout);
    m3d_screen_quad(x + w - 4.0f, y + 4.0f, x + w, y + h - 25.0f,
                    4.985f, HEX(0x452414), layout);
    m3d_screen_quad(x + 4.0f, y + h - 25.0f, x + w - 4.0f, y + h - 22.0f,
                    4.985f, HEX(0x583018), layout);
    m3d_screen_quad(x + 3.0f, y + 1.0f, x + w - 3.0f, y + 2.2f,
                    5.005f, HEX(0xE1B978), layout);

    /* Shelf nose and the accession plaque are raised pieces attached to the frame. */
    m3d_screen_quad(x + 4.0f, y + h - 25.0f, x + w - 4.0f, y + h - 18.0f,
                    5.00f, HEX(0xB47B42), layout);
    m3d_screen_quad(x + 5.0f, y + h - 25.0f, x + w - 5.0f, y + h - 24.0f,
                    5.015f, HEX(0xE6BC78), layout);
    float plaque_w = w * 0.78f;
    float plaque_x = x + (w - plaque_w) * 0.5f;
    float plaque_y = y + h - 17.0f;
    m3d_screen_quad(plaque_x, plaque_y, plaque_x + plaque_w, plaque_y + 14.0f,
                    5.005f, HEX(0xA66B32), layout);
    m3d_screen_quad(plaque_x + 2.0f, plaque_y + 2.0f,
                    plaque_x + plaque_w - 2.0f, plaque_y + 12.0f,
                    5.025f, HEX(0xE5C982), layout);
    m3d_screen_quad(plaque_x + 3.0f, plaque_y + 4.0f,
                    plaque_x + 5.0f, plaque_y + 6.0f,
                    5.035f, HEX(0x68401D), layout);
    m3d_screen_quad(plaque_x + plaque_w - 5.0f, plaque_y + 4.0f,
                    plaque_x + plaque_w - 3.0f, plaque_y + 6.0f,
                    5.035f, HEX(0x68401D), layout);
}

static void m3d_museum_grid(const M3DMuseumLayout *layout)
{
    const float x = layout->museum_x, y = layout->museum_y;
    const float panel_x = x + 458.0f, panel_y = y + 63.0f;
    const float panel_w = 350.0f, body_h = 413.0f;
    const float grid_x = x + 467.0f, grid_y = y + 134.0f;
    const float grid_w = 323.0f, grid_h = 330.0f;
    const float tile_w = 103.0f, tile_h = 75.0f, gap_x = 7.0f, row_step = 82.0f;

    /* Outer right-hand display board is a fitted wood panel on the cabinet face. */
    m3d_screen_quad(panel_x, panel_y + 5.0f, panel_x + panel_w - 5.0f,
                    panel_y + body_h - 5.0f, 4.75f, HEX(0x80512F), layout);
    m3d_screen_quad(panel_x + 4.0f, panel_y + 9.0f, panel_x + panel_w - 12.0f,
                    panel_y + body_h - 9.0f, 4.78f, HEX(0x4B2A18), layout);
    m3d_screen_quad(panel_x + 7.0f, panel_y + 11.0f, panel_x + panel_w - 15.0f,
                    panel_y + body_h - 12.0f, 4.79f, HEX(0x382012), layout);
    m3d_screen_quad(panel_x + 9.0f, panel_y + 6.0f, panel_x + panel_w - 8.0f,
                    panel_y + 7.0f, 4.81f, HEX(0xD8A866), layout);
    m3d_screen_quad(panel_x + 7.0f, panel_y + body_h - 7.0f,
                    panel_x + panel_w - 9.0f, panel_y + body_h - 6.0f,
                    4.81f, HEX(0x2A1309), layout);

    /* Recessed dark backing creates the shadowed interior behind the exhibits. */
    m3d_screen_quad(grid_x - 3.0f, grid_y - 3.0f,
                    grid_x + grid_w + 3.0f, grid_y + grid_h + 3.0f,
                    4.81f, HEX(0x1D0D06), layout);
    m3d_screen_quad(grid_x - 1.0f, grid_y - 1.0f,
                    grid_x + grid_w + 1.0f, grid_y + grid_h + 1.0f,
                    4.82f, HEX(0x302015), layout);

    /* Scrollable shelf rails are physical wood bars; their positions follow the list. */
    int total_rows = (layout->visible_entries + 2) / 3;
    for (int row = 0; row < total_rows; ++row) {
        float sy = grid_y + row * row_step + tile_h - layout->scroll;
        if (sy < grid_y - 7.0f || sy > grid_y + grid_h) continue;
        m3d_screen_quad(grid_x - 1.0f, sy, grid_x + grid_w + 1.0f, sy + 7.0f,
                        4.91f, HEX(0x9A6537), layout);
        m3d_screen_quad(grid_x, sy, grid_x + grid_w, sy + 1.0f,
                        4.94f, HEX(0xE2B775), layout);
        m3d_screen_quad(grid_x, sy + 5.5f, grid_x + grid_w, sy + 7.0f,
                        4.95f, HEX(0x2A1309), layout);
    }

    /* Two upright rails physically separate the three columns. */
    for (int divider = 0; divider < 2; ++divider) {
        float dx = grid_x + (divider + 1) * tile_w + divider * gap_x;
        m3d_screen_quad(dx, grid_y - 1.0f, dx + gap_x, grid_y + grid_h + 1.0f,
                        4.90f, HEX(0x9A663A), layout);
        m3d_screen_quad(dx, grid_y, dx + 1.0f, grid_y + grid_h,
                        4.94f, HEX(0xD3A067), layout);
        m3d_screen_quad(dx + gap_x - 1.0f, grid_y, dx + gap_x, grid_y + grid_h,
                        4.95f, HEX(0x211006), layout);
    }

    if (!layout->draw_cubby_grid || layout->visible_entries <= 0) return;

    int first_row = (int)floorf(layout->scroll / row_step);
    if (first_row < 0) first_row = 0;
    for (int row = first_row; row < first_row + 6 && row < total_rows; ++row) {
        float ty = grid_y + row * row_step - layout->scroll;
        if (ty < grid_y || ty + tile_h > grid_y + grid_h) continue;
        for (int col = 0; col < 3; ++col) {
            int index = row * 3 + col;
            if (index >= layout->visible_entries) break;
            float tx = grid_x + col * (tile_w + gap_x);
            m3d_screen_cubby(tx, ty, tile_w, tile_h, layout);
        }
    }
}

static void m3d_scene(float elapsed, const M3DMuseumLayout *layout)
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

    /* The museum furniture is real perspective geometry; controls are layered later. */
    m3d_cabinet(layout ? layout->cabinet_x_offset : 0.0f);
    m3d_projection_x_offset = 0.0f;
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
                              float elapsed_seconds, float cabinet_x_offset,
                              float museum_x, float museum_y, float museum_scroll,
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
    layout.cabinet_x_offset = cabinet_x_offset;
    layout.museum_x = museum_x; layout.museum_y = museum_y;
    layout.scroll = museum_scroll;
    layout.visible_entries = visible_entries < 0 ? 0 : visible_entries;
    layout.draw_cubby_grid = draw_cubby_grid;
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
