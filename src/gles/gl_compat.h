/* --------------------------------------------------------------------
EXTREME TUXRACER -- OpenGL ES 3 compatibility shim

Extreme Tux Racer targets a fixed-function OpenGL 1.2 pipeline, none of
which exists on GLES. Rather than rewrite the renderer, this shim
reimplements exactly the legacy subset the game actually uses, under the
original names, on top of GLES 3 plus a single uber-shader.

The game therefore compiles unchanged: bh.h includes this header in place
of <GL/gl.h> on Android, and every glBegin/glMatrixMode/glLightfv call site
keeps working.

What is emulated here:
  - the MODELVIEW/PROJECTION matrix stacks
  - glBegin/glEnd immediate mode
  - client-side vertex arrays (glVertexPointer and friends)
  - GL_QUADS / GL_QUAD_STRIP, which GLES does not have
  - fixed-function lighting, fog, alpha test, texgen and texture env

Anything already present in GLES 3 (glClear, glBlendFunc, glDepthMask,
glStencilOp, ...) is left alone and calls straight through.
---------------------------------------------------------------------*/

#ifndef ETR_GL_COMPAT_H
#define ETR_GL_COMPAT_H

#include <GLES3/gl3.h>

// GLES has no double-precision GL type, but the game's legacy call sites
// (glLoadMatrixd, glVertex3d, gluPerspective, ...) are written against it.
typedef double GLdouble;
typedef double GLclampd;

// --------------------------------------------------------------------
//	Enums that GLES 3 dropped along with the fixed-function pipeline
// --------------------------------------------------------------------

// Primitive types
#define GL_QUADS                  0x0007
#define GL_QUAD_STRIP             0x0008
#define GL_POLYGON                0x0009

// Matrix modes
#define GL_MODELVIEW              0x1700
#define GL_PROJECTION             0x1701

// Capabilities
#define GL_LIGHTING               0x0B50
#define GL_LIGHT0                 0x4000
#define GL_LIGHT1                 0x4001
#define GL_LIGHT2                 0x4002
#define GL_LIGHT3                 0x4003
#define GL_LIGHT4                 0x4004
#define GL_LIGHT5                 0x4005
#define GL_LIGHT6                 0x4006
#define GL_LIGHT7                 0x4007
#define GL_NORMALIZE              0x0BA1
#define GL_FOG                    0x0B60
#define GL_ALPHA_TEST             0x0BC0
#define GL_COLOR_MATERIAL         0x0B57
#define GL_TEXTURE_GEN_S          0x0C60
#define GL_TEXTURE_GEN_T          0x0C61

// Light / material parameters
#define GL_AMBIENT                0x1200
#define GL_DIFFUSE                0x1201
#define GL_SPECULAR               0x1202
#define GL_POSITION               0x1203
#define GL_SHININESS              0x1601
#define GL_AMBIENT_AND_DIFFUSE    0x1602
#define GL_EMISSION               0x1600

// Fog
#define GL_FOG_MODE               0x0B65
#define GL_FOG_DENSITY            0x0B62
#define GL_FOG_START              0x0B63
#define GL_FOG_END                0x0B64
#define GL_FOG_COLOR              0x0B66
#define GL_EXP                    0x0800
#define GL_EXP2                   0x0801

// Shade model
#define GL_FLAT                   0x1D00
#define GL_SMOOTH                 0x1D01

// Texture environment
#define GL_TEXTURE_ENV            0x2300
#define GL_TEXTURE_ENV_MODE       0x2200
#define GL_MODULATE               0x2100
#define GL_DECAL                  0x2101
#define GL_REPLACE                0x1E01

// Texgen
#define GL_TEXTURE_GEN_MODE       0x2500
#define GL_OBJECT_LINEAR          0x2401
#define GL_EYE_LINEAR             0x2400
#define GL_OBJECT_PLANE           0x2501
#define GL_EYE_PLANE              0x2502
#define GL_S                      0x2000
#define GL_T                      0x2001
#define GL_R                      0x2002
#define GL_Q                      0x2003

// Client array types
#define GL_VERTEX_ARRAY           0x8074
#define GL_NORMAL_ARRAY           0x8075
#define GL_COLOR_ARRAY            0x8076
#define GL_TEXTURE_COORD_ARRAY    0x8078

// glGetTexLevelParameteriv pnames
#define GL_TEXTURE_WIDTH          0x1000
#define GL_TEXTURE_HEIGHT         0x1001

// Hints
#define GL_FOG_HINT               0x0C54

// Misc legacy state queried by PrintGLInfo()
#define GL_DOUBLEBUFFER           0x0C32
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_POINT_SMOOTH_HINT      0x0C51
#define GL_LINE_SMOOTH_HINT       0x0C52
#define GL_STENCIL_BITS           0x0D57
#define GL_DEPTH_BITS             0x0D56
#define GL_RED_BITS               0x0D52
#define GL_GREEN_BITS             0x0D53
#define GL_BLUE_BITS              0x0D54
#define GL_ALPHA_BITS             0x0D55
#define GL_MODELVIEW_MATRIX       0x0BA6
#define GL_PROJECTION_MATRIX      0x0BA7
#define GL_MAX_LIGHTS                 0x0D31
#define GL_MAX_MODELVIEW_STACK_DEPTH  0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38

