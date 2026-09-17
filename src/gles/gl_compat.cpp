/* --------------------------------------------------------------------
EXTREME TUXRACER -- OpenGL ES 3 compatibility shim, implementation

See gl_compat.h for the rationale. This file owns the emulated fixed-
function state and translates it into uniform uploads plus generic vertex
attribute setup at draw time.

Two details worth knowing when reading it:

  * Matrices are stored column-major as 16 floats, exactly the layout GL
    expects, so glLoadMatrixd() is a straight widen-and-copy.

  * Vertex arrays the game leaves disabled are supplied as *constant*
    vertex attributes (glVertexAttrib4f). That is why the shader has no
    "is the colour array enabled" uniform: a disabled colour array simply
    becomes the current glColor value for every vertex.
---------------------------------------------------------------------*/

#define ETR_GL_COMPAT_IMPL
#include "gl_compat.h"
#include "gl_internal.h"

#include <android/log.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRGL", __VA_ARGS__)

namespace {

// --------------------------------------------------------------------
//	Minimal column-major 4x4 matrix maths
// --------------------------------------------------------------------

struct Mat4 {
	float m[16];
};

Mat4 identity() {
	Mat4 r{};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
	return r;
}

// r = a * b, with the same operand order as glMultMatrix (post-multiply).
Mat4 multiply(const Mat4& a, const Mat4& b) {
	Mat4 r{};
	for (int c = 0; c < 4; c++) {
		for (int row = 0; row < 4; row++) {
			float s = 0.f;
			for (int k = 0; k < 4; k++)
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	}
	return r;
}

// Upper-left 3x3, inverted and transposed, for transforming normals.
void normalMatrix(const Mat4& mv, float* out9) {
	const float* m = mv.m;
	const float a = m[0], b = m[4], c = m[8];
	const float d = m[1], e = m[5], f = m[9];
	const float g = m[2], h = m[6], i = m[10];

	const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::fabs(det) < 1e-12f) {
		// Degenerate modelview: fall back to the plain 3x3 so lighting
		// stays defined rather than producing NaNs.
		out9[0] = a; out9[1] = d; out9[2] = g;
		out9[3] = b; out9[4] = e; out9[5] = h;
		out9[6] = c; out9[7] = f; out9[8] = i;
		return;
	}
	const float id = 1.f / det;

	// inverse(M)^T, written out directly in column-major order.
	out9[0] = (e * i - f * h) * id;
	out9[1] = (c * h - b * i) * id;
	out9[2] = (b * f - c * e) * id;
	out9[3] = (f * g - d * i) * id;
	out9[4] = (a * i - c * g) * id;
	out9[5] = (c * d - a * f) * id;
	out9[6] = (d * h - e * g) * id;
	out9[7] = (b * g - a * h) * id;
	out9[8] = (a * e - b * d) * id;
}

// --------------------------------------------------------------------
//	Emulated state
// --------------------------------------------------------------------

struct ArrayState {
	bool enabled = false;
	GLint size = 0;
	GLenum type = GL_FLOAT;
	GLsizei stride = 0;
	const void* ptr = nullptr;
};

struct Vertex {
	float pos[3];
	float normal[3];
	float tex[2];
	float color[4];
};

struct State {
	bool initialised = false;
	EtrShader shader;

	// Matrix stacks
	GLenum matrix_mode = GL_MODELVIEW;
	std::vector<Mat4> modelview{identity()};
	std::vector<Mat4> projection{identity()};

	Mat4& current() {
		return matrix_mode == GL_PROJECTION ? projection.back()
		                                    : modelview.back();
	}

	// Immediate mode
	bool in_begin = false;
	GLenum begin_mode = GL_TRIANGLES;
	std::vector<Vertex> batch;
	float cur_normal[3] = {0.f, 0.f, 1.f};
	float cur_tex[2]    = {0.f, 0.f};
	float cur_color[4]  = {1.f, 1.f, 1.f, 1.f};

	// Client arrays
	ArrayState vertex_array, normal_array, texcoord_array, color_array;

	// Capabilities
	bool texture_2d  = false;
	bool lighting    = false;
	bool fog         = false;
	bool alpha_test  = false;
	bool texgen_s    = false;
	bool texgen_t    = false;

	// Lighting
	float light_pos_eye[4] = {0.f, 0.f, 1.f, 0.f};
	float light_ambient[4]  = {0.f, 0.f, 0.f, 1.f};
	float light_diffuse[4]  = {1.f, 1.f, 1.f, 1.f};
	float light_specular[4] = {1.f, 1.f, 1.f, 1.f};

