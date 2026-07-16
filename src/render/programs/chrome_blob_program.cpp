#include "render/programs/chrome_blob_program.h"

#include "render/core/render_styles.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  // This is an independent implementation of the observed smooth-blob
  // behaviour. It intentionally does not reuse shader/code from Caelestia.
  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

const int MAX_RECTS = 32;
uniform vec2 u_quad_size;
uniform vec2 u_pixel_scale;
uniform vec4 u_fill;
uniform vec4 u_shadow;
uniform vec4 u_aperture_insets; // left, top, right, bottom
uniform float u_rounding;
uniform float u_smoothing;
uniform float u_shadow_radius;
uniform vec2 u_shadow_offset;
uniform int u_rect_count;
uniform vec4 u_rects[MAX_RECTS];
// radius, deformation, union smoothing, joined-edge bit mask (top,right,bottom,left)
uniform vec4 u_rect_params[MAX_RECTS];
uniform int u_draw_frame;
uniform int u_debug;
uniform vec4 u_debug_input_rect;
uniform vec2 u_debug_input_params;
uniform float u_debug_progress;
uniform int u_debug_input_enabled;
varying vec2 v_pixel;

float rounded_box(vec2 point, vec4 rect, float radius, float deformation) {
    vec2 size = max(rect.zw, vec2(0.0));
    vec2 center = rect.xy + size * 0.5;
    vec2 p = point - center;
    float safe_radius = clamp(radius, 1.0, min(size.x, size.y) * 0.5);
    // Deformation is deliberately bounded and affects only the background
    // silhouette. Content receives the matching affine translation separately.
    float stretch = clamp(deformation, -0.12, 0.12);
    p.x /= 1.0 + stretch;
    p.y *= 1.0 + stretch * 0.35;
    vec2 q = abs(p) - (size * 0.5 - vec2(safe_radius));
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - safe_radius;
}

float rounded_local_box(vec2 point, vec2 size, float radius) {
    float safe_radius = clamp(radius, 1.0, min(size.x, size.y) * 0.5);
    vec2 q = abs(point - size * 0.5) - (size * 0.5 - vec2(safe_radius));
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - safe_radius;
}

float circle_extent(float radius, float delta) {
    return sqrt(max(0.0, radius * radius - delta * delta));
}

float corner_state(float packed_value, float divisor) {
    return mod(floor(packed_value / divisor), 3.0);
}