// GL_EXT_compiled_vertex_array does not exist on GLES. ogl.h declares
// function pointers of these types; they are simply never resolved, and
// quadtree.cpp already null-checks them before use.
typedef void (*PFNGLLOCKARRAYSEXTPROC)(GLint first, GLsizei count);
typedef void (*PFNGLUNLOCKARRAYSEXTPROC)(void);

// --------------------------------------------------------------------
//	Emulated entry points
// --------------------------------------------------------------------

extern "C" {

// Matrix stack
void etrgl_MatrixMode(GLenum mode);
void etrgl_LoadIdentity(void);
void etrgl_LoadMatrixd(const GLdouble* m);
void etrgl_LoadMatrixf(const GLfloat* m);
void etrgl_MultMatrixd(const GLdouble* m);
void etrgl_PushMatrix(void);
void etrgl_PopMatrix(void);
void etrgl_Translatef(GLfloat x, GLfloat y, GLfloat z);
void etrgl_Translated(GLdouble x, GLdouble y, GLdouble z);
void etrgl_Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void etrgl_Scalef(GLfloat x, GLfloat y, GLfloat z);
void etrgl_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                 GLdouble n, GLdouble f);

// Immediate mode
void etrgl_Begin(GLenum mode);
void etrgl_End(void);
void etrgl_Vertex2f(GLfloat x, GLfloat y);
void etrgl_Vertex3f(GLfloat x, GLfloat y, GLfloat z);
void etrgl_Vertex3d(GLdouble x, GLdouble y, GLdouble z);
void etrgl_Normal3f(GLfloat x, GLfloat y, GLfloat z);
void etrgl_Normal3d(GLdouble x, GLdouble y, GLdouble z);
void etrgl_Normal3i(GLint x, GLint y, GLint z);
void etrgl_TexCoord2f(GLfloat s, GLfloat t);
void etrgl_TexCoord2d(GLdouble s, GLdouble t);
void etrgl_Color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void etrgl_Color4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void etrgl_Color4ubv(const GLubyte* v);

// Client arrays
void etrgl_EnableClientState(GLenum array);
void etrgl_DisableClientState(GLenum array);
void etrgl_VertexPointer(GLint size, GLenum type, GLsizei stride,
                         const void* ptr);
void etrgl_NormalPointer(GLenum type, GLsizei stride, const void* ptr);
void etrgl_TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                           const void* ptr);
void etrgl_ColorPointer(GLint size, GLenum type, GLsizei stride,
                        const void* ptr);

// Fixed-function state
void etrgl_Enable(GLenum cap);
void etrgl_Disable(GLenum cap);
void etrgl_Lightfv(GLenum light, GLenum pname, const GLfloat* params);
void etrgl_Materialf(GLenum face, GLenum pname, GLfloat param);
void etrgl_Materialfv(GLenum face, GLenum pname, const GLfloat* params);
void etrgl_Materialiv(GLenum face, GLenum pname, const GLint* params);
void etrgl_Fogf(GLenum pname, GLfloat param);
void etrgl_Fogfv(GLenum pname, const GLfloat* params);
void etrgl_Fogi(GLenum pname, GLint param);
void etrgl_AlphaFunc(GLenum func, GLclampf ref);
void etrgl_ShadeModel(GLenum mode);
void etrgl_TexEnvf(GLenum target, GLenum pname, GLfloat param);
void etrgl_TexGeni(GLenum coord, GLenum pname, GLint param);
void etrgl_TexGenfv(GLenum coord, GLenum pname, const GLfloat* params);
void etrgl_Hint(GLenum target, GLenum mode);

// Intercepted GLES calls
void etrgl_DrawArrays(GLenum mode, GLint first, GLsizei count);
void etrgl_DrawElements(GLenum mode, GLsizei count, GLenum type,
                        const void* indices);
void etrgl_BindTexture(GLenum target, GLuint texture);
void etrgl_TexImage2D(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void* pixels);
void etrgl_GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname,
                                  GLint* params);

/** Registers the dimensions of a texture whose storage was created outside
 *  the shim (the platform layer's sf::Texture), so that
 *  glGetTexLevelParameteriv() still reports the right size for it. */
void etrgl_NoteTextureSize(GLuint texture, GLint w, GLint h);

// GLU replacements
void etrgl_Perspective(GLdouble fovy, GLdouble aspect, GLdouble zn,
                       GLdouble zf);
void etrgl_LookAt(GLdouble ex, GLdouble ey, GLdouble ez,
                  GLdouble cx, GLdouble cy, GLdouble cz,
                  GLdouble ux, GLdouble uy, GLdouble uz);
const GLubyte* etrgl_ErrorString(GLenum err);