	// Material
	float mat_amb_diff[4] = {0.8f, 0.8f, 0.8f, 1.f};
	float mat_specular[4] = {0.f, 0.f, 0.f, 1.f};
	float mat_shininess   = 0.f;

	// Fog
	int   fog_mode     = ETR_FOG_LINEAR;
	float fog_color[4] = {0.f, 0.f, 0.f, 1.f};
	float fog_start    = 0.f;
	float fog_end      = 1.f;
	float fog_density  = 1.f;

	// Texture environment / texgen
	int   texenv_mode = ETR_TEXENV_MODULATE;
	float s_plane[4]  = {1.f, 0.f, 0.f, 0.f};
	float t_plane[4]  = {0.f, 1.f, 0.f, 0.f};

	// Alpha test
	float alpha_ref = 0.f;

	// Texture dimension bookkeeping, since GLES has no
	// glGetTexLevelParameteriv.
	GLuint bound_texture = 0;
	std::unordered_map<GLuint, std::pair<GLint, GLint>> texture_size;

	// Scratch index buffer used to turn quads into triangles.
	std::vector<GLuint> quad_indices;
};

State g;

// --------------------------------------------------------------------
//	Draw-time translation
// --------------------------------------------------------------------

// GLES has no GL_QUADS. Builds (and caches) indices expanding N quads into
// 2N triangles: 0,1,2  0,2,3  4,5,6  4,6,7 ...
const GLuint* quadIndices(GLsizei vertex_count, GLsizei& out_index_count) {
	const GLsizei quads = vertex_count / 4;
	out_index_count = quads * 6;

	if (static_cast<GLsizei>(g.quad_indices.size()) < out_index_count) {
		g.quad_indices.resize(out_index_count);
		for (GLsizei q = 0; q < quads; q++) {
			const GLuint base = q * 4;
			GLuint* d = &g.quad_indices[q * 6];
			d[0] = base;     d[1] = base + 1; d[2] = base + 2;
			d[3] = base;     d[4] = base + 2; d[5] = base + 3;
		}
	}
	return g.quad_indices.data();
}

void bindAttrib(int index, const ArrayState& a, const float* constant,
                int constant_size) {
	if (a.enabled && a.ptr) {
		glEnableVertexAttribArray(index);
		// GL_FIXED and integer formats are all normalised the same way the
		// fixed-function pipeline did: positions and coordinates convert
		// straight to float, colours are normalised bytes.
		const GLboolean normalise =
		    (index == ETR_ATTRIB_COLOR && a.type == GL_UNSIGNED_BYTE)
		        ? GL_TRUE : GL_FALSE;
		glVertexAttribPointer(index, a.size, a.type, normalise, a.stride, a.ptr);
	} else {
		glDisableVertexAttribArray(index);
		switch (constant_size) {
			case 2: glVertexAttrib2fv(index, constant); break;
			case 3: glVertexAttrib3fv(index, constant); break;
			default: glVertexAttrib4fv(index, constant); break;
		}
	}
}

// Pushes the whole emulated pipeline state into the shader and wires up the
// vertex attributes. Called immediately before every draw.
void applyState() {
	const EtrShader& s = g.shader;
	glUseProgram(s.program);

	glUniformMatrix4fv(s.uModelView, 1, GL_FALSE, g.modelview.back().m);
	glUniformMatrix4fv(s.uProjection, 1, GL_FALSE, g.projection.back().m);

	float nm[9];
	normalMatrix(g.modelview.back(), nm);
	glUniformMatrix3fv(s.uNormalMatrix, 1, GL_FALSE, nm);

	glUniform1i(s.uUseLighting, g.lighting ? 1 : 0);
	if (g.lighting) {
		glUniform4fv(s.uLightPosEye, 1, g.light_pos_eye);
		glUniform4fv(s.uLightAmbient, 1, g.light_ambient);
		glUniform4fv(s.uLightDiffuse, 1, g.light_diffuse);
		glUniform4fv(s.uLightSpecular, 1, g.light_specular);
		glUniform4fv(s.uMatAmbDiff, 1, g.mat_amb_diff);
		glUniform4fv(s.uMatSpecular, 1, g.mat_specular);
		glUniform1f(s.uMatShininess, g.mat_shininess);
	}

	// Texgen is only meaningful with S and T both driven; the game always
	// sets them as a pair.
	const bool texgen = g.texgen_s && g.texgen_t;
	glUniform1i(s.uUseTexGen, texgen ? 1 : 0);
	if (texgen) {
		glUniform4fv(s.uSPlane, 1, g.s_plane);
		glUniform4fv(s.uTPlane, 1, g.t_plane);
	}

	glUniform1i(s.uUseTexture, g.texture_2d ? 1 : 0);
	glUniform1i(s.uTexEnvMode, g.texenv_mode);

	glUniform1i(s.uUseFog, g.fog ? 1 : 0);
	if (g.fog) {
		glUniform1i(s.uFogMode, g.fog_mode);
		glUniform4fv(s.uFogColor, 1, g.fog_color);
		glUniform1f(s.uFogStart, g.fog_start);
		glUniform1f(s.uFogEnd, g.fog_end);
		glUniform1f(s.uFogDensity, g.fog_density);
	}

	glUniform1i(s.uUseAlphaTest, g.alpha_test ? 1 : 0);
	glUniform1f(s.uAlphaRef, g.alpha_ref);

	bindAttrib(ETR_ATTRIB_POS,    g.vertex_array,   nullptr,       4);
	bindAttrib(ETR_ATTRIB_NORMAL, g.normal_array,   g.cur_normal,  3);
	bindAttrib(ETR_ATTRIB_TEX,    g.texcoord_array, g.cur_tex,     2);
	bindAttrib(ETR_ATTRIB_COLOR,  g.color_array,    g.cur_color,   4);
}

// Maps a legacy primitive onto its GLES equivalent. GL_QUAD_STRIP is
// vertex-for-vertex identical to GL_TRIANGLE_STRIP, and GL_POLYGON is a
// fan for the convex polygons the game draws.
GLenum translatePrimitive(GLenum mode, bool& needs_quad_indices) {
	needs_quad_indices = false;
	switch (mode) {
		case GL_QUADS:
			needs_quad_indices = true;
			return GL_TRIANGLES;
		case GL_QUAD_STRIP:
			return GL_TRIANGLE_STRIP;
		case GL_POLYGON:
			return GL_TRIANGLE_FAN;
		default:
			return mode;
	}
}

}  // namespace

