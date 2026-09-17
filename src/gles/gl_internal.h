/* --------------------------------------------------------------------
EXTREME TUXRACER -- GLES shim internals

Shared between gl_compat.cpp (the state machine) and gl_shaders.cpp (the
uber-shader). Not included by game code.
---------------------------------------------------------------------*/

#ifndef ETR_GL_INTERNAL_H
#define ETR_GL_INTERNAL_H

#include <GLES3/gl3.h>

// Fixed attribute slots, matching the layout() qualifiers in the vertex
// shader. Arrays that the game leaves disabled are supplied as constant
// vertex attributes instead, which is why the shader needs no "is this
// array enabled" uniforms.
enum {
	ETR_ATTRIB_POS    = 0,
	ETR_ATTRIB_NORMAL = 1,
	ETR_ATTRIB_TEX    = 2,
	ETR_ATTRIB_COLOR  = 3
};

// Texture environment modes, matching uTexEnvMode in the fragment shader.
enum {
	ETR_TEXENV_MODULATE = 0,
	ETR_TEXENV_DECAL    = 1,
	ETR_TEXENV_REPLACE  = 2
};

// Fog modes, matching uFogMode in the fragment shader.
enum {
	ETR_FOG_LINEAR = 0,
	ETR_FOG_EXP    = 1,
	ETR_FOG_EXP2   = 2
};

struct EtrShader {
	GLuint program = 0;

	GLint uModelView    = -1;
	GLint uProjection   = -1;
	GLint uNormalMatrix = -1;

	GLint uUseLighting   = -1;
	GLint uLightPosEye   = -1;
	GLint uLightAmbient  = -1;
	GLint uLightDiffuse  = -1;
	GLint uLightSpecular = -1;
	GLint uMatAmbDiff    = -1;
	GLint uMatSpecular   = -1;
	GLint uMatShininess  = -1;

	GLint uUseTexGen = -1;
	GLint uSPlane    = -1;
	GLint uTPlane    = -1;

	GLint uUseTexture = -1;
	GLint uTexEnvMode = -1;
	GLint uTex        = -1;

	GLint uUseFog     = -1;
	GLint uFogMode    = -1;
	GLint uFogColor   = -1;
	GLint uFogStart   = -1;
	GLint uFogEnd     = -1;
	GLint uFogDensity = -1;

	GLint uUseAlphaTest = -1;
	GLint uAlphaRef     = -1;
};

/** Compiles and links the uber-shader and resolves every uniform location.
 *  Returns false and logs on failure. */
bool etrglInternal_BuildShader(EtrShader& out);

#endif  // ETR_GL_INTERNAL_H
