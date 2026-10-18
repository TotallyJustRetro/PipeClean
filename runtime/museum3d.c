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

#if defined(__ANDROID__)
#define M3D_W 960
#define M3D_H 628
#define M3D_RENDER_SCALE 1.0f
#else
#define M3D_W 1080
#define M3D_H 707
#define M3D_RENDER_SCALE 1.125f
#endif
#define M3D_PIXELS (M3D_W * M3D_H)
#define M3D_NEAR 0.35f
#define HEX(rgb) (0xFF000000u | (uint32_t)(rgb))

typedef struct { float x, y, z; } M3DVec3;
typedef struct { float x, y, invz, z; } M3DProjected;

enum { M3D_TRACK_CONTROL_COUNT = 20, M3D_TRACK_PATH_STEPS = 5,
       M3D_TRACK_PATH_COUNT = M3D_TRACK_CONTROL_COUNT * M3D_TRACK_PATH_STEPS };
static const M3DVec3 m3d_track_control[M3D_TRACK_CONTROL_COUNT] = {
    /* The rear straight reaches the room margins, where it can visibly join
       the descents rather than ending behind the cabinet's opaque panels. */
    {-7.45f, 4.225f, -1.71f},
    { 7.45f, 4.225f, -1.71f},

    /* Right ramp is deliberately outside the cabinet's projected edge for
       its full drop. As it descends, x comes inward gradually; the last floor
       turn stays outside the exhibit plinth and joins the foreground return. */
    { 7.55f, 3.65f, -1.45f},
    { 7.35f, 2.85f, -0.70f},
    { 6.95f, 1.90f,  0.15f},
    { 6.60f, 0.95f,  1.00f},
    { 6.45f, 0.090f, 1.70f},
    { 6.30f, 0.090f, 2.25f},
    { 6.30f, 0.090f, 3.10f},
    { 5.05f, 0.090f, 4.45f},

    /* Long foreground return, then the left side rises back to the rear rail.
       It routes outside the Toad plinth and just left of the navigation panel
       so both sides read as the same continuous railway in the final image. */
    {-5.05f, 0.090f, 4.45f},
    {-6.30f, 0.090f, 3.10f},
    {-6.80f, 0.090f, 2.25f},
    {-6.80f, 0.090f, 1.70f},
    {-6.60f, 0.95f,  1.00f},
    {-6.95f, 1.90f,  0.15f},
    {-7.35f, 2.85f, -0.70f},
    {-7.70f, 3.65f, -1.45f},
    {-7.55f, 3.95f, -1.62f},
    {-7.45f, 4.15f, -1.68f}
};
static M3DVec3 m3d_track_path[M3D_TRACK_PATH_COUNT];