// Reproduces the explicit per-corner path. Insets are derived from the packed
// states because every concave wing is exactly one effective radius wide.
float panel_box(
    vec2 point, vec4 rect, float radius, float deformation,
    float packed_corner_shapes
) {
    vec2 size = max(rect.zw, vec2(1.0));
    vec2 centered = point - (rect.xy + size * 0.5);
    float stretch = clamp(deformation, -0.12, 0.12);
    centered.x /= 1.0 + stretch;
    centered.y *= 1.0 + stretch * 0.35;
    vec2 local = centered + size * 0.5;

    vec4 corner_shapes = vec4(
        corner_state(packed_corner_shapes, 1.0),
        corner_state(packed_corner_shapes, 3.0),
        corner_state(packed_corner_shapes, 9.0),
        corner_state(packed_corner_shapes, 27.0)
    );
    bool tl_h = corner_shapes.x > 0.5 && corner_shapes.x < 1.5;
    bool tr_h = corner_shapes.y > 0.5 && corner_shapes.y < 1.5;
    bool br_h = corner_shapes.z > 0.5 && corner_shapes.z < 1.5;
    bool bl_h = corner_shapes.w > 0.5 && corner_shapes.w < 1.5;
    bool tl_v = corner_shapes.x > 1.5;
    bool tr_v = corner_shapes.y > 1.5;
    bool br_v = corner_shapes.z > 1.5;
    bool bl_v = corner_shapes.w > 1.5;
    float r = max(radius, 1.0);
    vec4 safe_inset = vec4(
        (tl_h || bl_h) ? r : 0.0,
        (tl_v || tr_v) ? r : 0.0,
        (tr_h || br_h) ? r : 0.0,
        (bl_v || br_v) ? r : 0.0
    );
    vec2 body_min = min(safe_inset.xy, size);
    vec2 body_max = max(body_min, size - safe_inset.zw);
    vec2 body_size = max(body_max - body_min, vec2(1.0));
    r = clamp(r, 1.0, min(body_size.x, body_size.y) * 0.5);
    if (!(tl_h || tr_h || br_h || bl_h || tl_v || tr_v || br_v || bl_v)) {
        return rounded_local_box(local - body_min, body_size, r);
    }

    float x = local.x;
    float y = local.y;
    float left = body_min.x;
    float right = body_max.x;
    float top = body_min.y;
    float bottom = body_max.y;

    if (y < body_min.y + r) {
        float dy = clamp(y, body_min.y, body_min.y + r) - (body_min.y + r);
        float extent = circle_extent(r, dy);
        left = tl_h ? min(left, body_min.x - r + extent)
                    : (tl_v ? left : max(left, body_min.x + r - extent));
        right = tr_h ? max(right, body_max.x + r - extent)
                     : (tr_v ? right : min(right, body_max.x - r + extent));
    }
    if (x < body_min.x + r) {
        float dx = clamp(x, body_min.x, body_min.x + r) - (body_min.x + r);
        float extent = circle_extent(r, dx);
        top = tl_v ? min(top, body_min.y - r + extent)
                   : (tl_h ? top : max(top, body_min.y + r - extent));
        bottom = bl_v ? max(bottom, body_max.y + r - extent)
                      : (bl_h ? bottom : min(bottom, body_max.y - r + extent));
    }
    if (x > body_max.x - r) {
        float dx = clamp(x, body_max.x - r, body_max.x) - (body_max.x - r);
        float extent = circle_extent(r, dx);
        top = tr_v ? min(top, body_min.y - r + extent)
                   : (tr_h ? top : max(top, body_min.y + r - extent));
        bottom = br_v ? max(bottom, body_max.y + r - extent)
                      : (br_h ? bottom : min(bottom, body_max.y - r + extent));
    }
    if (y > body_max.y - r) {
        float dy = clamp(y, body_max.y - r, body_max.y) - (body_max.y - r);
        float extent = circle_extent(r, dy);
        left = bl_h ? min(left, body_min.x - r + extent)
                    : (bl_v ? left : max(left, body_min.x + r - extent));
        right = br_h ? max(right, body_max.x + r - extent)
                     : (br_v ? right : min(right, body_max.x - r + extent));
    }

    // Keep the exact inside test of the explicit path, but measure outside
    // corners in two dimensions. The previous Chebyshev distance made shadow
    // falloff square around concave wings even though the zero contour was
    // circular.
    float horizontal = max(left - x, x - right);
    float vertical = max(top - y, y - bottom);
    vec2 outside_axis = max(vec2(horizontal, vertical), vec2(0.0));
    float boundary = length(outside_axis) + min(max(horizontal, vertical), 0.0);
    float visual_clip = max(max(-x, x - size.x), max(-y, y - size.y));
    return max(boundary, visual_clip);
}

float smooth_union(float a, float b, float amount) {
    if (amount <= 0.001) return min(a, b);
    float h = clamp(0.5 + 0.5 * (b - a) / amount, 0.0, 1.0);
    return mix(b, a, h) - amount * h * (1.0 - h);
}

float circular_smooth_union(float a, float b, float amount) {
    if (amount <= 0.001) return min(a, b);
    vec2 support = max(vec2(amount) - vec2(a, b), vec2(0.0));
    return max(amount, min(a, b)) - length(support);
}

float joined_edge(float mask, float bit) {
    return mod(floor(mask / bit), 2.0);
}

vec4 render_rect(vec4 logical_rect, float joined_mask) {
    vec4 rect = logical_rect;
    // Caelestia keeps the popout physically attached during movement by
    // painting a generous hidden bridge underneath the frame. This bridge is
    // render-only: layout, clipping and input retain logical_rect.
    float horizontal_underlap = logical_rect.z * 0.2;
    float vertical_underlap = logical_rect.w * 0.2;
    if (joined_edge(joined_mask, 1.0) > 0.5) {
        rect.y -= vertical_underlap;
        rect.w += vertical_underlap;
    }
    if (joined_edge(joined_mask, 2.0) > 0.5) {
        rect.z += horizontal_underlap;
    }
    if (joined_edge(joined_mask, 4.0) > 0.5) {
        rect.w += vertical_underlap;
    }
    if (joined_edge(joined_mask, 8.0) > 0.5) {
        rect.x -= horizontal_underlap;
        rect.z += horizontal_underlap;
    }
    return rect;
}