// --------------------------------------------------------------------
//	Lifecycle
// --------------------------------------------------------------------

extern "C" bool etrgl_Init(void) {
	if (g.initialised) return true;
	if (!etrglInternal_BuildShader(g.shader)) {
		LOGE("failed to build the compatibility shader");
		return false;
	}
	g.modelview.assign(1, identity());
	g.projection.assign(1, identity());
	g.initialised = true;
	return true;
}

extern "C" void etrgl_Shutdown(void) {
	if (g.shader.program) glDeleteProgram(g.shader.program);
	g.shader = EtrShader();
	g.texture_size.clear();
	g.initialised = false;
}

extern "C" void etrgl_InvalidateState(void) {
	// Every uniform is re-uploaded on each draw anyway; all that has to be
	// undone here is attribute enablement, which other code may have
	// changed behind our back.
	for (int i = 0; i < 4; i++) glDisableVertexAttribArray(i);
}

// --------------------------------------------------------------------
//	Matrix stack
// --------------------------------------------------------------------

extern "C" void etrgl_MatrixMode(GLenum mode) { g.matrix_mode = mode; }

extern "C" void etrgl_LoadIdentity(void) { g.current() = identity(); }

extern "C" void etrgl_LoadMatrixd(const GLdouble* m) {
	Mat4& c = g.current();
	for (int i = 0; i < 16; i++) c.m[i] = static_cast<float>(m[i]);
}

extern "C" void etrgl_LoadMatrixf(const GLfloat* m) {
	std::memcpy(g.current().m, m, sizeof(float) * 16);
}

extern "C" void etrgl_MultMatrixd(const GLdouble* m) {
	Mat4 rhs;
	for (int i = 0; i < 16; i++) rhs.m[i] = static_cast<float>(m[i]);
	Mat4& c = g.current();
	c = multiply(c, rhs);
}

extern "C" void etrgl_PushMatrix(void) {
	if (g.matrix_mode == GL_PROJECTION)
		g.projection.push_back(g.projection.back());
	else
		g.modelview.push_back(g.modelview.back());
}

extern "C" void etrgl_PopMatrix(void) {
	std::vector<Mat4>& stack =
	    (g.matrix_mode == GL_PROJECTION) ? g.projection : g.modelview;
	if (stack.size() > 1) stack.pop_back();
}

extern "C" void etrgl_Translatef(GLfloat x, GLfloat y, GLfloat z) {
	Mat4 t = identity();
	t.m[12] = x; t.m[13] = y; t.m[14] = z;
	Mat4& c = g.current();
	c = multiply(c, t);
}

