/* --------------------------------------------------------------------
EXTREME TUXRACER -- GLES shim uber-shader

One program reproduces the whole fixed-function pipeline the game relies
on: per-vertex directional/positional lighting, object-linear texgen,
MODULATE and DECAL texture environments, alpha test and the three fog
modes. Everything is switched by uniforms rather than by compiling
variants -- the branches are uniform-controlled, so they cost effectively
nothing, and it keeps the state machine in gl_compat.cpp simple.
---------------------------------------------------------------------*/

#include "gl_internal.h"

#include <android/log.h>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRGL", __VA_ARGS__)

namespace {

const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;

uniform mat4 uModelView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;

uniform bool  uUseLighting;
uniform vec4  uLightPosEye;
uniform vec4  uLightAmbient;
uniform vec4  uLightDiffuse;
uniform vec4  uLightSpecular;
uniform vec4  uMatAmbDiff;
uniform vec4  uMatSpecular;
uniform float uMatShininess;

uniform bool uUseTexGen;
uniform vec4 uSPlane;
uniform vec4 uTPlane;

out vec4  vColor;
out vec2  vTexCoord;
out float vFogDist;

void main()
{
    vec4 eye = uModelView * aPos;
    gl_Position = uProjection * eye;

    // Fixed-function fog is a function of eye-space distance from the
    // viewer; -z matches what GL uses when GL_FOG_COORD_SRC is default.
    vFogDist = -eye.z;

    // GL_OBJECT_LINEAR texgen: the plane equation is dotted with the
    // untransformed object-space position.
    vTexCoord = uUseTexGen
              ? vec2(dot(aPos, uSPlane), dot(aPos, uTPlane))
              : aTexCoord;

    if (uUseLighting)
    {
        vec3 n = normalize(uNormalMatrix * aNormal);
        vec3 l = (uLightPosEye.w == 0.0)
               ? normalize(uLightPosEye.xyz)
               : normalize(uLightPosEye.xyz - eye.xyz);

        float ndotl = max(dot(n, l), 0.0);
        vec3 c = uLightAmbient.rgb * uMatAmbDiff.rgb
               + uLightDiffuse.rgb * uMatAmbDiff.rgb * ndotl;

        if (ndotl > 0.0)
        {
            // Blinn-Foley halfway vector, as the fixed-function pipeline
            // uses when GL_LIGHT_MODEL_LOCAL_VIEWER is off.
            vec3 v = normalize(-eye.xyz);
            vec3 h = normalize(l + v);
            c += uLightSpecular.rgb * uMatSpecular.rgb
               * pow(max(dot(n, h), 0.0), max(uMatShininess, 1.0));
        }
        vColor = vec4(c, uMatAmbDiff.a);
    }
    else
    {
        vColor = aColor;
    }
}
)";

const char* kFragmentShader = R"(#version 300 es
precision mediump float;

in vec4  vColor;
in vec2  vTexCoord;
in float vFogDist;

uniform bool      uUseTexture;
uniform int       uTexEnvMode;
uniform sampler2D uTex;

uniform bool  uUseFog;
uniform int   uFogMode;
uniform vec4  uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogDensity;

uniform bool  uUseAlphaTest;
uniform float uAlphaRef;

out vec4 fragColor;

void main()
{
    vec4 c = vColor;

    if (uUseTexture)
    {
        vec4 t = texture(uTex, vTexCoord);
        if (uTexEnvMode == 1)        // GL_DECAL
            c = vec4(mix(c.rgb, t.rgb, t.a), c.a);
        else if (uTexEnvMode == 2)   // GL_REPLACE
            c = t;
        else                         // GL_MODULATE
            c = c * t;
    }

    // The game only ever uses glAlphaFunc(GL_GEQUAL, ref), so the test is
    // simply "discard anything below the reference".
    if (uUseAlphaTest && c.a < uAlphaRef)
        discard;

    if (uUseFog)
    {
        float f;
        if (uFogMode == 0)
            f = (uFogEnd - vFogDist) / (uFogEnd - uFogStart);
        else if (uFogMode == 1)
            f = exp(-uFogDensity * vFogDist);
        else
            f = exp(-(uFogDensity * vFogDist) * (uFogDensity * vFogDist));

        c = vec4(mix(uFogColor.rgb, c.rgb, clamp(f, 0.0, 1.0)), c.a);
    }

    fragColor = c;
}
)";

GLuint compile(GLenum type, const char* src) {
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, nullptr);
	glCompileShader(sh);

	GLint ok = GL_FALSE;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint len = 0;
		glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
		std::vector<char> log(len > 1 ? len : 1);
		glGetShaderInfoLog(sh, log.size(), nullptr, log.data());
		LOGE("%s shader compile failed:\n%s",
		     type == GL_VERTEX_SHADER ? "vertex" : "fragment", log.data());
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

}  // namespace

bool etrglInternal_BuildShader(EtrShader& out) {
	GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
	if (!vs) return false;
	GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
	if (!fs) {
		glDeleteShader(vs);
		return false;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = GL_FALSE;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		GLint len = 0;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
		std::vector<char> log(len > 1 ? len : 1);
		glGetProgramInfoLog(prog, log.size(), nullptr, log.data());
		LOGE("program link failed:\n%s", log.data());
		glDeleteProgram(prog);
		return false;
	}

	out.program = prog;

#define U(field, name) out.field = glGetUniformLocation(prog, name)
	U(uModelView,    "uModelView");
	U(uProjection,   "uProjection");
	U(uNormalMatrix, "uNormalMatrix");

	U(uUseLighting,   "uUseLighting");
	U(uLightPosEye,   "uLightPosEye");
	U(uLightAmbient,  "uLightAmbient");
	U(uLightDiffuse,  "uLightDiffuse");
	U(uLightSpecular, "uLightSpecular");
	U(uMatAmbDiff,    "uMatAmbDiff");
	U(uMatSpecular,   "uMatSpecular");
	U(uMatShininess,  "uMatShininess");

	U(uUseTexGen, "uUseTexGen");
	U(uSPlane,    "uSPlane");
	U(uTPlane,    "uTPlane");

	U(uUseTexture, "uUseTexture");
	U(uTexEnvMode, "uTexEnvMode");
	U(uTex,        "uTex");

	U(uUseFog,     "uUseFog");
	U(uFogMode,    "uFogMode");
	U(uFogColor,   "uFogColor");
	U(uFogStart,   "uFogStart");
	U(uFogEnd,     "uFogEnd");
	U(uFogDensity, "uFogDensity");

	U(uUseAlphaTest, "uUseAlphaTest");
	U(uAlphaRef,     "uAlphaRef");
#undef U

	// Only texture unit 0 is ever used.
	glUseProgram(prog);
	glUniform1i(out.uTex, 0);
	glUseProgram(0);

	return true;
}