typedef struct {
    float view_x, view_y, view_w, view_h;
    float museum_x, museum_y, scroll;
    float ui_origin_x, ui_origin_y, ui_scale_x, ui_scale_y;
    float elapsed_seconds;
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
static float m3d_translate_y;
static float m3d_translate_z;
/* Scales only the moving railway vehicle about the track's centerline. */
static float m3d_train_scale = 1.0f;
static float m3d_train_yaw;
static float m3d_train_pitch;
static int m3d_train_transform_active;
static int m3d_clip_active;
static int m3d_clip_left, m3d_clip_top, m3d_clip_right, m3d_clip_bottom;

static const float m3d_cam_y = 4.6f;
static const float m3d_cam_z = 10.5f;
static const float m3d_pitch_sin = 0.31f;
static const float m3d_pitch_cos = 0.9502f;
static const float m3d_focal = 754.0f * M3D_RENDER_SCALE;
static float m3d_focal_x = 754.0f * M3D_RENDER_SCALE;
static const float m3d_center_y = 270.0f * M3D_RENDER_SCALE;

static M3DVec3 m3d_v(float x, float y, float z)
{
    M3DVec3 v = {x, y, z};
    return v;
}

static float m3d_catmull(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
        (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2 +
        (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3);
}

static void m3d_build_track_path(void)
{
    for (int i = 0; i < M3D_TRACK_CONTROL_COUNT; ++i) {
        const M3DVec3 p0 = m3d_track_control[(i + M3D_TRACK_CONTROL_COUNT - 1) %
                                              M3D_TRACK_CONTROL_COUNT];
        const M3DVec3 p1 = m3d_track_control[i];
        const M3DVec3 p2 = m3d_track_control[(i + 1) % M3D_TRACK_CONTROL_COUNT];
        const M3DVec3 p3 = m3d_track_control[(i + 2) % M3D_TRACK_CONTROL_COUNT];
        for (int step = 0; step < M3D_TRACK_PATH_STEPS; ++step) {
            const float t = (float)step / (float)M3D_TRACK_PATH_STEPS;
            const int idx = i * M3D_TRACK_PATH_STEPS + step;
            m3d_track_path[idx].x = m3d_catmull(p0.x, p1.x, p2.x, p3.x, t);
            m3d_track_path[idx].y = m3d_catmull(p0.y, p1.y, p2.y, p3.y, t);
            m3d_track_path[idx].z = m3d_catmull(p0.z, p1.z, p2.z, p3.z, t);
        }
    }
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
    if (m3d_train_transform_active) {
        /* Pivot at the carriage centerline and wheel axle. This lets the
           complete toy turn and climb/descend the sloped rail as one model. */
        const float lx = (p.x - 2.0f) * m3d_train_scale;
        const float ly = (p.y - 0.275f) * m3d_train_scale;
        const float lz = (p.z - 3.49f) * m3d_train_scale;
        const float cp = cosf(m3d_train_pitch), sp = sinf(m3d_train_pitch);
        const float cy = cosf(m3d_train_yaw), sy = sinf(m3d_train_yaw);
        const float pitched_x = lx * cp - ly * sp;
        const float pitched_y = lx * sp + ly * cp;
        p.x = pitched_x * cy - lz * sy + m3d_translate_x;
        p.y = pitched_y + m3d_translate_y;
        p.z = pitched_x * sy + lz * cy + m3d_translate_z;
    } else {
        p.x += m3d_translate_x;
        p.y += m3d_translate_y;
        p.z += m3d_translate_z;
    }
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

static void m3d_toad_figure(float x, float z, float bob)
{
    const float b = bob;
    /* Brown shoes and a blue vest sit on the actual wooden display plinth. */
    m3d_sphere(x - 0.15f, 0.535f + b, z + 0.035f, 0.135f,
               HEX(0x71452A), 10, 6);
    m3d_sphere(x + 0.15f, 0.535f + b, z + 0.035f, 0.135f,
               HEX(0x71452A), 10, 6);
    m3d_cylinder_y(x, 0.56f + b, z, 0.245f, 0.43f,
                   HEX(0x247EC5), 14);
    m3d_sphere(x, 0.80f + b, z, 0.265f, HEX(0x287FC7), 14, 8);
    m3d_sphere(x - 0.285f, 0.82f + b, z + 0.01f, 0.12f,
               HEX(0xE9C9A5), 9, 6);
    m3d_sphere(x + 0.285f, 0.82f + b, z + 0.01f, 0.12f,
               HEX(0xE9C9A5), 9, 6);
    m3d_box(x - 0.11f, 0.73f + b, z + 0.225f,
            x + 0.11f, 0.765f + b, z + 0.248f, HEX(0xE9C9A5));

    /* Round peach face, oversized white mushroom cap, and signature spots. */
    m3d_sphere(x, 1.205f + b, z + 0.135f, 0.285f,
               HEX(0xF0D2AD), 14, 9);
    m3d_sphere(x, 1.535f + b, z, 0.425f,
               HEX(0xF7F4E8), 18, 10);
    m3d_sphere(x, 1.835f + b, z + 0.285f, 0.105f,
               HEX(0xD94343), 10, 6);
    m3d_sphere(x - 0.275f, 1.625f + b, z + 0.285f, 0.093f,
               HEX(0xD94343), 10, 6);
    m3d_sphere(x + 0.275f, 1.625f + b, z + 0.285f, 0.093f,
               HEX(0xD94343), 10, 6);
    m3d_sphere(x - 0.115f, 1.215f + b, z + 0.392f, 0.044f,
               HEX(0x29211B), 8, 5);
    m3d_sphere(x + 0.115f, 1.215f + b, z + 0.392f, 0.044f,
               HEX(0x29211B), 8, 5);
    m3d_sphere(x, 1.075f + b, z + 0.402f, 0.045f,
               HEX(0xC4826A), 8, 5);
}

static void m3d_goomba_figure(float x, float z, float bob)
{
    const float b = bob;
    /* Stubby mushroom body, tan muzzle, and two dark feet. */
    m3d_sphere(x - 0.205f, 0.535f + b, z + 0.015f, 0.145f,
               HEX(0x593225), 10, 6);
    m3d_sphere(x + 0.205f, 0.535f + b, z + 0.015f, 0.145f,
               HEX(0x593225), 10, 6);
    m3d_sphere(x, 0.805f + b, z, 0.355f,
               HEX(0x9C5835), 14, 8);
    m3d_sphere(x, 1.155f + b, z - 0.005f, 0.405f,
               HEX(0x8D4A2F), 16, 9);
    m3d_sphere(x, 0.985f + b, z + 0.205f, 0.265f,
               HEX(0xE8C99D), 14, 8);

    /* Slanted brows and tiny pupils give the face the classic grumpy look. */
    m3d_quad(m3d_v(x - 0.235f, 1.205f + b, z + 0.385f),
             m3d_v(x - 0.045f, 1.155f + b, z + 0.435f),
             m3d_v(x - 0.045f, 1.205f + b, z + 0.435f),
             m3d_v(x - 0.235f, 1.255f + b, z + 0.385f),
             HEX(0x3D241C));
    m3d_quad(m3d_v(x + 0.045f, 1.155f + b, z + 0.435f),
             m3d_v(x + 0.235f, 1.205f + b, z + 0.385f),
             m3d_v(x + 0.235f, 1.255f + b, z + 0.385f),
             m3d_v(x + 0.045f, 1.205f + b, z + 0.435f),
             HEX(0x3D241C));
    m3d_sphere(x - 0.115f, 1.105f + b, z + 0.425f, 0.063f,
               HEX(0xFFF9E8), 9, 6);
    m3d_sphere(x + 0.115f, 1.105f + b, z + 0.425f, 0.063f,
               HEX(0xFFF9E8), 9, 6);
    m3d_sphere(x - 0.102f, 1.095f + b, z + 0.478f, 0.030f,
               HEX(0x211713), 8, 5);
    m3d_sphere(x + 0.102f, 1.095f + b, z + 0.478f, 0.030f,
               HEX(0x211713), 8, 5);
    m3d_box(x - 0.105f, 0.865f + b, z + 0.423f,
            x + 0.105f, 0.905f + b, z + 0.452f, HEX(0x42231B));
    m3d_triangle(m3d_v(x - 0.065f, 0.905f + b, z + 0.454f),
                 m3d_v(x - 0.015f, 0.905f + b, z + 0.454f),
                 m3d_v(x - 0.040f, 0.845f + b, z + 0.454f), HEX(0xFFF3D7));
    m3d_triangle(m3d_v(x + 0.015f, 0.905f + b, z + 0.454f),
                 m3d_v(x + 0.065f, 0.905f + b, z + 0.454f),
                 m3d_v(x + 0.040f, 0.845f + b, z + 0.454f), HEX(0xFFF3D7));
}

static void m3d_pipe(float x, float z)
{
    const float base = 0.02f, height = 4.6f, radius = 0.34f;
    const float lip_outer = radius + 0.13f;
    const float bore_radius = radius * 0.70f;

    m3d_box(x - 0.42f, 0.0f, z - 0.43f, x + 0.42f, 0.11f, z + 0.43f,
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

    /* Opaque rear throat: the interior is deep and shaded, but not a
       see-through tunnel. It sits behind the inner green wall and in front of
       the rear railway, so the train cannot appear to pass through the pipe. */
    m3d_box(x - bore_radius * 0.94f, base + 0.18f, z - 0.58f,
            x + bore_radius * 0.94f, base + height - 0.34f, z - 0.52f,
            HEX(0x042914));
    m3d_box(x - bore_radius * 0.78f, base + 0.26f, z - 0.515f,
            x - bore_radius * 0.72f, base + height - 0.58f, z - 0.51f,
            HEX(0x0C3D20));

    /* This low, horizontal shadow floor still tells the eye the shaft is deep. */
    m3d_disk_horizontal(x, base + 1.42f, z, bore_radius * 0.98f,
                        HEX(0x042914), 28);

    m3d_cylinder_y(x - radius * 0.25f, 0.11f, z + radius * 0.92f, 0.020f, 3.95f,
                   HEX(0xA4F47E), 6);
}

/* A reusable section of hollow green pipe. The same radial geometry is
   used for the short wall outlet and foreground mouths so the props share
   a consistent lip, bore, shading, and glossy highlight with the room pipes. */
static void m3d_pipe_tube(float x, float base_y, float z,
                          float height, float radius)
{
    if (height < 0.25f || radius < 0.08f) return;
    const float lip_h = fminf(0.22f, height * 0.32f);
    const float lip_outer = radius + 0.11f;
    const float bore_radius = radius * 0.69f;

    m3d_cylinder_y_open(x, base_y, z, radius, height,
                        HEX(0x139B3B), 28);
    m3d_cylinder_y_open(x, base_y + height - lip_h, z, lip_outer, lip_h,
                        HEX(0x38CE51), 28);
    m3d_ring_horizontal(x, base_y + height + 0.02f, z,
                        bore_radius, lip_outer, HEX(0x43D65A), 28);
    m3d_cylinder_y_open(x, base_y + 0.055f, z, bore_radius,
                        fmaxf(0.12f, height - 0.075f),
                        HEX(0x0B5729), 28);
    /* The shadow floor is part-way down the bore, not on the lip surface. */
    m3d_disk_horizontal(x, base_y + fmaxf(0.10f, height * 0.30f), z,
                        bore_radius * 0.96f, HEX(0x042914), 28);
    m3d_cylinder_y(x - radius * 0.25f, base_y + 0.04f,
                   z + radius * 0.92f, 0.014f,
                   fmaxf(0.12f, height - lip_h),
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

static void m3d_train(float elapsed, const M3DVec3 *path, int path_count)
{
    if (!path || path_count < 2) return;
    float total_length = 0.0f;
    for (int i = 0; i < path_count; ++i) {
        const M3DVec3 a = path[i], b = path[(i + 1) % path_count];
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        total_length += sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (total_length < 0.01f) return;

    /* Distance-based travel avoids speed changes at the smoothed corners. */
    float distance = fmodf(elapsed * 2.50f + 12.50f, total_length);
    if (distance < 0.0f) distance += total_length;
    M3DVec3 position = path[0], tangent = m3d_v(1.0f, 0.0f, 0.0f);
    for (int i = 0; i < path_count; ++i) {
        const M3DVec3 a = path[i], b = path[(i + 1) % path_count];
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float length = sqrtf(dx * dx + dy * dy + dz * dz);
        if (length < 0.0001f) continue;
        if (distance <= length || i == path_count - 1) {
            float t = distance / length;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            position = m3d_v(a.x + dx * t, a.y + dy * t, a.z + dz * t);
            tangent = m3d_v(dx / length, dy / length, dz / length);
            break;
        }
        distance -= length;
    }

    const float train_scale = 0.35f;
    const float wheel_rotation = elapsed * (0.65f / 0.15f);
    m3d_train_scale = train_scale;
    m3d_train_yaw = atan2f(tangent.z, tangent.x);
    m3d_train_pitch = atan2f(tangent.y,
                             sqrtf(tangent.x * tangent.x + tangent.z * tangent.z));
    m3d_translate_x = position.x;
    /* The model pivot is the axle center, so keep the wheels just touching
       the rail top regardless of the train's scale or the track elevation. */
    m3d_translate_y = position.y + 0.05f + 0.15f * train_scale;
    m3d_translate_z = position.z;
    m3d_train_transform_active = 1;

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

    m3d_train_transform_active = 0;
    m3d_train_scale = 1.0f;
    m3d_train_yaw = 0.0f;
    m3d_train_pitch = 0.0f;
    m3d_translate_x = 0.0f;
    m3d_translate_y = 0.0f;
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
                    HEX(0x8D5D35), layout);
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
                     5.06f, 3.55f, HEX(0x56351F), layout);
    m3d_screen_prism(right - 7.0f, top, right, bottom,
                     5.06f, 3.55f, HEX(0x4A2616), layout);
    m3d_screen_quad(left + 1.0f, top + 5.0f, left + 2.5f, bottom - 8.0f,
                    5.075f, HEX(0xBD8A4E), layout);
    m3d_screen_quad(right - 2.5f, top + 5.0f, right - 1.0f, bottom - 8.0f,
                    5.075f, HEX(0xA8733C), layout);

    /* The top crown is a separate extrusion, rather than a frame drawn over UI. */
    m3d_screen_prism(left, top, right, top + 6.0f,
                     5.04f, 3.86f, HEX(0x5A3823), layout);
    m3d_screen_face(left, top, 4.99f, right, top, 4.99f,
                    right + 5.0f, top - 5.5f, 4.62f,
                    left - 5.0f, top - 5.5f, 4.62f,
                    HEX(0x946637), layout);
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
                     5.08f, 4.48f, HEX(0x684328), layout);
    m3d_screen_quad(left, bottom + 1.0f, right, bottom + 4.0f,
                    5.105f, HEX(0xD6AD69), layout);
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

    /* Fine grain on the broad crown and the narrow vertical stiles. A slightly
       darker line paired with a warm highlight suggests varnished walnut. */
    m3d_screen_quad(left + 22.0f, top + 3.4f, right - 22.0f, top + 3.8f,
                    5.06f, HEX(0x51311E), layout);
    m3d_screen_quad(left + 46.0f, top + 4.7f, right - 46.0f, top + 5.0f,
                    5.061f, HEX(0x9A6B3E), layout);
    m3d_screen_quad(left + 3.4f, top + 19.0f, left + 3.9f, bottom - 22.0f,
                    5.085f, HEX(0x402617), layout);
    m3d_screen_quad(left + 5.1f, top + 28.0f, left + 5.35f, bottom - 35.0f,
                    5.086f, HEX(0x98643A), layout);
    m3d_screen_quad(right - 4.0f, top + 19.0f, right - 3.5f, bottom - 22.0f,
                    5.085f, HEX(0x28160E), layout);
    m3d_screen_quad(right - 5.4f, top + 32.0f, right - 5.15f, bottom - 38.0f,
                    5.086f, HEX(0x83542F), layout);
}


/* Convert launcher logical coordinates back into this renderer's image plane,
   then invert the perspective camera at a chosen depth. That lets the physical
   cubbies line up exactly underneath SDL's still-interactive text and sprites. */
static M3DVec3 m3d_screen_point(float logical_x, float logical_y, float z,
                                const M3DMuseumLayout *layout)
{
    logical_x = layout->ui_origin_x +
                (logical_x - layout->ui_origin_x) * layout->ui_scale_x;
    logical_y = layout->ui_origin_y +
                (logical_y - layout->ui_origin_y) * layout->ui_scale_y;
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
    const float tx0 = layout->ui_origin_x + (x - layout->ui_origin_x) * layout->ui_scale_x;
    const float ty0 = layout->ui_origin_y + (y - layout->ui_origin_y) * layout->ui_scale_y;
    const float tx1 = layout->ui_origin_x +
                      (x + width - layout->ui_origin_x) * layout->ui_scale_x;
    const float ty1 = layout->ui_origin_y +
                      (y + height - layout->ui_origin_y) * layout->ui_scale_y;
    const float sx_scale = (float)M3D_W / (layout->view_w + 58.0f);
    const float sy_scale = (float)M3D_H / (layout->view_h + 36.0f);
    const float sx0 = (tx0 - layout->view_x + 29.0f) * sx_scale;
    const float sy0 = (ty0 - layout->view_y + 18.0f) * sy_scale;
    const float sx1 = (tx1 - layout->view_x + 29.0f) * sx_scale;
    const float sy1 = (ty1 - layout->view_y + 18.0f) * sy_scale;

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



/* Screen-anchored ribbon segments build the curved brass shackle with
   depth-tested geometry while preserving the live UI alignment. */
static void m3d_screen_ribbon_segment(float ax, float ay, float bx, float by,
                                      float thickness, float z, uint32_t color,
                                      const M3DMuseumLayout *layout)
{
    const float dx = bx - ax, dy = by - ay;
    const float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.0001f || thickness <= 0.0f) return;
    const float nx = -dy / length * thickness * 0.5f;
    const float ny =  dx / length * thickness * 0.5f;
    m3d_screen_face(ax + nx, ay + ny, z,
                    bx + nx, by + ny, z,
                    bx - nx, by - ny, z,
                    ax - nx, ay - ny, z, color, layout);
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

    /* A narrow specular streak travels slowly over the polished gold face,
       borrowing the same warm, restrained glint used in the field-guide art. */
    float glint_span = width - 12.0f - height * 0.22f - 2.0f;
    if (glint_span < 1.0f) glint_span = 1.0f;
    const float glint_x = x + 5.0f +
        fmodf(layout->elapsed_seconds * 7.0f, glint_span);
    const float glint_z = highlight_z + 0.012f;
    m3d_screen_face(glint_x, y + 2.5f, glint_z,
                    glint_x + 2.0f, y + 2.5f, glint_z,
                    glint_x + 2.0f + height * 0.22f, bottom - 2.8f, glint_z,
                    glint_x + height * 0.22f, bottom - 2.8f, glint_z,
                    HEX(0xFFF7D9), layout);

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

/* Raised wooden surround for the parchment Field Guide. The SDL paper is
   still rendered afterward, but these extruded rails remain visible around it
   and share the same camera/depth buffer as the collection shelf. */
static void m3d_field_guide_frame(const M3DMuseumLayout *layout)
{
    if (!layout) return;
    const float x = layout->museum_x + 1.0f;
    const float y = layout->museum_y + 63.0f;
    const float w = 450.0f, h = 413.0f;

    m3d_screen_quad(x - 2.0f, y - 2.0f, x + w + 2.0f, y + h + 2.0f,
                    3.30f, HEX(0x2A1308), layout);

    /* Outer dark reveal and the warm beveled hardwood face. */
    m3d_screen_prism(x - 1.0f, y - 1.0f, x + w + 1.0f, y + 8.0f,
                     5.09f, 3.94f, HEX(0x3D1E0E), layout);
    m3d_screen_prism(x, y, x + w, y + 6.0f,
                     5.13f, 4.16f, HEX(0x6A4329), layout);
    m3d_screen_prism(x + 2.0f, y + 1.0f, x + w - 2.0f, y + 3.2f,
                     5.155f, 4.40f, HEX(0x93683A), layout);
    m3d_screen_quad(x + 7.0f, y + 1.6f, x + w - 7.0f, y + 2.3f,
                    5.18f, HEX(0xF0C982), layout);
    m3d_screen_quad(x + 8.0f, y + 4.4f, x + w - 8.0f, y + 5.1f,
                    5.18f, HEX(0x47230F), layout);

    m3d_screen_prism(x - 1.0f, y + h - 8.0f, x + w + 1.0f, y + h + 1.0f,
                     5.09f, 3.94f, HEX(0x32180B), layout);
    m3d_screen_prism(x, y + h - 6.0f, x + w, y + h,
                     5.13f, 4.16f, HEX(0x52321D), layout);
    m3d_screen_prism(x + 2.0f, y + h - 3.2f, x + w - 2.0f, y + h - 1.0f,
                     5.155f, 4.40f, HEX(0x7D522F), layout);
    m3d_screen_quad(x + 7.0f, y + h - 2.3f, x + w - 7.0f, y + h - 1.6f,
                    5.18f, HEX(0xE7BD78), layout);

    m3d_screen_prism(x - 1.0f, y + 7.0f, x + 8.0f, y + h - 7.0f,
                     5.09f, 3.94f, HEX(0x744525), layout);
    m3d_screen_prism(x, y + 8.0f, x + 6.0f, y + h - 8.0f,
                     5.13f, 4.16f, HEX(0x7C512F), layout);
    m3d_screen_quad(x + 1.3f, y + 15.0f, x + 2.0f, y + h - 15.0f,
                    5.18f, HEX(0xE4B774), layout);

    m3d_screen_prism(x + w - 8.0f, y + 7.0f, x + w + 1.0f, y + h - 7.0f,
                     5.09f, 3.94f, HEX(0x32180D), layout);
    m3d_screen_prism(x + w - 6.0f, y + 8.0f, x + w, y + h - 8.0f,
                     5.13f, 4.16f, HEX(0x784A28), layout);
    m3d_screen_quad(x + w - 2.0f, y + 15.0f, x + w - 1.3f, y + h - 15.0f,
                    5.18f, HEX(0xB27C44), layout);

    /* Small brass corner blocks finish the joinery without touching the
       clickable area of the header, specimen window, or field notes. */
    for (int side = 0; side < 2; ++side) {
        const float cx = side ? x + w - 4.0f : x + 2.0f;
        for (int row = 0; row < 2; ++row) {
            const float cy = row ? y + h - 5.0f : y + 2.0f;
            m3d_screen_quad(cx, cy, cx + 2.0f, cy + 2.0f,
                            5.19f, HEX(0xE6BE78), layout);
        }
    }
}

static void m3d_screen_cubby(float x, float y, float w, float h, int locked,
                             const M3DMuseumLayout *layout)
{
    /* A single cabinet-maker's pocket: shadow reveal, deep walls, hard back,
       layered walnut lip and a separate accession plate. All scene geometry
       shares the same camera/depth buffer; only collectible art and text are
       supplied by the still-interactive SDL layer. */
    const float front_z = 5.26f;
    const float rear_z = 3.36f;
    const float opening_left = x + 6.0f, opening_right = x + w - 6.0f;
    const float opening_top = y + 6.0f, opening_bottom = y + h - 25.0f;

    M3DVec3 ft_l = m3d_screen_point(opening_left, opening_top, front_z, layout);
    M3DVec3 ft_r = m3d_screen_point(opening_right, opening_top, front_z, layout);
    M3DVec3 fb_r = m3d_screen_point(opening_right, opening_bottom, front_z, layout);
    M3DVec3 fb_l = m3d_screen_point(opening_left, opening_bottom, front_z, layout);
    M3DVec3 rt_l = m3d_v(ft_l.x, ft_l.y, rear_z);
    M3DVec3 rt_r = m3d_v(ft_r.x, ft_r.y, rear_z);
    M3DVec3 rb_r = m3d_v(fb_r.x, fb_r.y, rear_z);
    M3DVec3 rb_l = m3d_v(fb_l.x, fb_l.y, rear_z);

    /* Rear apron is screen-aligned and opaque so there is no line of sight
       through the furniture. The smaller world-space core sits in front of it,
       creating a genuine inset instead of a transparent perspective tunnel. */
    m3d_screen_quad(opening_left, opening_top, opening_right, opening_bottom,
                    3.14f, HEX(0x4B2C18), layout);
    m3d_screen_quad(opening_left + 3.0f, opening_top + 3.0f,
                    opening_right - 3.0f, opening_top + 4.0f,
                    3.155f, HEX(0x966439), layout);
    m3d_screen_quad(opening_left + 3.0f, opening_bottom - 4.0f,
                    opening_right - 3.0f, opening_bottom - 3.0f,
                    3.155f, HEX(0x211006), layout);
    m3d_quad(rt_l, rt_r, rb_r, rb_l, HEX(0x120804));
    m3d_quad(ft_l, ft_r, rt_r, rt_l, HEX(0x6B452A));
    m3d_quad(ft_l, rt_l, rb_l, fb_l, HEX(0x52321E));
    m3d_quad(ft_r, fb_r, rb_r, rt_r, HEX(0x3A1D10));
    m3d_quad(fb_l, rb_l, rb_r, fb_r, HEX(0x54301B));

    /* The face is built in three steps: dark joinery shadow, thick walnut,
       then a narrow lighter bevel on the interior edge. */
    m3d_screen_prism(x - 1.0f, y - 1.0f, x + w + 1.0f, y + 6.0f,
                     5.23f, 4.10f, HEX(0x3A1D0E), layout);
    m3d_screen_prism(x, y, x + w, y + 5.0f,
                     5.29f, 4.28f, HEX(0x5C3922), layout);
    m3d_screen_prism(x + 1.0f, y + 1.0f, x + w - 1.0f, y + 3.2f,
                     5.32f, 4.46f, HEX(0x79502F), layout);

    m3d_screen_prism(x - 1.0f, y + 4.5f, x + 6.0f, y + h - 25.0f,
                     5.25f, 4.10f, HEX(0x4A2814), layout);
    m3d_screen_prism(x, y + 5.0f, x + 5.0f, y + h - 25.0f,
                     5.29f, 4.28f, HEX(0x754628), layout);
    m3d_screen_prism(x + 3.4f, y + 7.0f, x + 4.5f, y + h - 27.0f,
                     5.32f, 4.43f, HEX(0xA6793E), layout);

    m3d_screen_prism(x + w - 6.0f, y + 4.5f, x + w + 1.0f, y + h - 25.0f,
                     5.25f, 4.10f, HEX(0x271207), layout);
    m3d_screen_prism(x + w - 5.0f, y + 5.0f, x + w, y + h - 25.0f,
                     5.29f, 4.28f, HEX(0x4A2615), layout);
    m3d_screen_quad(x + w - 4.5f, y + 7.0f, x + w - 3.4f, y + h - 27.0f,
                    5.32f, HEX(0x2A1409), layout);

    /* A broad shelf nose supports the plate and reads as a projecting board. */
    m3d_screen_prism(x + 3.0f, y + h - 25.0f, x + w - 3.0f, y + h - 18.0f,
                     5.31f, 4.42f, HEX(0x5D3923), layout);
    m3d_screen_prism(x + 4.0f, y + h - 25.0f, x + w - 4.0f, y + h - 22.0f,
                     5.34f, 4.62f, HEX(0xA16B34), layout);
    m3d_screen_quad(x + 6.0f, y + h - 24.0f, x + w - 6.0f, y + h - 23.0f,
                    5.365f, HEX(0xDDB16D), layout);
    m3d_screen_quad(x + 5.0f, y + h - 19.0f, x + w - 5.0f, y + h - 18.0f,
                    5.34f, HEX(0x38200F), layout);

    /* Discovered objects sit on an individual brass-and-walnut display riser.
       The SDL artwork is drawn afterward, so transparent pixel-art edges can
       overlap the stand naturally without being baked into a flat sprite card. */
    if (!locked) {
        const float stand_left = x + 22.0f;
        const float stand_right = x + w - 22.0f;
        const float stand_top = y + h - 40.0f;
        m3d_screen_prism(stand_left - 2.0f, stand_top + 4.0f,
                         stand_right + 2.0f, y + h - 28.0f,
                         5.285f, 4.62f, HEX(0x3A1C0D), layout);
        m3d_screen_prism(stand_left, stand_top + 2.0f,
                         stand_right, y + h - 31.0f,
                         5.32f, 4.78f, HEX(0x7E4B25), layout);
        m3d_screen_prism(stand_left + 2.0f, stand_top,
                         stand_right - 2.0f, stand_top + 4.0f,
                         5.35f, 4.94f, HEX(0xC18B45), layout);
        m3d_screen_quad(stand_left + 4.0f, stand_top + 0.8f,
                        stand_right - 4.0f, stand_top + 1.6f,
                        5.375f, HEX(0xF7D68D), layout);
        m3d_screen_quad(stand_left + 3.0f, y + h - 30.5f,
                        stand_right - 3.0f, y + h - 29.7f,
                        5.34f, HEX(0xE1B56C), layout);
    }

    /* Undiscovered items have an actual recessed roller shutter in the pocket. */
    if (locked) {
        const float shutter_x = x + 14.0f, shutter_w = w - 28.0f;
        m3d_screen_prism(shutter_x - 2.0f, y + 12.0f,
                         shutter_x + shutter_w + 2.0f, y + 47.0f,
                         5.20f, 4.62f, HEX(0x211F1A), layout);
        for (int i = 0; i < 5; ++i) {
            const float sy = y + 15.0f + i * 6.1f;
            const uint32_t face = i == 4 ? HEX(0x424741) : HEX(0x625A50);
            m3d_screen_prism(shutter_x, sy, shutter_x + shutter_w, sy + 5.2f,
                             5.23f, 4.65f, face, layout);
            m3d_screen_quad(shutter_x + 1.2f, sy + 0.3f,
                            shutter_x + shutter_w - 1.2f, sy + 1.0f,
                            5.265f, HEX(0xBFAF91), layout);
            m3d_screen_quad(shutter_x + 1.2f, sy + 4.1f,
                            shutter_x + shutter_w - 1.2f, sy + 4.8f,
                            5.265f, HEX(0x30342F), layout);
            m3d_screen_quad(shutter_x + 3.0f, sy + 1.7f,
                            shutter_x + 4.5f, sy + 3.0f,
                            5.27f, HEX(0xD3AC64), layout);
            m3d_screen_quad(shutter_x + shutter_w - 4.5f, sy + 1.7f,
                            shutter_x + shutter_w - 3.0f, sy + 3.0f,
                            5.27f, HEX(0xD3AC64), layout);
        }

        /* The padlock is genuine depth-buffered cabinet geometry, not the
           former flat SDL glyph. A thick brass shackle rises from a layered
           body, with a dark keyway, reflected edges, and tiny corner rivets. */
        const float lock_cx = x + w * 0.5f;
        const float shackle_l = lock_cx - 11.0f;
        const float shackle_r = lock_cx + 11.0f;
        const float shackle_top = y + 14.0f;
        const float lock_body_top = y + 29.0f;
        const float lock_body_bottom = y + 49.0f;

        /*
         * Rounded U-shaped steel, built from separate ribbon segments. Three
         * nested finishes—dark edge, bronze body, and a narrow gold catchlight—
         * give the shackle a curved profile. The bottom ends vanish behind the
         * lock body so the hardware reads as one assembled brass padlock.
         */
        m3d_screen_quad(lock_cx - 7.5f, shackle_top + 3.0f,
                        lock_cx + 7.5f, lock_body_top + 2.0f,
                        5.365f, HEX(0x21160D), layout);
        const float arch_x[9] = {
            lock_cx - 9.0f, lock_cx - 9.0f, lock_cx - 8.0f,
            lock_cx - 5.0f, lock_cx, lock_cx + 5.0f,
            lock_cx + 8.0f, lock_cx + 9.0f, lock_cx + 9.0f
        };
        const float arch_y[9] = {
            lock_body_top + 2.0f, y + 23.0f, y + 19.0f,
            shackle_top + 1.4f, shackle_top,
            shackle_top + 1.4f, y + 19.0f, y + 23.0f,
            lock_body_top + 2.0f
        };
        for (int segment = 0; segment < 8; ++segment) {
            m3d_screen_ribbon_segment(arch_x[segment], arch_y[segment],
                                      arch_x[segment + 1], arch_y[segment + 1],
                                      5.0f, 5.385f, HEX(0x4A270F), layout);
            m3d_screen_ribbon_segment(arch_x[segment], arch_y[segment],
                                      arch_x[segment + 1], arch_y[segment + 1],
                                      3.25f, 5.412f, HEX(0xC7963F), layout);
            m3d_screen_ribbon_segment(arch_x[segment], arch_y[segment],
                                      arch_x[segment + 1], arch_y[segment + 1],
                                      0.75f, 5.432f, HEX(0xFFE4A0), layout);
        }
        m3d_screen_prism(lock_cx - 17.0f, lock_body_top,
                         lock_cx + 17.0f, lock_body_bottom,
                         5.40f, 4.66f, HEX(0x3E210D), layout);
        m3d_screen_prism(lock_cx - 15.0f, lock_body_top + 1.5f,
                         lock_cx + 15.0f, lock_body_bottom - 1.0f,
                         5.445f, 4.83f, HEX(0xC7963F), layout);
        m3d_screen_quad(lock_cx - 12.5f, lock_body_top + 3.2f,
                        lock_cx + 12.5f, lock_body_bottom - 3.0f,
                        5.465f, HEX(0xDAB15F), layout);
        m3d_screen_quad(lock_cx - 11.0f, lock_body_top + 3.4f,
                        lock_cx + 11.0f, lock_body_top + 4.2f,
                        5.49f, HEX(0xFFE4A0), layout);
        m3d_screen_quad(lock_cx + 11.5f, lock_body_top + 4.0f,
                        lock_cx + 12.5f, lock_body_bottom - 4.0f,
                        5.49f, HEX(0x8B5D24), layout);
        /* Keyhole head and tapered lower stem are separate inset facets. */
        m3d_screen_quad(lock_cx - 2.0f, lock_body_top + 6.0f,
                        lock_cx + 2.0f, lock_body_top + 10.0f,
                        5.505f, HEX(0x39200E), layout);
        m3d_screen_face(lock_cx - 1.3f, lock_body_top + 9.0f, 5.51f,
                        lock_cx + 1.3f, lock_body_top + 9.0f, 5.51f,
                        lock_cx + 3.2f, lock_body_top + 14.0f, 5.51f,
                        lock_cx - 3.2f, lock_body_top + 14.0f, 5.51f,
                        HEX(0x39200E), layout);
        for (int side = 0; side < 2; ++side) {
            const float rivet_x = side ? lock_cx + 10.5f : lock_cx - 12.5f;
            m3d_screen_quad(rivet_x, lock_body_top + 8.0f,
                            rivet_x + 1.6f, lock_body_top + 9.6f,
                            5.50f, HEX(0x76501F), layout);
            m3d_screen_quad(rivet_x + 0.3f, lock_body_top + 8.2f,
                            rivet_x + 0.9f, lock_body_top + 8.7f,
                            5.52f, HEX(0xFFE4A0), layout);
        }
    }

    /* A polished brass accession plate is mounted into the shelf nose. */
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

    /* The display is now a single shadow-box assembly: dark joinery first,
       warm carved walnut rails second, and a thin brass bevel as the final
       edge. The recessed board sits behind every pocket and hides the train. */
    m3d_screen_prism(panel_x + 4.0f, panel_y + 9.0f,
                     panel_x + panel_w - 12.0f, panel_y + body_h - 9.0f,
                     3.02f, 2.30f, HEX(0x4B2B16), layout);
    m3d_screen_prism(panel_x + 7.0f, panel_y + 12.0f,
                     panel_x + panel_w - 15.0f, panel_y + body_h - 12.0f,
                     3.08f, 2.42f, HEX(0x241107), layout);

    /* Outer frame and inner bevel are independently extruded, rather than
       outlined rectangles painted on a flat board. */
    m3d_screen_prism(panel_x, panel_y + 4.0f,
                     panel_x + panel_w - 5.0f, panel_y + 11.0f,
                     5.05f, 3.72f, HEX(0x60371D), layout);
    m3d_screen_prism(panel_x + 1.5f, panel_y + 5.0f,
                     panel_x + panel_w - 7.0f, panel_y + 9.0f,
                     5.10f, 3.96f, HEX(0x85522D), layout);
    m3d_screen_quad(panel_x + 9.0f, panel_y + 5.9f,
                    panel_x + panel_w - 13.0f, panel_y + 6.8f,
                    5.135f, HEX(0xD3A15F), layout);
    m3d_screen_prism(panel_x, panel_y + 10.0f,
                     panel_x + 7.0f, panel_y + body_h - 5.0f,
                     5.05f, 3.72f, HEX(0x704326), layout);
    m3d_screen_prism(panel_x + 1.5f, panel_y + 12.0f,
                     panel_x + 5.0f, panel_y + body_h - 7.0f,
                     5.10f, 3.96f, HEX(0x986638), layout);
    m3d_screen_quad(panel_x + 3.0f, panel_y + 18.0f,
                    panel_x + 4.0f, panel_y + body_h - 14.0f,
                    5.135f, HEX(0xE3B978), layout);
    m3d_screen_prism(panel_x + panel_w - 12.0f, panel_y + 10.0f,
                     panel_x + panel_w - 5.0f, panel_y + body_h - 5.0f,
                     5.05f, 3.72f, HEX(0x291309), layout);
    m3d_screen_prism(panel_x + panel_w - 10.0f, panel_y + 12.0f,
                     panel_x + panel_w - 6.5f, panel_y + body_h - 7.0f,
                     5.10f, 3.96f, HEX(0x4A2615), layout);
    m3d_screen_prism(panel_x + 5.0f, panel_y + body_h - 12.0f,
                     panel_x + panel_w - 10.0f, panel_y + body_h - 5.0f,
                     5.05f, 3.72f, HEX(0x4A2715), layout);
    m3d_screen_prism(panel_x + 7.0f, panel_y + body_h - 10.0f,
                     panel_x + panel_w - 12.0f, panel_y + body_h - 6.5f,
                     5.10f, 3.96f, HEX(0x83502A), layout);

    /* Recessed header fascia, gold sign and three wooden filter keys remain
       exactly behind their current clickable SDL counterparts. */
    if (!layout->draw_cubby_grid) return;
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

    const float filter_x = panel_x + 9.0f;
    const float filter_y = panel_y + 40.0f;
    const float filter_w = (panel_w - 35.0f) / 3.0f;
    for (int filter = 0; filter < 3; ++filter) {
        const float key_x = filter_x + (filter_w + 4.0f) * (float)filter;
        const float key_right = key_x + filter_w;
        m3d_screen_prism(key_x, filter_y, key_right, filter_y + 24.0f,
                         5.16f, 4.40f,
                         filter == 0 ? HEX(0x79502E) : HEX(0x57331E), layout);
        m3d_screen_prism(key_x + 2.0f, filter_y + 2.0f,
                         key_right - 2.0f, filter_y + 20.0f,
                         5.18f, 4.52f, filter == 0 ? HEX(0x8E6036) : HEX(0x684027), layout);
        m3d_screen_quad(key_x + 3.0f, filter_y + 1.4f,
                        key_right - 3.0f, filter_y + 2.3f,
                        5.205f, HEX(0xD1A06A), layout);
        m3d_screen_quad(key_x + 3.0f, filter_y + 21.7f,
                        key_right - 3.0f, filter_y + 22.6f,
                        5.205f, HEX(0x31190D), layout);
    }

    /* A second shadow line and raised rails make the grid a replaceable insert
       set into the larger cabinet, not cards floating in an empty rectangle. */
    m3d_screen_prism(grid_x - 7.0f, grid_y - 7.0f,
                     grid_x + grid_w + 7.0f, grid_y,
                     5.08f, 3.62f, HEX(0x342011), layout);
    m3d_screen_prism(grid_x - 5.0f, grid_y - 5.0f,
                     grid_x + grid_w + 5.0f, grid_y - 1.0f,
                     5.14f, 3.94f, HEX(0xA17040), layout);
    m3d_screen_quad(grid_x - 3.0f, grid_y - 4.0f,
                    grid_x + grid_w + 3.0f, grid_y - 3.0f,
                    5.17f, HEX(0xF0CD8A), layout);
    m3d_screen_prism(grid_x - 7.0f, grid_y - 1.0f,
                     grid_x - 1.0f, grid_y + grid_h + 7.0f,
                     5.08f, 3.62f, HEX(0x57331D), layout);
    m3d_screen_prism(grid_x - 5.0f, grid_y,
                     grid_x - 1.0f, grid_y + grid_h + 5.0f,
                     5.14f, 3.94f, HEX(0x9D6B3C), layout);
    m3d_screen_quad(grid_x - 3.5f, grid_y + 4.0f,
                    grid_x - 2.5f, grid_y + grid_h - 4.0f,
                    5.17f, HEX(0xE2B676), layout);
    m3d_screen_prism(grid_x + grid_w + 1.0f, grid_y - 1.0f,
                     grid_x + grid_w + 7.0f, grid_y + grid_h + 7.0f,
                     5.08f, 3.62f, HEX(0x291309), layout);
    m3d_screen_prism(grid_x + grid_w + 1.0f, grid_y,
                     grid_x + grid_w + 5.0f, grid_y + grid_h + 5.0f,
                     5.14f, 3.94f, HEX(0x57321D), layout);
    m3d_screen_prism(grid_x - 7.0f, grid_y + grid_h,
                     grid_x + grid_w + 7.0f, grid_y + grid_h + 7.0f,
                     5.08f, 3.62f, HEX(0x4A2815), layout);
    m3d_screen_prism(grid_x - 5.0f, grid_y + grid_h,
                     grid_x + grid_w + 5.0f, grid_y + grid_h + 5.0f,
                     5.14f, 3.94f, HEX(0x8B592F), layout);
    m3d_screen_quad(grid_x - 3.0f, grid_y + grid_h + 1.0f,
                    grid_x + grid_w + 3.0f, grid_y + grid_h + 2.0f,
                    5.17f, HEX(0xD7A466), layout);

    /* Full opaque backboard is a depth-tested plane behind the pockets. */
    m3d_screen_prism(grid_x - 0.5f, grid_y,
                     grid_x + grid_w + 0.5f, grid_y + grid_h,
                     3.10f, 2.54f, HEX(0x1D0D06), layout);

    int total_rows = (layout->visible_entries + 2) / 3;
    /* Keep the physical cabinet dressed even when a filter currently exposes
       fewer than twelve records. Empty positions are closed, locked specimen
       drawers instead of bare vertical voids beneath the last populated row. */
    if (total_rows < 4) total_rows = 4;
    for (int row = 0; row < total_rows; ++row) {
        const float sy = grid_y + row * row_step + tile_h - layout->scroll;
        if (sy < grid_y - 8.0f || sy > grid_y + grid_h) continue;
        m3d_screen_prism(grid_x - 1.0f, sy, grid_x + grid_w + 1.0f, sy + 7.5f,
                         5.12f, 3.80f, HEX(0x694329), layout);
        m3d_screen_prism(grid_x, sy, grid_x + grid_w, sy + 1.2f,
                         5.16f, 4.95f, HEX(0xE8BE7A), layout);
        m3d_screen_quad(grid_x, sy + 6.3f, grid_x + grid_w, sy + 7.1f,
                        5.165f, HEX(0x2A1409), layout);
    }

    /* Each divider has a broad wooden body and two edge treatments. */
    m3d_set_logical_clip(grid_x, grid_y, grid_w, grid_h, layout);
    for (int divider = 0; divider < 2; ++divider) {
        const float dx = grid_x + (divider + 1) * tile_w + divider * gap_x;
        m3d_screen_prism(dx, grid_y - 1.0f, dx + gap_x, grid_y + grid_h + 1.0f,
                         5.10f, 3.78f, HEX(0x754525), layout);
        m3d_screen_prism(dx + 0.6f, grid_y, dx + 2.0f, grid_y + grid_h,
                         5.14f, 4.94f, HEX(0xD6A66E), layout);
        m3d_screen_prism(dx + gap_x - 1.7f, grid_y, dx + gap_x - 0.3f, grid_y + grid_h,
                         5.14f, 4.94f, HEX(0x241106), layout);
    }

    if (layout->visible_entries > 0) {
        int first_row = (int)floorf(layout->scroll / row_step);
        if (first_row < 0) first_row = 0;
        for (int row = first_row; row < first_row + 6 && row < total_rows; ++row) {
            const float ty = grid_y + row * row_step - layout->scroll;
            if (ty + tile_h < grid_y || ty > grid_y + grid_h) continue;
            for (int col = 0; col < 3; ++col) {
                const int index = row * 3 + col;
                const float tx = grid_x + col * (tile_w + gap_x);
                if (index >= layout->visible_entries) {
                    if (layout->visible_entries > 0 &&
                        layout->visible_entries < 12 && layout->scroll < 0.01f) {
                        m3d_screen_cubby(tx, ty, tile_w, tile_h, 1, layout);
                    }
                    continue;
                }
                const int locked = layout->entry_locked ? layout->entry_locked[index] : 0;
                m3d_screen_cubby(tx, ty, tile_w, tile_h, locked, layout);
            }
        }
    }
    m3d_reset_clip();
}


/* A short extruded rail following a path in the X/Z floor plane. Narrow
   cross-sections make linked segments read as a continuous rounded track
   instead of long axis-aligned strips with abrupt square corners. */
static void m3d_track_rail_segment(M3DVec3 a, M3DVec3 b,
                                   float half_width, float height,
                                   uint32_t material)
{
    float dx = b.x - a.x, dz = b.z - a.z;
    float length = sqrtf(dx * dx + dz * dz);
    if (length < 0.0001f) return;
    const float px = -dz / length * half_width;
    const float pz =  dx / length * half_width;

    M3DVec3 bl = m3d_v(a.x + px, a.y, a.z + pz);
    M3DVec3 br = m3d_v(a.x - px, a.y, a.z - pz);
    M3DVec3 el = m3d_v(b.x + px, b.y, b.z + pz);
    M3DVec3 er = m3d_v(b.x - px, b.y, b.z - pz);
    M3DVec3 blt = m3d_v(bl.x, bl.y + height, bl.z);
    M3DVec3 brt = m3d_v(br.x, br.y + height, br.z);
    M3DVec3 elt = m3d_v(el.x, el.y + height, el.z);
    M3DVec3 ert = m3d_v(er.x, er.y + height, er.z);

    m3d_quad(blt, elt, ert, brt, material);
    m3d_quad(bl, el, elt, blt, m3d_tint(material, 0.62f));
    m3d_quad(br, brt, ert, er, m3d_tint(material, 0.47f));
    m3d_quad(bl, br, brt, blt, m3d_tint(material, 0.70f));
}

/* One cross-tie spanning the paired rails at a sampled point on the route. */
static void m3d_track_sleeper(M3DVec3 center, float nx, float nz,
                              float half_gauge, uint32_t color)
{
    float length = sqrtf(nx * nx + nz * nz);
    if (length < 0.0001f) return;
    nx /= length;
    nz /= length;
    const float tx = -nz, tz = nx;
    const float across = half_gauge + 0.085f;
    const float along = 0.052f;
    const float base_y = center.y - 0.065f, top_y = center.y + 0.015f;
    M3DVec3 base[4], top[4];
    const float nxv[4] = { across, across, -across, -across };
    const float txv[4] = { along, -along, -along, along };
    for (int i = 0; i < 4; ++i) {
        const float px = center.x + nx * nxv[i] + tx * txv[i];
        const float pz = center.z + nz * nxv[i] + tz * txv[i];
        base[i] = m3d_v(px, base_y, pz);
        top[i] = m3d_v(px, top_y, pz);
    }
    m3d_quad(top[0], top[1], top[2], top[3], color);
    m3d_quad(base[0], base[1], top[1], top[0], m3d_tint(color, 0.80f));
    m3d_quad(base[1], base[2], top[2], top[1], m3d_tint(color, 0.58f));
    m3d_quad(base[2], base[3], top[3], top[2], m3d_tint(color, 0.68f));
    m3d_quad(base[3], base[0], top[0], top[3], m3d_tint(color, 0.88f));
}

static void m3d_scene(float elapsed, const M3DMuseumLayout *layout)
{
    m3d_clip_active = 0;
    m3d_translate_x = 0.0f;
    m3d_translate_y = 0.0f;
    m3d_translate_z = 0.0f;
    m3d_train_scale = 1.0f;
    m3d_clear();

    /* Back wall and its panelled timber frame. */
    m3d_box(-8.5f, -0.1f, -4.55f, 8.5f, 8.2f, -4.0f, HEX(0x573523));
    m3d_box(-8.4f, 6.9f, -3.98f, 8.4f, 7.35f, -3.82f, HEX(0x98643C));
    m3d_box(-8.4f, 0.0f, -3.98f, 8.4f, 0.34f, -3.82f, HEX(0x6A4329));
    for (int i = 0; i < 13; ++i) {
        float x = -8.0f + (float)i * 1.33f;
        m3d_box(x, 0.38f, -3.96f, x + 0.045f, 6.87f, -3.84f,
                HEX(0x8B5B37));
    }
    /* Long, restrained vertical grain follows the timber panels. The paired
       dark/light traces catch the room light without turning the wall into
       a noisy striped texture, and a few short knots break the repetition. */
    for (int i = 0; i < 13; ++i) {
        const float gx = -7.78f + (float)i * 1.33f;
        const uint32_t dark_grain = (i % 2) ? HEX(0x4A2C1B) : HEX(0x4E2F1C);
        const uint32_t light_grain = (i % 3) ? HEX(0x765038) : HEX(0x6E482F);
        m3d_box(gx, 0.70f, -3.988f, gx + 0.018f, 6.55f, -3.978f,
                dark_grain);
        m3d_box(gx + 0.038f, 0.90f, -3.987f, gx + 0.047f, 6.30f, -3.978f,
                light_grain);
        if (i % 3 == 1) {
            const float knot_y = 1.45f + (float)(i % 4) * 0.78f;
            m3d_box(gx + 0.065f, knot_y, -3.987f,
                    gx + 0.17f, knot_y + 0.024f, -3.978f,
                    HEX(0x402515));
            m3d_box(gx + 0.088f, knot_y + 0.008f, -3.986f,
                    gx + 0.145f, knot_y + 0.015f, -3.977f,
                    HEX(0x6B452C));
        }
    }
    /* A raised brass-edged arch frames the cabinet from behind. */
    m3d_box(-5.5f, 1.0f, -3.72f, -5.17f, 6.0f, -3.47f, HEX(0xBF8E4F));
    m3d_box(5.17f, 1.0f, -3.72f, 5.5f, 6.0f, -3.47f, HEX(0xBF8E4F));
    m3d_box(-5.5f, 5.68f, -3.72f, 5.5f, 6.0f, -3.47f, HEX(0xD8AC65));
    m3d_box(-5.05f, 1.15f, -3.44f, -4.88f, 5.60f, -3.30f, HEX(0x5B3624));
    m3d_box(4.88f, 1.15f, -3.44f, 5.05f, 5.60f, -3.30f, HEX(0x5B3624));

    /* A short hollow outlet rises from the rear wall over the cabinet. The
       elevated railway passes in front of it, while the cabinet naturally
       hides the lower end—matching the layered pipe/rail composition shown
       in the polished reference rather than adding a floating green cylinder. */
    m3d_pipe_tube(-0.30f, 3.70f, -3.18f, 0.66f, 0.22f);

    /* Floor slab and separate boards with real perspective and depth testing. */
    m3d_box(-9.0f, -0.35f, -4.6f, 9.0f, -0.01f, 8.5f, HEX(0x55321F));
    for (int i = 0; i < 17; ++i) {
        float x = -8.4f + (float)i * 1.05f;
        uint32_t wood = (i % 3 == 0) ? HEX(0x7E5332) :
                        (i % 3 == 1) ? HEX(0x684329) : HEX(0x72492C);
        m3d_box(x, 0.002f, -4.4f, x + 0.96f, 0.025f, 8.4f, wood);
        m3d_box(x + 0.96f, 0.003f, -1.5f, x + 0.99f, 0.026f, 8.4f,
                HEX(0x392116));
        /* Thin grain runs with the length of each board, plus short broken
           marks so the floor reads as timber rather than repeated flat slabs. */
        for (int grain = 0; grain < 3; ++grain) {
            const float gx = x + 0.18f + (float)((i + grain * 2) % 4) * 0.13f;
            const float gz = -1.25f + (float)grain * 2.35f;
            const float grain_len = 0.78f + (float)((i + grain) % 4) * 0.19f;
            const uint32_t grain_color = ((i + grain) % 2)
                ? HEX(0x583722) : HEX(0x805532);
            m3d_box(gx, 0.026f, gz, gx + 0.016f, 0.030f,
                    gz + grain_len, grain_color);
        }
    }
    m3d_box(-8.8f, 0.03f, 7.5f, 8.8f, 0.10f, 7.7f, HEX(0xA36C3D));

    /* The museum furniture is real perspective geometry; controls are layered later. */
    m3d_cabinet(layout);
    m3d_field_guide_frame(layout);
    if (layout) m3d_museum_grid(layout);

    /* Physical green pipe models sit beside the display housing. */
    /* Pull the framing pipes slightly into view so the side props read
       as a deliberate frame around the collection cabinet at 1100x720. */
    m3d_pipe(-7.20f, 0.80f);
    m3d_pipe(7.20f, 0.80f);

    /* Soft-lit museum plinths, then two original stylised toy figures. */
    m3d_box(-6.35f, 0.03f, 1.2f, -5.05f, 0.48f, 2.7f, HEX(0x8C6038));
    m3d_box(4.80f, 0.03f, 1.45f, 6.00f, 0.48f, 3.95f, HEX(0x8C6038));
    m3d_toad_figure(-6.05f, 1.82f, sinf(elapsed * 1.25f) * 0.025f);
    m3d_goomba_figure(5.05f, 3.05f, sinf(elapsed * 1.25f + 1.0f) * 0.025f);

    /* Floating question blocks are bevelled solids, not flat UI glyphs. */
    float block_bob = sinf(elapsed * 1.25f) * 0.09f;
    m3d_question_block(-4.5f, 4.55f, -0.75f, 0.65f, block_bob);
    m3d_question_block(4.45f, 4.30f, -0.80f, 0.72f, -block_bob * 0.72f);

    /* A faceted, rotating gold coin is a thin extruded 3D disk. */
    m3d_coin(-3.6f, 3.72f + sinf(elapsed * 1.7f) * 0.08f,
             0.2f, 0.40f, elapsed * 1.15f);

    /*
     * One continuous railway winds around the collection cabinet: elevated
     * behind the shelf, descending beside the green pipes, then returning
     * across the foreground floor. Rails, sleepers, and the moving train share
     * exactly the same smoothed path, so the locomotive cannot hover away from
     * the track or drive through the side pipes.
     */
    {
        const float half_gauge = 0.095f;
        const float rail_y = 0.090f;
        const uint32_t tie_color = HEX(0x624329);
        const uint32_t rail_color = HEX(0xB7BFC0);
        const uint32_t dark_rail_color = HEX(0x666D70);
        m3d_build_track_path();
        const M3DVec3 *path = m3d_track_path;
        M3DVec3 normal[M3D_TRACK_PATH_COUNT];
        M3DVec3 rail_a[M3D_TRACK_PATH_COUNT], rail_b[M3D_TRACK_PATH_COUNT];

        for (int i = 0; i < M3D_TRACK_PATH_COUNT; ++i) {
            const M3DVec3 prev = path[(i + M3D_TRACK_PATH_COUNT - 1) %
                                       M3D_TRACK_PATH_COUNT];
            const M3DVec3 next = path[(i + 1) % M3D_TRACK_PATH_COUNT];
            float dx = next.x - prev.x, dz = next.z - prev.z;
            float length = sqrtf(dx * dx + dz * dz);
            if (length < 0.0001f) length = 1.0f;
            normal[i] = m3d_v(-dz / length, 0.0f, dx / length);
            rail_a[i] = m3d_v(path[i].x + normal[i].x * half_gauge,
                               path[i].y, path[i].z + normal[i].z * half_gauge);
            rail_b[i] = m3d_v(path[i].x - normal[i].x * half_gauge,
                               path[i].y, path[i].z - normal[i].z * half_gauge);
        }

        /* Sleepers are evenly spaced along the 3D path, including the ramps. */
        for (int i = 0; i < M3D_TRACK_PATH_COUNT; ++i) {
            const int j = (i + 1) % M3D_TRACK_PATH_COUNT;
            const M3DVec3 a = path[i], b = path[j];
            const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            const float length = sqrtf(dx * dx + dy * dy + dz * dz);
            int steps = (int)ceilf(length / 0.28f);
            if (steps < 1) steps = 1;
            for (int n = 0; n < steps; ++n) {
                const float t = (float)n / (float)steps;
                M3DVec3 center = m3d_v(a.x + dx * t, a.y + dy * t, a.z + dz * t);
                float nx = normal[i].x + (normal[j].x - normal[i].x) * t;
                float nz = normal[i].z + (normal[j].z - normal[i].z) * t;
                m3d_track_sleeper(center, nx, nz, half_gauge, tie_color);
            }
        }

        for (int i = 0; i < M3D_TRACK_PATH_COUNT; ++i) {
            const int j = (i + 1) % M3D_TRACK_PATH_COUNT;
            m3d_track_rail_segment(rail_a[i], rail_a[j], 0.026f, 0.050f,
                                   rail_color);
            m3d_track_rail_segment(rail_b[i], rail_b[j], 0.026f, 0.050f,
                                   dark_rail_color);
        }

        /* The train follows the very same centerline, including bankless climbs
           and the foreground return, with its wheels centered on both rails. */
        m3d_train(elapsed, path, M3D_TRACK_PATH_COUNT);
    }

    /*
     * Two little pipe mouths sit in the extreme foreground. Their bases and
     * short shafts straddle the foreground rail, so the track is visibly
     * behind them while all functional Museum text and controls remain drawn
     * on the front UI layer.
     */
    const float foreground_pipe_z = 3.75f;
    const float foreground_pipe_radius = 0.16f;
    const float foreground_pipe_positions[2] = {-4.65f, 4.65f};
    for (int i = 0; i < 2; ++i) {
        const float px = foreground_pipe_positions[i];
        m3d_box(px - foreground_pipe_radius - 0.10f, 0.015f,
                foreground_pipe_z - foreground_pipe_radius - 0.10f,
                px + foreground_pipe_radius + 0.10f, 0.095f,
                foreground_pipe_z + foreground_pipe_radius + 0.10f,
                HEX(0x70452A));
        m3d_pipe_tube(px, 0.08f, foreground_pipe_z, 0.34f,
                      foreground_pipe_radius);
    }

    /* Small brass exhibit lights hang high on each side of the room. */
    m3d_box(-6.6f, 5.7f, -3.4f, -5.9f, 5.85f, -3.05f, HEX(0xB5803D));
    m3d_sphere(-6.25f, 5.48f, -3.15f, 0.24f, HEX(0xFFD88A), 10, 5);
    m3d_box(5.9f, 5.7f, -3.4f, 6.6f, 5.85f, -3.05f, HEX(0xB5803D));
    m3d_sphere(6.25f, 5.48f, -3.15f, 0.24f, HEX(0xFFD88A), 10, 5);
}

void museum3d_draw_background(SDL_Renderer *renderer,
                              float x, float y, float width, float height,
                              float elapsed_seconds,
                              float museum_x, float museum_y,
                              float ui_origin_x, float ui_origin_y,
                              float ui_scale_x, float ui_scale_y,
                              float museum_scroll,
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
    layout.ui_origin_x = ui_origin_x; layout.ui_origin_y = ui_origin_y;
    layout.ui_scale_x = ui_scale_x; layout.ui_scale_y = ui_scale_y;
    layout.scroll = museum_scroll;
    layout.elapsed_seconds = elapsed_seconds;
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
        m3d_focal_x = 0.80f * m3d_focal * target_aspect / output_aspect;
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