extern "C" void etrgl_Translated(GLdouble x, GLdouble y, GLdouble z) {
	etrgl_Translatef(static_cast<float>(x), static_cast<float>(y),
	                 static_cast<float>(z));
}

extern "C" void etrgl_Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
	const float len = std::sqrt(x * x + y * y + z * z);
	if (len < 1e-8f) return;
	x /= len; y /= len; z /= len;

	const float rad = angle * 3.14159265358979323846f / 180.f;
	const float c_ = std::cos(rad), s_ = std::sin(rad), t = 1.f - c_;

	Mat4 r = identity();
	r.m[0]  = t * x * x + c_;
	r.m[1]  = t * x * y + s_ * z;
	r.m[2]  = t * x * z - s_ * y;
	r.m[4]  = t * x * y - s_ * z;
	r.m[5]  = t * y * y + c_;
	r.m[6]  = t * y * z + s_ * x;
	r.m[8]  = t * x * z + s_ * y;
	r.m[9]  = t * y * z - s_ * x;
	r.m[10] = t * z * z + c_;

	Mat4& cur = g.current();
	cur = multiply(cur, r);
}

extern "C" void etrgl_Scalef(GLfloat x, GLfloat y, GLfloat z) {
	Mat4 s = identity();
	s.m[0] = x; s.m[5] = y; s.m[10] = z;
	Mat4& c = g.current();
	c = multiply(c, s);
}

extern "C" void etrgl_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                            GLdouble n, GLdouble f) {
	Mat4 o = identity();
	o.m[0]  = static_cast<float>(2.0 / (r - l));
	o.m[5]  = static_cast<float>(2.0 / (t - b));
	o.m[10] = static_cast<float>(-2.0 / (f - n));
	o.m[12] = static_cast<float>(-(r + l) / (r - l));
	o.m[13] = static_cast<float>(-(t + b) / (t - b));
	o.m[14] = static_cast<float>(-(f + n) / (f - n));

	Mat4& c = g.current();
	c = multiply(c, o);
}

// --------------------------------------------------------------------
//	Immediate mode
// --------------------------------------------------------------------

extern "C" void etrgl_Begin(GLenum mode) {
	g.in_begin = true;
	g.begin_mode = mode;
	g.batch.clear();
}

extern "C" void etrgl_End(void) {
	g.in_begin = false;
	if (g.batch.empty()) return;

	// Temporarily point the client arrays at the batch we just built, draw,
	// then restore whatever the game had configured.
	const ArrayState saved_v = g.vertex_array;
	const ArrayState saved_n = g.normal_array;
	const ArrayState saved_t = g.texcoord_array;
	const ArrayState saved_c = g.color_array;

	const Vertex* base = g.batch.data();
	const GLsizei stride = sizeof(Vertex);

	g.vertex_array   = {true, 3, GL_FLOAT, stride, &base->pos};
	g.normal_array   = {true, 3, GL_FLOAT, stride, &base->normal};
	g.texcoord_array = {true, 2, GL_FLOAT, stride, &base->tex};
	g.color_array    = {true, 4, GL_FLOAT, stride, &base->color};

	applyState();

	bool needs_quads = false;
	const GLenum prim = translatePrimitive(g.begin_mode, needs_quads);
	const GLsizei count = static_cast<GLsizei>(g.batch.size());

	if (needs_quads) {
		GLsizei index_count = 0;
		const GLuint* idx = quadIndices(count, index_count);
		glDrawElements(prim, index_count, GL_UNSIGNED_INT, idx);
	} else {
		glDrawArrays(prim, 0, count);
	}

	g.vertex_array   = saved_v;
	g.normal_array   = saved_n;
	g.texcoord_array = saved_t;
	g.color_array    = saved_c;
}

extern "C" void etrgl_Vertex3f(GLfloat x, GLfloat y, GLfloat z) {
	Vertex v;
	v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
	std::memcpy(v.normal, g.cur_normal, sizeof(v.normal));
	std::memcpy(v.tex, g.cur_tex, sizeof(v.tex));
	std::memcpy(v.color, g.cur_color, sizeof(v.color));
	g.batch.push_back(v);
}

extern "C" void etrgl_Vertex2f(GLfloat x, GLfloat y) {
	etrgl_Vertex3f(x, y, 0.f);
}

extern "C" void etrgl_Vertex3d(GLdouble x, GLdouble y, GLdouble z) {
	etrgl_Vertex3f(static_cast<float>(x), static_cast<float>(y),
	               static_cast<float>(z));
}

extern "C" void etrgl_Normal3f(GLfloat x, GLfloat y, GLfloat z) {
	g.cur_normal[0] = x; g.cur_normal[1] = y; g.cur_normal[2] = z;
}