// GLU quadrics -- only what tux.cpp needs to draw the penguin's body parts.
typedef struct ETRQuadric ETRQuadric;
ETRQuadric* etrglu_NewQuadric(void);
void etrglu_DeleteQuadric(ETRQuadric* q);
void etrglu_QuadricDrawStyle(ETRQuadric* q, GLenum style);
void etrglu_QuadricNormals(ETRQuadric* q, GLenum normals);
void etrglu_QuadricOrientation(ETRQuadric* q, GLenum orientation);
void etrglu_Sphere(ETRQuadric* q, GLdouble radius, GLint slices,
                   GLint stacks);

#define GLU_FILL                  100012
#define GLU_SMOOTH                100000
#define GLU_OUTSIDE               100020
#define GLU_INSIDE                100021

// --------------------------------------------------------------------
//	Lifecycle -- called by the platform layer, not by game code
// --------------------------------------------------------------------

/** Compiles the uber-shader and allocates scratch buffers. Requires a
 *  current GLES 3 context. */
bool etrgl_Init(void);
void etrgl_Shutdown(void);

/** Drops cached uniform/attribute state so the next draw re-uploads
 *  everything. Call after any code outside the shim has touched GL. */
void etrgl_InvalidateState(void);

}  // extern "C"

// --------------------------------------------------------------------
//	Redirect the legacy names onto the shim
// --------------------------------------------------------------------
// gl_compat.cpp defines ETR_GL_COMPAT_IMPL so that it can still reach the
// genuine GLES entry points it forwards to.

#ifndef ETR_GL_COMPAT_IMPL

#define glMatrixMode            etrgl_MatrixMode
#define glLoadIdentity          etrgl_LoadIdentity
#define glLoadMatrixd           etrgl_LoadMatrixd
#define glLoadMatrixf           etrgl_LoadMatrixf
#define glMultMatrixd           etrgl_MultMatrixd
#define glPushMatrix            etrgl_PushMatrix
#define glPopMatrix             etrgl_PopMatrix
#define glTranslatef            etrgl_Translatef
#define glTranslated            etrgl_Translated
#define glRotatef               etrgl_Rotatef
#define glScalef                etrgl_Scalef
#define glOrtho                 etrgl_Ortho

#define glBegin                 etrgl_Begin
#define glEnd                   etrgl_End
#define glVertex2f              etrgl_Vertex2f
#define glVertex3f              etrgl_Vertex3f
#define glVertex3d              etrgl_Vertex3d
#define glNormal3f              etrgl_Normal3f
#define glNormal3d              etrgl_Normal3d
#define glNormal3i              etrgl_Normal3i
#define glTexCoord2f            etrgl_TexCoord2f
#define glTexCoord2d            etrgl_TexCoord2d
#define glColor4f               etrgl_Color4f
#define glColor4ub              etrgl_Color4ub
#define glColor4ubv             etrgl_Color4ubv

#define glEnableClientState     etrgl_EnableClientState
#define glDisableClientState    etrgl_DisableClientState
#define glVertexPointer         etrgl_VertexPointer
#define glNormalPointer         etrgl_NormalPointer
#define glTexCoordPointer       etrgl_TexCoordPointer
#define glColorPointer          etrgl_ColorPointer

#define glEnable                etrgl_Enable
#define glDisable               etrgl_Disable
#define glLightfv               etrgl_Lightfv
#define glMaterialf             etrgl_Materialf
#define glMaterialfv            etrgl_Materialfv
#define glMaterialiv            etrgl_Materialiv
#define glFogf                  etrgl_Fogf
#define glFogfv                 etrgl_Fogfv
#define glFogi                  etrgl_Fogi
#define glAlphaFunc             etrgl_AlphaFunc
#define glShadeModel            etrgl_ShadeModel
#define glTexEnvf               etrgl_TexEnvf
#define glTexGeni               etrgl_TexGeni
#define glTexGenfv              etrgl_TexGenfv
#define glHint                  etrgl_Hint

#define glDrawArrays            etrgl_DrawArrays
#define glDrawElements          etrgl_DrawElements
#define glBindTexture           etrgl_BindTexture
#define glTexImage2D            etrgl_TexImage2D
#define glGetTexLevelParameteriv etrgl_GetTexLevelParameteriv

#define gluPerspective          etrgl_Perspective
#define gluLookAt               etrgl_LookAt
#define gluErrorString          etrgl_ErrorString

#define GLUquadricObj           ETRQuadric
#define GLUquadric              ETRQuadric
#define gluNewQuadric           etrglu_NewQuadric
#define gluDeleteQuadric        etrglu_DeleteQuadric
#define gluQuadricDrawStyle     etrglu_QuadricDrawStyle
#define gluQuadricNormals       etrglu_QuadricNormals
#define gluQuadricOrientation   etrglu_QuadricOrientation
#define gluSphere               etrglu_Sphere

#endif  // ETR_GL_COMPAT_IMPL

#endif  // ETR_GL_COMPAT_H
