#pragma once

#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

struct ChromeBlobStyle;

// Draws the screen frame, rail and panel backgrounds as one signed-distance
// field. The shader owns the smooth union and its global shadow, so seams and
// z-order cannot diverge between individual rounded-rect nodes.
class ChromeBlobProgram {
public:
  void ensureInitialized();
  void destroy();
  void abandon() noexcept;

  void draw(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const ChromeBlobStyle& style, const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_quadSizeLocation = -1;
  GLint m_pixelScaleLocation = -1;
  GLint m_transformLocation = -1;
  GLint m_fillLocation = -1;
  GLint m_shadowLocation = -1;
  GLint m_apertureInsetsLocation = -1;
  GLint m_roundingLocation = -1;
  GLint m_smoothingLocation = -1;
  GLint m_shadowRadiusLocation = -1;
  GLint m_shadowOffsetLocation = -1;
  GLint m_rectCountLocation = -1;
  GLint m_rectsLocation = -1;
  GLint m_rectParamsLocation = -1;
  GLint m_drawFrameLocation = -1;
  GLint m_debugLocation = -1;
  GLint m_debugInputRectLocation = -1;
  GLint m_debugInputParamsLocation = -1;
  GLint m_debugProgressLocation = -1;
  GLint m_debugInputEnabledLocation = -1;
};