extern "C" void etrgl_Normal3d(GLdouble x, GLdouble y, GLdouble z) {
	etrgl_Normal3f(static_cast<float>(x), static_cast<float>(y),
	               static_cast<float>(z));
}

extern "C" void etrgl_Normal3i(GLint x, GLint y, GLint z) {
	etrgl_Normal3f(static_cast<float>(x), static_cast<float>(y),
	               static_cast<float>(z));
}

extern "C" void etrgl_TexCoord2f(GLfloat s, GLfloat t) {
	g.cur_tex[0] = s; g.cur_tex[1] = t;
}

extern "C" void etrgl_TexCoord2d(GLdouble s, GLdouble t) {
	etrgl_TexCoord2f(static_cast<float>(s), static_cast<float>(t));
}

extern "C" void etrgl_Color4f(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) {
	g.cur_color[0] = r; g.cur_color[1] = gg;
	g.cur_color[2] = b; g.cur_color[3] = a;
}

extern "C" void etrgl_Color4ub(GLubyte r, GLubyte gg, GLubyte b, GLubyte a) {
	etrgl_Color4f(r / 255.f, gg / 255.f, b / 255.f, a / 255.f);
}

extern "C" void etrgl_Color4ubv(const GLubyte* v) {
	etrgl_Color4ub(v[0], v[1], v[2], v[3]);
}

// --------------------------------------------------------------------
//	Client arrays
// --------------------------------------------------------------------

namespace {
ArrayState* arrayFor(GLenum array) {
	switch (array) {
		case GL_VERTEX_ARRAY:        return &g.vertex_array;
		case GL_NORMAL_ARRAY:        return &g.normal_array;
		case GL_TEXTURE_COORD_ARRAY: return &g.texcoord_array;
		case GL_COLOR_ARRAY:         return &g.color_array;
		default:                     return nullptr;
	}
}
}  // namespace

extern "C" void etrgl_EnableClientState(GLenum array) {
	if (ArrayState* a = arrayFor(array)) a->enabled = true;
}

extern "C" void etrgl_DisableClientState(GLenum array) {
	if (ArrayState* a = arrayFor(array)) a->enabled = false;
}

extern "C" void etrgl_VertexPointer(GLint size, GLenum type, GLsizei stride,
                                    const void* ptr) {
	g.vertex_array.size = size;
	g.vertex_array.type = type;
	g.vertex_array.stride = stride;
	g.vertex_array.ptr = ptr;
}

extern "C" void etrgl_NormalPointer(GLenum type, GLsizei stride,
                                    const void* ptr) {
	g.normal_array.size = 3;
	g.normal_array.type = type;
	g.normal_array.stride = stride;
	g.normal_array.ptr = ptr;
}

extern "C" void etrgl_TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                                      const void* ptr) {
	g.texcoord_array.size = size;
	g.texcoord_array.type = type;
	g.texcoord_array.stride = stride;
	g.texcoord_array.ptr = ptr;
}

extern "C" void etrgl_ColorPointer(GLint size, GLenum type, GLsizei stride,
                                   const void* ptr) {
	g.color_array.size = size;
	g.color_array.type = type;
	g.color_array.stride = stride;
	g.color_array.ptr = ptr;
}

// --------------------------------------------------------------------
//	Fixed-function state
// --------------------------------------------------------------------

extern "C" void etrgl_Enable(GLenum cap) {
	switch (cap) {
		case GL_TEXTURE_2D:      g.texture_2d = true; return;
		case GL_LIGHTING:        g.lighting   = true; return;
		case GL_FOG:             g.fog        = true; return;
		case GL_ALPHA_TEST:      g.alpha_test = true; return;
		case GL_TEXTURE_GEN_S:   g.texgen_s   = true; return;
		case GL_TEXTURE_GEN_T:   g.texgen_t   = true; return;
		// Emulated implicitly or simply irrelevant on GLES.
		case GL_LIGHT0: case GL_LIGHT1: case GL_LIGHT2: case GL_LIGHT3:
		case GL_LIGHT4: case GL_LIGHT5: case GL_LIGHT6: case GL_LIGHT7:
		case GL_NORMALIZE:
		case GL_COLOR_MATERIAL:
			return;
		default:
			glEnable(cap);
			return;
	}
}