float chrome_distance(vec2 point) {
    float d = 100000.0;
    vec2 inner_origin = u_aperture_insets.xy;
    vec2 inner_size = max(
        u_quad_size - vec2(
            u_aperture_insets.x + u_aperture_insets.z,
            u_aperture_insets.y + u_aperture_insets.w
        ),
        vec2(1.0)
    );
    vec4 inner = vec4(inner_origin, inner_size);
    if (u_draw_frame == 1) {
        // The output quad clips the outer half, so negating the inner SDF is
        // exactly an inverted rounded rectangle frame.
        d = -rounded_box(point, inner, u_rounding, 0.0);
    }
    for (int i = 0; i < MAX_RECTS; ++i) {
        if (i < u_rect_count) {
            vec4 rect = render_rect(u_rects[i], u_rect_params[i].w);
            float primitive = rounded_box(point, rect, u_rect_params[i].x, u_rect_params[i].y);
            float smoothing = u_rect_params[i].z < 0.0 ? u_smoothing : u_rect_params[i].z;
            if (u_rect_params[i].w > 0.5) smoothing = max(smoothing, u_smoothing);
            d = circular_smooth_union(d, primitive, smoothing);
        }
    }
    return d;
}

void main() {
    float d = chrome_distance(v_pixel);
    // Keep the antialiasing band close to one physical pixel at fractional
    // output scales. A fixed logical width caused a faint seam at the join.
    float aa = 0.85 / max(max(u_pixel_scale.x, u_pixel_scale.y), 0.001);
    float fill_coverage = 1.0 - smoothstep(-aa, aa, d);

    float shadow_distance = chrome_distance(v_pixel - u_shadow_offset);
    float shadow_coverage = (1.0 - smoothstep(0.0, max(u_shadow_radius, 0.01), shadow_distance))
        * (1.0 - fill_coverage);

    vec4 color = u_shadow;
    float alpha = color.a * shadow_coverage;
    vec3 premultiplied = color.rgb * alpha;

    float fill_alpha = u_fill.a * fill_coverage;
    premultiplied = u_fill.rgb * fill_alpha + premultiplied * (1.0 - fill_alpha);
    alpha = fill_alpha + alpha * (1.0 - fill_alpha);

    if (u_debug == 1) {
        float contour = 1.0 - smoothstep(0.75, 1.75, abs(d));
        float bounds = 0.0;
        if (u_draw_frame == 1) {
            vec2 inner_origin = u_aperture_insets.xy;
            vec2 inner_size = max(
                u_quad_size - vec2(
                    u_aperture_insets.x + u_aperture_insets.z,
                    u_aperture_insets.y + u_aperture_insets.w
                ),
                vec2(1.0)
            );
            float frame_edge = rounded_box(
                v_pixel, vec4(inner_origin, inner_size), u_rounding, 0.0
            );
            bounds = max(bounds, 1.0 - smoothstep(0.75, 1.75, abs(frame_edge)));
        }
        for (int i = 0; i < MAX_RECTS; ++i) {
            if (i < u_rect_count) {
                vec4 rect = render_rect(u_rects[i], u_rect_params[i].w);
                float primitive = rounded_box(
                    v_pixel, rect, u_rect_params[i].x, u_rect_params[i].y
                );
                bounds = max(bounds, 1.0 - smoothstep(0.75, 1.75, abs(primitive)));
            }
        }

        float input_outline = 0.0;
        if (u_debug_input_enabled == 1) {
            float input_distance = rounded_box(
                v_pixel, u_debug_input_rect, u_debug_input_params.x, u_debug_input_params.y
            );
            input_outline = 1.0 - smoothstep(2.25, 3.25, abs(input_distance));
        }

        // A compact state meter makes animation progress visible without text
        // or another scene node. Magenta means the panel currently accepts
        // input; amber means input has already been released during close.
        float meter_bg = 1.0 - smoothstep(
            -0.5, 0.5, rounded_box(v_pixel, vec4(8.0, 8.0, 128.0, 8.0), 4.0, 0.0)
        );
        float meter_width = 124.0 * clamp(u_debug_progress, 0.0, 1.0);
        float meter_fill = meter_width > 0.01
            ? 1.0 - smoothstep(
                -0.5, 0.5, rounded_box(v_pixel, vec4(10.0, 10.0, meter_width, 4.0), 2.0, 0.0)
              )
            : 0.0;

        vec3 debug_color = d <= 0.0 ? vec3(0.1, 1.0, 0.45) : vec3(1.0, 0.25, 0.2);
        float debug_alpha = max(contour * 0.9, bounds * 0.65);
        debug_color = mix(debug_color, vec3(0.25, 0.75, 1.0), bounds);
        debug_color = mix(debug_color, vec3(0.8, 0.2, 1.0), input_outline);
        debug_alpha = max(debug_alpha, input_outline * 0.95);
        debug_color = mix(debug_color, vec3(0.1), meter_bg * 0.85);
        debug_alpha = max(debug_alpha, meter_bg * 0.85);
        vec3 meter_color = u_debug_input_enabled == 1
            ? vec3(0.8, 0.2, 1.0)
            : vec3(1.0, 0.65, 0.1);
        debug_color = mix(debug_color, meter_color, meter_fill);
        debug_alpha = max(debug_alpha, meter_fill);
        premultiplied = debug_color * debug_alpha + premultiplied * (1.0 - debug_alpha);
        alpha = debug_alpha + alpha * (1.0 - debug_alpha);
    }

    if (alpha <= 0.001) discard;
    gl_FragColor = vec4(premultiplied, alpha);
}
)";

} // namespace

void ChromeBlobProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }
  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_quadSizeLocation = glGetUniformLocation(m_program.id(), "u_quad_size");
  m_pixelScaleLocation = glGetUniformLocation(m_program.id(), "u_pixel_scale");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");
  m_fillLocation = glGetUniformLocation(m_program.id(), "u_fill");
  m_shadowLocation = glGetUniformLocation(m_program.id(), "u_shadow");
  m_apertureInsetsLocation = glGetUniformLocation(m_program.id(), "u_aperture_insets");
  m_roundingLocation = glGetUniformLocation(m_program.id(), "u_rounding");
  m_smoothingLocation = glGetUniformLocation(m_program.id(), "u_smoothing");
  m_shadowRadiusLocation = glGetUniformLocation(m_program.id(), "u_shadow_radius");
  m_shadowOffsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_offset");
  m_rectCountLocation = glGetUniformLocation(m_program.id(), "u_rect_count");
  m_rectsLocation = glGetUniformLocation(m_program.id(), "u_rects[0]");
  m_rectParamsLocation = glGetUniformLocation(m_program.id(), "u_rect_params[0]");
  m_drawFrameLocation = glGetUniformLocation(m_program.id(), "u_draw_frame");
  m_debugLocation = glGetUniformLocation(m_program.id(), "u_debug");
  m_debugInputRectLocation = glGetUniformLocation(m_program.id(), "u_debug_input_rect");
  m_debugInputParamsLocation = glGetUniformLocation(m_program.id(), "u_debug_input_params");
  m_debugProgressLocation = glGetUniformLocation(m_program.id(), "u_debug_progress");
  m_debugInputEnabledLocation = glGetUniformLocation(m_program.id(), "u_debug_input_enabled");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_pixelScaleLocation < 0
      || m_transformLocation < 0
      || m_fillLocation < 0
      || m_shadowLocation < 0
      || m_apertureInsetsLocation < 0
      || m_roundingLocation < 0
      || m_smoothingLocation < 0
      || m_shadowRadiusLocation < 0
      || m_shadowOffsetLocation < 0
      || m_rectCountLocation < 0
      || m_rectsLocation < 0
      || m_rectParamsLocation < 0
      || m_drawFrameLocation < 0
      || m_debugLocation < 0
      || m_debugInputRectLocation < 0
      || m_debugInputParamsLocation < 0
      || m_debugProgressLocation < 0
      || m_debugInputEnabledLocation < 0) {
    destroy();
    throw std::runtime_error("failed to get chrome blob shader uniform locations");
  }
}

void ChromeBlobProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_pixelScaleLocation = -1;
  m_transformLocation = -1;
  m_fillLocation = -1;
  m_shadowLocation = -1;
  m_apertureInsetsLocation = -1;
  m_roundingLocation = -1;
  m_smoothingLocation = -1;
  m_shadowRadiusLocation = -1;
  m_shadowOffsetLocation = -1;
  m_rectCountLocation = -1;
  m_rectsLocation = -1;
  m_rectParamsLocation = -1;
  m_drawFrameLocation = -1;
  m_debugLocation = -1;
  m_debugInputRectLocation = -1;
  m_debugInputParamsLocation = -1;
  m_debugProgressLocation = -1;
  m_debugInputEnabledLocation = -1;
}

void ChromeBlobProgram::abandon() noexcept { m_program.abandon(); }

void ChromeBlobProgram::draw(
    float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
    const ChromeBlobStyle& style, const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f) {
    return;
  }

  static constexpr std::array<GLfloat, 12> kVertices = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };
  std::array<GLfloat, kMaxChromeBlobRects * 4> rectValues{};
  std::array<GLfloat, kMaxChromeBlobRects * 4> paramValues{};
  const std::size_t count = std::min<std::size_t>(style.rectCount, kMaxChromeBlobRects);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& rect = style.rects[i];
    rectValues[i * 4] = rect.x;
    rectValues[i * 4 + 1] = rect.y;
    rectValues[i * 4 + 2] = std::max(0.0f, rect.width);
    rectValues[i * 4 + 3] = std::max(0.0f, rect.height);
    paramValues[i * 4] = std::max(1.0f, rect.radius);
    paramValues[i * 4 + 1] = std::clamp(rect.deformation, -0.12f, 0.12f);
    paramValues[i * 4 + 2] = rect.unionSmoothing < 0.0f ? -1.0f : std::max(0.0f, rect.unionSmoothing);
    paramValues[i * 4 + 3] = static_cast<GLfloat>(rect.joinedEdges);
  }

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, width, height);
  glUniform2f(m_pixelScaleLocation, std::max(0.001f, pixelScaleX), std::max(0.001f, pixelScaleY));
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, transform.m.data());
  glUniform4f(m_fillLocation, style.fill.r, style.fill.g, style.fill.b, style.fill.a);
  glUniform4f(m_shadowLocation, style.shadow.r, style.shadow.g, style.shadow.b, style.shadow.a);
  glUniform4f(
      m_apertureInsetsLocation,
      std::max(0.0f, style.apertureInsets.left),
      std::max(0.0f, style.apertureInsets.top),
      std::max(0.0f, style.apertureInsets.right),
      std::max(0.0f, style.apertureInsets.bottom)
  );
  glUniform1f(m_roundingLocation, std::max(1.0f, style.rounding));
  glUniform1f(m_smoothingLocation, std::max(0.0f, style.smoothing));
  glUniform1f(m_shadowRadiusLocation, std::max(0.0f, style.shadowRadius));
  glUniform2f(m_shadowOffsetLocation, style.shadowOffsetX, style.shadowOffsetY);
  glUniform1i(m_rectCountLocation, static_cast<GLint>(count));
  glUniform4fv(m_rectsLocation, static_cast<GLsizei>(kMaxChromeBlobRects), rectValues.data());
  glUniform4fv(m_rectParamsLocation, static_cast<GLsizei>(kMaxChromeBlobRects), paramValues.data());
  glUniform1i(m_drawFrameLocation, style.drawFrame ? 1 : 0);
  glUniform1i(m_debugLocation, style.debug ? 1 : 0);
  glUniform4f(
      m_debugInputRectLocation, style.debugInputRect.x, style.debugInputRect.y, style.debugInputRect.width,
      style.debugInputRect.height
  );
  glUniform2f(m_debugInputParamsLocation, style.debugInputRect.radius, style.debugInputRect.deformation);
  glUniform1f(m_debugProgressLocation, std::clamp(style.debugProgress, 0.0f, 1.0f));
  glUniform1i(m_debugInputEnabledLocation, style.debugInputEnabled ? 1 : 0);

  const auto position = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, kVertices.data());
  glEnableVertexAttribArray(position);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(position);
}