extern "C" void etrgl_Disable(GLenum cap) {
	switch (cap) {
		case GL_TEXTURE_2D:      g.texture_2d = false; return;
		case GL_LIGHTING:        g.lighting   = false; return;
		case GL_FOG:             g.fog        = false; return;
		case GL_ALPHA_TEST:      g.alpha_test = false; return;
		case GL_TEXTURE_GEN_S:   g.texgen_s   = false; return;
		case GL_TEXTURE_GEN_T:   g.texgen_t   = false; return;
		case GL_LIGHT0: case GL_LIGHT1: case GL_LIGHT2: case GL_LIGHT3:
		case GL_LIGHT4: case GL_LIGHT5: case GL_LIGHT6: case GL_LIGHT7:
		case GL_NORMALIZE:
		case GL_COLOR_MATERIAL:
			return;
		default:
			glDisable(cap);
			return;
	}
}

extern "C" void etrgl_Lightfv(GLenum /*light*/, GLenum pname,
                              const GLfloat* params) {
	switch (pname) {
		case GL_POSITION: {
			// GL transforms the light position by the modelview matrix in
			// force at the time of the call, so do the same here and keep
			// the result in eye space.
			const Mat4& mv = g.modelview.back();
			for (int i = 0; i < 4; i++) {
				g.light_pos_eye[i] = mv.m[i] * params[0]
				                   + mv.m[4 + i] * params[1]
				                   + mv.m[8 + i] * params[2]
				                   + mv.m[12 + i] * params[3];
			}
			break;
		}
		case GL_AMBIENT:  std::memcpy(g.light_ambient, params, 16); break;
		case GL_DIFFUSE:  std::memcpy(g.light_diffuse, params, 16); break;
		case GL_SPECULAR: std::memcpy(g.light_specular, params, 16); break;
		default: break;
	}
}

extern "C" void etrgl_Materialf(GLenum /*face*/, GLenum pname, GLfloat param) {
	if (pname == GL_SHININESS) g.mat_shininess = param;
}

extern "C" void etrgl_Materialfv(GLenum /*face*/, GLenum pname,
                                 const GLfloat* params) {
	switch (pname) {
		case GL_AMBIENT_AND_DIFFUSE:
		case GL_DIFFUSE:
			std::memcpy(g.mat_amb_diff, params, 16);
			break;
		case GL_SPECULAR:
			std::memcpy(g.mat_specular, params, 16);
			break;
		case GL_SHININESS:
			g.mat_shininess = params[0];
			break;
		default:
			break;
	}
}

extern "C" void etrgl_Materialiv(GLenum face, GLenum pname,
                                 const GLint* params) {
	// GL maps integer colour components from the full int range onto
	// [0,1]; ogl.cpp builds them as component * (INT_MAX / 255).
	GLfloat f[4];
	for (int i = 0; i < 4; i++)
		f[i] = static_cast<float>(params[i]) / 2147483647.f;
	etrgl_Materialfv(face, pname, f);
}

extern "C" void etrgl_Fogf(GLenum pname, GLfloat param) {
	switch (pname) {
		case GL_FOG_START:   g.fog_start = param; break;
		case GL_FOG_END:     g.fog_end = param; break;
		case GL_FOG_DENSITY: g.fog_density = param; break;
		case GL_FOG_MODE:    etrgl_Fogi(pname, static_cast<GLint>(param)); break;
		default: break;
	}
}

extern "C" void etrgl_Fogfv(GLenum pname, const GLfloat* params) {
	if (pname == GL_FOG_COLOR)
		std::memcpy(g.fog_color, params, 16);
	else
		etrgl_Fogf(pname, params[0]);
}

extern "C" void etrgl_Fogi(GLenum pname, GLint param) {
	if (pname != GL_FOG_MODE) {
		etrgl_Fogf(pname, static_cast<GLfloat>(param));
		return;
	}
	switch (param) {
		case GL_EXP:  g.fog_mode = ETR_FOG_EXP;  break;
		case GL_EXP2: g.fog_mode = ETR_FOG_EXP2; break;
		default:      g.fog_mode = ETR_FOG_LINEAR; break;
	}
}

extern "C" void etrgl_AlphaFunc(GLenum /*func*/, GLclampf ref) {
	// Only GL_GEQUAL is ever used, so the reference value is all we need.
	g.alpha_ref = ref;
}

extern "C" void etrgl_ShadeModel(GLenum /*mode*/) {
	// The shader is always smooth-shaded; the game only ever asks for
	// GL_SMOOTH anyway.
}

extern "C" void etrgl_TexEnvf(GLenum /*target*/, GLenum pname, GLfloat param) {
	if (pname != GL_TEXTURE_ENV_MODE) return;
	switch (static_cast<GLenum>(param)) {
		case GL_DECAL:   g.texenv_mode = ETR_TEXENV_DECAL;   break;
		case GL_REPLACE: g.texenv_mode = ETR_TEXENV_REPLACE; break;
		default:         g.texenv_mode = ETR_TEXENV_MODULATE; break;
	}
}

extern "C" void etrgl_TexGeni(GLenum /*coord*/, GLenum /*pname*/,
                              GLint /*param*/) {
	// Only GL_OBJECT_LINEAR is used, which is what the shader implements.
}

extern "C" void etrgl_TexGenfv(GLenum coord, GLenum pname,
                               const GLfloat* params) {
	if (pname != GL_OBJECT_PLANE) return;
	if (coord == GL_S) std::memcpy(g.s_plane, params, 16);
	else if (coord == GL_T) std::memcpy(g.t_plane, params, 16);
}

extern "C" void etrgl_Hint(GLenum target, GLenum mode) {
	// GL_FOG_HINT and friends do not exist on GLES; passing them through
	// would only raise GL_INVALID_ENUM.
	if (target == GL_GENERATE_MIPMAP_HINT ||
	    target == GL_FRAGMENT_SHADER_DERIVATIVE_HINT)
		glHint(target, mode);
}

// --------------------------------------------------------------------
//	Draw calls
// --------------------------------------------------------------------

extern "C" void etrgl_DrawArrays(GLenum mode, GLint first, GLsizei count) {
	applyState();

	bool needs_quads = false;
	const GLenum prim = translatePrimitive(mode, needs_quads);

	if (needs_quads) {
		// Quad expansion indexes from zero, so a non-zero 'first' would need
		// rebasing. The game never uses one here.
		GLsizei index_count = 0;
		const GLuint* idx = quadIndices(count, index_count);
		glDrawElements(prim, index_count, GL_UNSIGNED_INT, idx);
	} else {
		glDrawArrays(prim, first, count);
	}
}

extern "C" void etrgl_DrawElements(GLenum mode, GLsizei count, GLenum type,
                                   const void* indices) {
	applyState();
	bool needs_quads = false;
	const GLenum prim = translatePrimitive(mode, needs_quads);
	// Indexed quads are never drawn by the game, so the index list is
	// forwarded as-is.
	glDrawElements(prim, count, type, indices);
}

// --------------------------------------------------------------------
//	Texture bookkeeping
// --------------------------------------------------------------------

extern "C" void etrgl_BindTexture(GLenum target, GLuint texture) {
	if (target == GL_TEXTURE_2D) g.bound_texture = texture;
	glBindTexture(target, texture);
}

extern "C" void etrgl_TexImage2D(GLenum target, GLint level,
                                 GLint internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLenum format,
                                 GLenum type, const void* pixels) {
	if (target == GL_TEXTURE_2D && level == 0)
		g.texture_size[g.bound_texture] = {width, height};
	glTexImage2D(target, level, internalformat, width, height, border, format,
	             type, pixels);
}

extern "C" void etrgl_GetTexLevelParameteriv(GLenum /*target*/, GLint /*level*/,
                                             GLenum pname, GLint* params) {
	auto it = g.texture_size.find(g.bound_texture);
	const GLint w = (it != g.texture_size.end()) ? it->second.first : 0;
	const GLint h = (it != g.texture_size.end()) ? it->second.second : 0;
	*params = (pname == GL_TEXTURE_HEIGHT) ? h : w;
}

/** Lets the platform layer register a texture whose storage it created
 *  itself, so glGetTexLevelParameteriv() still reports the right size. */
extern "C" void etrgl_NoteTextureSize(GLuint texture, GLint w, GLint h) {
	g.texture_size[texture] = {w, h};
}

// --------------------------------------------------------------------
//	GLU replacements
// --------------------------------------------------------------------

extern "C" void etrgl_Perspective(GLdouble fovy, GLdouble aspect, GLdouble zn,
                                  GLdouble zf) {
	const double f = 1.0 / std::tan(fovy * 3.14159265358979323846 / 360.0);
	Mat4 p{};
	p.m[0]  = static_cast<float>(f / aspect);
	p.m[5]  = static_cast<float>(f);
	p.m[10] = static_cast<float>((zf + zn) / (zn - zf));
	p.m[11] = -1.f;
	p.m[14] = static_cast<float>((2.0 * zf * zn) / (zn - zf));

	Mat4& c = g.current();
	c = multiply(c, p);
}

extern "C" void etrgl_LookAt(GLdouble ex, GLdouble ey, GLdouble ez,
                             GLdouble cx, GLdouble cy, GLdouble cz,
                             GLdouble ux, GLdouble uy, GLdouble uz) {
	double fx = cx - ex, fy = cy - ey, fz = cz - ez;
	double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
	if (fl < 1e-12) return;
	fx /= fl; fy /= fl; fz /= fl;

	double ul = std::sqrt(ux * ux + uy * uy + uz * uz);
	if (ul < 1e-12) return;
	ux /= ul; uy /= ul; uz /= ul;

	// s = f x up, u = s x f
	double sx = fy * uz - fz * uy;
	double sy = fz * ux - fx * uz;
	double sz = fx * uy - fy * ux;
	const double sl = std::sqrt(sx * sx + sy * sy + sz * sz);
	if (sl < 1e-12) return;
	sx /= sl; sy /= sl; sz /= sl;

	const double vx = sy * fz - sz * fy;
	const double vy = sz * fx - sx * fz;
	const double vz = sx * fy - sy * fx;

	Mat4 m = identity();
	m.m[0] = static_cast<float>(sx); m.m[4] = static_cast<float>(sy); m.m[8]  = static_cast<float>(sz);
	m.m[1] = static_cast<float>(vx); m.m[5] = static_cast<float>(vy); m.m[9]  = static_cast<float>(vz);
	m.m[2] = static_cast<float>(-fx); m.m[6] = static_cast<float>(-fy); m.m[10] = static_cast<float>(-fz);

	Mat4& c = g.current();
	c = multiply(c, m);
	etrgl_Translated(-ex, -ey, -ez);
}

extern "C" const GLubyte* etrgl_ErrorString(GLenum err) {
	switch (err) {
		case GL_NO_ERROR:          return (const GLubyte*)"no error";
		case GL_INVALID_ENUM:      return (const GLubyte*)"invalid enum";
		case GL_INVALID_VALUE:     return (const GLubyte*)"invalid value";
		case GL_INVALID_OPERATION: return (const GLubyte*)"invalid operation";
		case GL_OUT_OF_MEMORY:     return (const GLubyte*)"out of memory";
		case GL_INVALID_FRAMEBUFFER_OPERATION:
			return (const GLubyte*)"invalid framebuffer operation";
		default:                   return (const GLubyte*)"unknown error";
	}
}

// --------------------------------------------------------------------
//	GLU quadrics -- just enough for the penguin's spheres
// --------------------------------------------------------------------

struct ETRQuadric {
	GLenum orientation = GLU_OUTSIDE;
};

extern "C" ETRQuadric* etrglu_NewQuadric(void) { return new ETRQuadric(); }
extern "C" void etrglu_DeleteQuadric(ETRQuadric* q) { delete q; }
extern "C" void etrglu_QuadricDrawStyle(ETRQuadric*, GLenum) {}
extern "C" void etrglu_QuadricNormals(ETRQuadric*, GLenum) {}

extern "C" void etrglu_QuadricOrientation(ETRQuadric* q, GLenum orientation) {
	if (q) q->orientation = orientation;
}

extern "C" void etrglu_Sphere(ETRQuadric* q, GLdouble radius, GLint slices,
                              GLint stacks) {
	if (slices < 3) slices = 3;
	if (stacks < 2) stacks = 2;

	const bool inside = q && q->orientation == GLU_INSIDE;
	const float pi = 3.14159265358979323846f;

	// Emitted through the immediate-mode path so it picks up the current
	// material, matrix and texture state exactly as GLU's sphere did.
	for (int i = 0; i < stacks; i++) {
		const float phi0 = pi * static_cast<float>(i) / stacks;
		const float phi1 = pi * static_cast<float>(i + 1) / stacks;

		etrgl_Begin(GL_TRIANGLE_STRIP);
		for (int j = 0; j <= slices; j++) {
			const float theta = 2.f * pi * static_cast<float>(j) / slices;
			const float ct = std::cos(theta), st = std::sin(theta);

			for (int k = 0; k < 2; k++) {
				const float phi = (k == 0) ? phi0 : phi1;
				const float sp = std::sin(phi), cp = std::cos(phi);
				float nx = sp * ct, ny = sp * st, nz = cp;
				if (inside) { nx = -nx; ny = -ny; nz = -nz; }

				etrgl_Normal3f(nx, ny, nz);
				etrgl_TexCoord2f(static_cast<float>(j) / slices,
				                 1.f - phi / pi);
				etrgl_Vertex3f(static_cast<float>(radius) * sp * ct,
				               static_cast<float>(radius) * sp * st,
				               static_cast<float>(radius) * cp);
			}
		}
		etrgl_End();
	}
}
