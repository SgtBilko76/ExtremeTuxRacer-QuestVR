/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer: Graphics module

All 2D drawing goes through the GLES shim's immediate mode, in SFML's
screen-space convention: origin top-left, y increasing downwards. The
game brackets its text and widget drawing with pushGLStates()/popGLStates()
(via Winsys.beginSFML()/endSFML()), and those are what set that projection
up and tear it down again.
---------------------------------------------------------------------*/

#include "SFML/Graphics.hpp"
#include "platform.h"

#include "../gles/gl_compat.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRGfx", __VA_ARGS__)

namespace sf {

// --------------------------------------------------------------------
//	Constants
// --------------------------------------------------------------------

const Color Color::Black(0, 0, 0);
const Color Color::White(255, 255, 255);
const Color Color::Red(255, 0, 0);
const Color Color::Green(0, 255, 0);
const Color Color::Blue(0, 0, 255);
const Color Color::Yellow(255, 255, 0);
const Color Color::Magenta(255, 0, 255);
const Color Color::Cyan(0, 255, 255);
const Color Color::Transparent(0, 0, 0, 0);

const BlendMode BlendAlpha(BlendMode::Alpha);
const BlendMode BlendAdd(BlendMode::Add);
const BlendMode BlendNone(BlendMode::None);

const RenderStates RenderStates::Default;

namespace {

void applyBlend(const BlendMode& bm) {
	switch (bm.mode) {
		case BlendMode::None:
			glDisable(GL_BLEND);
			break;
		case BlendMode::Add:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		case BlendMode::Multiply:
			glEnable(GL_BLEND);
			glBlendFunc(GL_DST_COLOR, GL_ZERO);
			break;
		default:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
	}
}

/** Emits one textured, tinted quad in screen space. */
void drawQuad(float x, float y, float w, float h,
              float u0, float v0, float u1, float v1, const Color& c) {
	glBegin(GL_QUADS);
	glColor4ub(c.r, c.g, c.b, c.a);
	glTexCoord2f(u0, v0); glVertex2f(x, y);
	glTexCoord2f(u1, v0); glVertex2f(x + w, y);
	glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
	glTexCoord2f(u0, v1); glVertex2f(x, y + h);
	glEnd();
}

}  // namespace

// --------------------------------------------------------------------
//	Image
// --------------------------------------------------------------------

bool Image::loadFromFile(const std::string& filename) {
	int w = 0, h = 0, channels = 0;
	stbi_uc* data = stbi_load(filename.c_str(), &w, &h, &channels, 4);
	if (!data) {
		LOGE("failed to load image '%s': %s", filename.c_str(),
		     stbi_failure_reason());
		return false;
	}

	m_size = Vector2u(static_cast<unsigned int>(w),
	                  static_cast<unsigned int>(h));
	m_pixels.assign(data, data + (static_cast<std::size_t>(w) * h * 4));
	stbi_image_free(data);
	return true;
}

bool Image::saveToFile(const std::string& filename) const {
	// Screenshots are the only writer, and are a convenience rather than a
	// feature the game depends on. Written as a minimal binary PPM so no
	// encoder has to be linked in.
	FILE* f = std::fopen(filename.c_str(), "wb");
	if (!f) return false;

	std::fprintf(f, "P6\n%u %u\n255\n", m_size.x, m_size.y);
	for (std::size_t i = 0; i < m_pixels.size(); i += 4)
		std::fwrite(&m_pixels[i], 1, 3, f);

	std::fclose(f);
	return true;
}

void Image::create(unsigned int w, unsigned int h, const Color& color) {
	m_size = Vector2u(w, h);
	m_pixels.resize(static_cast<std::size_t>(w) * h * 4);
	for (std::size_t i = 0; i < m_pixels.size(); i += 4) {
		m_pixels[i]     = color.r;
		m_pixels[i + 1] = color.g;
		m_pixels[i + 2] = color.b;
		m_pixels[i + 3] = color.a;
	}
}

void Image::flipVertically() {
	const std::size_t row = static_cast<std::size_t>(m_size.x) * 4;
	std::vector<Uint8> tmp(row);
	for (unsigned int y = 0; y < m_size.y / 2; y++) {
		Uint8* a = &m_pixels[y * row];
		Uint8* b = &m_pixels[(m_size.y - 1 - y) * row];
		std::memcpy(tmp.data(), a, row);
		std::memcpy(a, b, row);
		std::memcpy(b, tmp.data(), row);
	}
}

Color Image::getPixel(unsigned int x, unsigned int y) const {
	if (x >= m_size.x || y >= m_size.y) return Color::Black;
	const std::size_t i = (static_cast<std::size_t>(y) * m_size.x + x) * 4;
	return Color(m_pixels[i], m_pixels[i + 1], m_pixels[i + 2], m_pixels[i + 3]);
}

void Image::setPixel(unsigned int x, unsigned int y, const Color& c) {
	if (x >= m_size.x || y >= m_size.y) return;
	const std::size_t i = (static_cast<std::size_t>(y) * m_size.x + x) * 4;
	m_pixels[i]     = c.r;
	m_pixels[i + 1] = c.g;
	m_pixels[i + 2] = c.b;
	m_pixels[i + 3] = c.a;
}

// --------------------------------------------------------------------
//	Texture
// --------------------------------------------------------------------

Texture::Texture()
	: m_handle(0), m_size(0, 0), m_smooth(false), m_repeated(false) {}

Texture::Texture(const Texture& other)
	: m_handle(0), m_size(0, 0), m_smooth(other.m_smooth),
	  m_repeated(other.m_repeated) {
	if (!other.m_pixels.empty()) {
		Image img;
		img.m_size = other.m_size;
		img.m_pixels = other.m_pixels;
		loadFromImage(img);
	}
}

Texture& Texture::operator=(const Texture& other) {
	if (this == &other) return *this;
	release();
	m_smooth = other.m_smooth;
	m_repeated = other.m_repeated;
	if (!other.m_pixels.empty()) {
		Image img;
		img.m_size = other.m_size;
		img.m_pixels = other.m_pixels;
		loadFromImage(img);
	}
	return *this;
}

Texture::~Texture() { release(); }

void Texture::release() {
	if (m_handle) {
		GLuint h = m_handle;
		glDeleteTextures(1, &h);
		m_handle = 0;
	}
	m_pixels.clear();
	m_size = Vector2u(0, 0);
}

bool Texture::loadFromFile(const std::string& filename, const IntRect&) {
	Image img;
	if (!img.loadFromFile(filename)) return false;
	return loadFromImage(img);
}

bool Texture::loadFromImage(const Image& image) {
	if (image.m_size.x == 0 || image.m_size.y == 0) return false;

	if (!m_handle) {
		GLuint h = 0;
		glGenTextures(1, &h);
		m_handle = h;
	}
	m_size = image.m_size;
	m_pixels = image.m_pixels;

	glBindTexture(GL_TEXTURE_2D, m_handle);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_size.x, m_size.y, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, m_pixels.data());
	applyParameters();
	etrgl_NoteTextureSize(m_handle, m_size.x, m_size.y);
	return true;
}

bool Texture::create(unsigned int w, unsigned int h) {
	if (!m_handle) {
		GLuint handle = 0;
		glGenTextures(1, &handle);
		m_handle = handle;
	}
	m_size = Vector2u(w, h);
	m_pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);

	glBindTexture(GL_TEXTURE_2D, m_handle);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, nullptr);
	applyParameters();
	etrgl_NoteTextureSize(m_handle, w, h);
	return true;
}

void Texture::update(const Uint8* pixels) {
	if (!m_handle || !pixels) return;
	const std::size_t bytes = static_cast<std::size_t>(m_size.x) * m_size.y * 4;
	m_pixels.assign(pixels, pixels + bytes);

	glBindTexture(GL_TEXTURE_2D, m_handle);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_size.x, m_size.y, GL_RGBA,
	                GL_UNSIGNED_BYTE, pixels);
}

void Texture::update(const Window& window) {
	// GLES can only read back the default framebuffer, which is exactly
	// what the screenshot path wants.
	const Vector2u size = window.getSize();
	if (size.x == 0 || size.y == 0) return;
	if (m_size.x != size.x || m_size.y != size.y) create(size.x, size.y);

	m_pixels.assign(static_cast<std::size_t>(size.x) * size.y * 4, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, static_cast<GLsizei>(size.x),
	             static_cast<GLsizei>(size.y), GL_RGBA, GL_UNSIGNED_BYTE,
	             m_pixels.data());

	glBindTexture(GL_TEXTURE_2D, m_handle);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.x, size.y, GL_RGBA,
	                GL_UNSIGNED_BYTE, m_pixels.data());
}

void Texture::applyParameters() const {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
	                m_smooth ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
	                m_smooth ? GL_LINEAR : GL_NEAREST);
	const GLint wrap = m_repeated ? GL_REPEAT : GL_CLAMP_TO_EDGE;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}

void Texture::setSmooth(bool smooth) {
	m_smooth = smooth;
	if (m_handle) {
		glBindTexture(GL_TEXTURE_2D, m_handle);
		applyParameters();
	}
}

void Texture::setRepeated(bool repeated) {
	m_repeated = repeated;
	if (m_handle) {
		glBindTexture(GL_TEXTURE_2D, m_handle);
		applyParameters();
	}
}

Image Texture::copyToImage() const {
	// GLES cannot read a texture back directly, so the CPU-side copy kept
	// at upload time is what gets returned.
	Image img;
	img.m_size = m_size;
	img.m_pixels = m_pixels;
	if (img.m_pixels.empty())
		img.create(m_size.x, m_size.y);
	return img;
}

void Texture::bind(const Texture* texture) {
	if (texture && texture->m_handle) {
		glBindTexture(GL_TEXTURE_2D, texture->m_handle);
	} else {
		glBindTexture(GL_TEXTURE_2D, 0);
	}
}

// --------------------------------------------------------------------
//	Font
// --------------------------------------------------------------------

struct Font::Page {
	Texture texture;
	std::vector<Uint8> atlas;          // single-channel coverage
	unsigned int width = 512, height = 512;
	unsigned int pen_x = 1, pen_y = 1, row_height = 0;
	std::map<Uint32, Glyph> glyphs;
	float scale = 1.f;
	float ascent = 0.f, descent = 0.f, line_gap = 0.f;
	bool dirty = false;
};

struct Font::Impl {
	std::vector<unsigned char> file;
	stbtt_fontinfo info{};
	bool loaded = false;
	mutable std::map<unsigned int, Page> pages;
};

Font::Font() : m_impl(new Impl) {}
Font::~Font() {}

bool Font::loadFromFile(const std::string& filename) {
	// bionic's fopen() happily opens a directory for reading, and seeking to
	// its end then yields a nonsense length. Without this check a malformed
	// font path turns into a multi-gigabyte allocation and a bad_alloc abort
	// rather than a clean failure.
	struct stat st;
	if (stat(filename.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
		LOGE("font '%s' is not a readable regular file", filename.c_str());
		return false;
	}

	FILE* f = std::fopen(filename.c_str(), "rb");
	if (!f) {
		LOGE("failed to open font '%s'", filename.c_str());
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);

	// 64 MB is far beyond any legitimate TrueType face.
	if (size <= 0 || size > 64L * 1024 * 1024) {
		LOGE("font '%s' has an implausible size (%ld)", filename.c_str(), size);
		std::fclose(f);
		return false;
	}

	m_impl->file.resize(static_cast<std::size_t>(size));
	const std::size_t got = std::fread(m_impl->file.data(), 1,
	                                   m_impl->file.size(), f);
	std::fclose(f);
	if (got != m_impl->file.size()) return false;

	if (!stbtt_InitFont(&m_impl->info, m_impl->file.data(),
	                    stbtt_GetFontOffsetForIndex(m_impl->file.data(), 0))) {
		LOGE("stbtt_InitFont failed for '%s'", filename.c_str());
		return false;
	}
	m_impl->loaded = true;
	m_impl->pages.clear();
	return true;
}

Font::Page& Font::getPage(unsigned int characterSize) const {
	auto it = m_impl->pages.find(characterSize);
	if (it != m_impl->pages.end()) return it->second;

	Page& page = m_impl->pages[characterSize];
	page.atlas.assign(static_cast<std::size_t>(page.width) * page.height, 0);
	page.scale = stbtt_ScaleForPixelHeight(&m_impl->info,
	                                       static_cast<float>(characterSize));

	int ascent = 0, descent = 0, line_gap = 0;
	stbtt_GetFontVMetrics(&m_impl->info, &ascent, &descent, &line_gap);
	page.ascent   = ascent * page.scale;
	page.descent  = descent * page.scale;
	page.line_gap = line_gap * page.scale;

	page.texture.setSmooth(true);
	page.texture.create(page.width, page.height);
	return page;
}

const Glyph& Font::getGlyph(Uint32 codepoint,
                            unsigned int characterSize) const {
	static const Glyph empty;
	if (!m_impl->loaded) return empty;

	Page& page = getPage(characterSize);
	auto it = page.glyphs.find(codepoint);
	if (it != page.glyphs.end()) return it->second;

	Glyph glyph;

	int advance = 0, lsb = 0;
	stbtt_GetCodepointHMetrics(&m_impl->info, static_cast<int>(codepoint),
	                           &advance, &lsb);
	glyph.advance = advance * page.scale;

	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	stbtt_GetCodepointBitmapBox(&m_impl->info, static_cast<int>(codepoint),
	                            page.scale, page.scale, &x0, &y0, &x1, &y1);

	const unsigned int gw = static_cast<unsigned int>(x1 - x0);
	const unsigned int gh = static_cast<unsigned int>(y1 - y0);

	if (gw > 0 && gh > 0) {
		// Simple shelf packer: advance along the row, drop to the next
		// shelf when it no longer fits.
		if (page.pen_x + gw + 1 >= page.width) {
			page.pen_x = 1;
			page.pen_y += page.row_height + 1;
			page.row_height = 0;
		}
		if (page.pen_y + gh + 1 < page.height) {
			stbtt_MakeCodepointBitmap(
			    &m_impl->info,
			    &page.atlas[page.pen_y * page.width + page.pen_x],
			    static_cast<int>(gw), static_cast<int>(gh),
			    static_cast<int>(page.width), page.scale, page.scale,
			    static_cast<int>(codepoint));

			glyph.textureRect = IntRect(page.pen_x, page.pen_y, gw, gh);
			glyph.bounds = FloatRect(static_cast<float>(x0),
			                         static_cast<float>(y0),
			                         static_cast<float>(gw),
			                         static_cast<float>(gh));

			page.pen_x += gw + 1;
			page.row_height = std::max(page.row_height, gh);
			page.dirty = true;
		}
	}

	return page.glyphs.emplace(codepoint, glyph).first->second;
}

float Font::getKerning(Uint32 first, Uint32 second,
                       unsigned int characterSize) const {
	if (!m_impl->loaded) return 0.f;
	Page& page = getPage(characterSize);
	return stbtt_GetCodepointKernAdvance(&m_impl->info,
	                                     static_cast<int>(first),
	                                     static_cast<int>(second)) * page.scale;
}

float Font::getLineSpacing(unsigned int characterSize) const {
	if (!m_impl->loaded) return static_cast<float>(characterSize);
	Page& page = getPage(characterSize);
	return page.ascent - page.descent + page.line_gap;
}

float Font::getAscent(unsigned int characterSize) const {
	if (!m_impl->loaded) return static_cast<float>(characterSize);
	return getPage(characterSize).ascent;
}

const Texture* Font::getTexture(unsigned int characterSize) const {
	if (!m_impl->loaded) return nullptr;
	Page& page = getPage(characterSize);

	if (page.dirty) {
		// Expand the coverage mask to RGBA once per batch of new glyphs
		// rather than on every upload.
		std::vector<Uint8> rgba(page.atlas.size() * 4);
		for (std::size_t i = 0; i < page.atlas.size(); i++) {
			rgba[i * 4]     = 255;
			rgba[i * 4 + 1] = 255;
			rgba[i * 4 + 2] = 255;
			rgba[i * 4 + 3] = page.atlas[i];
		}
		page.texture.update(rgba.data());
		page.dirty = false;
	}
	return &page.texture;
}

// --------------------------------------------------------------------
//	Sprite
// --------------------------------------------------------------------

void Sprite::setTexture(const Texture& texture, bool resetRect) {
	m_texture = &texture;
	if (resetRect || (m_rect.width == 0 && m_rect.height == 0)) {
		m_rect = IntRect(0, 0, static_cast<int>(texture.getSize().x),
		                 static_cast<int>(texture.getSize().y));
	}
}

FloatRect Sprite::getLocalBounds() const {
	return FloatRect(0.f, 0.f, static_cast<float>(std::abs(m_rect.width)),
	                 static_cast<float>(std::abs(m_rect.height)));
}

FloatRect Sprite::getGlobalBounds() const {
	const FloatRect l = getLocalBounds();
	return FloatRect(m_position.x, m_position.y,
	                 l.width * m_scale.x, l.height * m_scale.y);
}

void Sprite::draw(RenderTarget&, const RenderStates& states) const {
	if (!m_texture) return;

	applyBlend(states.blendMode);
	glEnable(GL_TEXTURE_2D);
	Texture::bind(m_texture);

	const Vector2u ts = m_texture->getSize();
	if (ts.x == 0 || ts.y == 0) return;

	const float u0 = static_cast<float>(m_rect.left) / ts.x;
	const float v0 = static_cast<float>(m_rect.top) / ts.y;
	const float u1 = static_cast<float>(m_rect.left + m_rect.width) / ts.x;
	const float v1 = static_cast<float>(m_rect.top + m_rect.height) / ts.y;

	drawQuad(m_position.x, m_position.y,
	         m_rect.width * m_scale.x, m_rect.height * m_scale.y,
	         u0, v0, u1, v1, m_color);
}

// --------------------------------------------------------------------
//	RectangleShape
// --------------------------------------------------------------------

FloatRect RectangleShape::getLocalBounds() const {
	return FloatRect(0.f, 0.f, m_size.x, m_size.y);
}

FloatRect RectangleShape::getGlobalBounds() const {
	return FloatRect(m_position.x, m_position.y,
	                 m_size.x * m_scale.x, m_size.y * m_scale.y);
}

void RectangleShape::draw(RenderTarget&, const RenderStates& states) const {
	applyBlend(states.blendMode);
	glDisable(GL_TEXTURE_2D);

	const float w = m_size.x * m_scale.x;
	const float h = m_size.y * m_scale.y;

	if (m_fill.a > 0)
		drawQuad(m_position.x, m_position.y, w, h, 0, 0, 1, 1, m_fill);

	if (m_thickness > 0.f && m_outline.a > 0) {
		// SFML grows the outline outwards from the shape's edge.
		const float t = m_thickness;
		const float x = m_position.x, y = m_position.y;
		drawQuad(x - t, y - t,     w + 2 * t, t,         0, 0, 1, 1, m_outline);
		drawQuad(x - t, y + h,     w + 2 * t, t,         0, 0, 1, 1, m_outline);
		drawQuad(x - t, y,         t,         h,         0, 0, 1, 1, m_outline);
		drawQuad(x + w, y,         t,         h,         0, 0, 1, 1, m_outline);
	}
}

// --------------------------------------------------------------------
//	Text
// --------------------------------------------------------------------

namespace {

/** Walks a string accumulating pen positions. Shared by layout (bounds)
 *  and drawing so the two can never disagree. */
template <typename F>
void forEachGlyph(const String& str, const Font& font, unsigned int size,
                  F&& fn) {
	const float line_spacing = font.getLineSpacing(size);
	float x = 0.f;
	float y = font.getAscent(size);
	Uint32 prev = 0;

	for (std::size_t i = 0; i < str.getSize(); i++) {
		const Uint32 cp = static_cast<Uint32>(str[i]);

		if (cp == U'\n') {
			x = 0.f;
			y += line_spacing;
			prev = 0;
			continue;
		}
		if (prev) x += font.getKerning(prev, cp, size);

		const Glyph& glyph = font.getGlyph(cp, size);
		fn(glyph, x, y);
		x += glyph.advance;
		prev = cp;
	}
}

}  // namespace

FloatRect Text::getLocalBounds() const {
	if (!m_font || m_string.isEmpty()) return FloatRect();

	float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
	bool any = false;

	forEachGlyph(m_string, *m_font, m_size,
	             [&](const Glyph& gl, float x, float y) {
		if (gl.bounds.width <= 0.f || gl.bounds.height <= 0.f) return;
		const float l = x + gl.bounds.left;
		const float t = y + gl.bounds.top;
		min_x = std::min(min_x, l);
		min_y = std::min(min_y, t);
		max_x = std::max(max_x, l + gl.bounds.width);
		max_y = std::max(max_y, t + gl.bounds.height);
		any = true;
	});

	if (!any) return FloatRect();
	return FloatRect(min_x, min_y, max_x - min_x, max_y - min_y);
}

FloatRect Text::getGlobalBounds() const {
	const FloatRect l = getLocalBounds();
	return FloatRect(m_position.x + l.left * m_scale.x,
	                 m_position.y + l.top * m_scale.y,
	                 l.width * m_scale.x, l.height * m_scale.y);
}

void Text::draw(RenderTarget&, const RenderStates& states) const {
	if (!m_font || m_string.isEmpty()) return;

	const Texture* atlas = m_font->getTexture(m_size);
	if (!atlas) return;

	applyBlend(states.blendMode);
	glEnable(GL_TEXTURE_2D);
	Texture::bind(atlas);

	const Vector2u ts = atlas->getSize();
	if (ts.x == 0 || ts.y == 0) return;

	forEachGlyph(m_string, *m_font, m_size,
	             [&](const Glyph& gl, float x, float y) {
		if (gl.textureRect.width <= 0 || gl.textureRect.height <= 0) return;

		const float px = m_position.x + (x + gl.bounds.left) * m_scale.x;
		const float py = m_position.y + (y + gl.bounds.top) * m_scale.y;

		drawQuad(px, py,
		         gl.bounds.width * m_scale.x, gl.bounds.height * m_scale.y,
		         static_cast<float>(gl.textureRect.left) / ts.x,
		         static_cast<float>(gl.textureRect.top) / ts.y,
		         static_cast<float>(gl.textureRect.left + gl.textureRect.width) / ts.x,
		         static_cast<float>(gl.textureRect.top + gl.textureRect.height) / ts.y,
		         m_fill);
	});
}

// --------------------------------------------------------------------
//	VertexArray
// --------------------------------------------------------------------

void VertexArray::draw(RenderTarget&, const RenderStates& states) const {
	if (m_vertices.empty()) return;

	applyBlend(states.blendMode);
	if (states.texture) {
		glEnable(GL_TEXTURE_2D);
		Texture::bind(states.texture);
	} else {
		glDisable(GL_TEXTURE_2D);
	}

	GLenum mode = GL_TRIANGLES;
	switch (m_type) {
		case Points:        mode = GL_POINTS;         break;
		case Lines:         mode = GL_LINES;          break;
		case LineStrip:     mode = GL_LINE_STRIP;     break;
		case Triangles:     mode = GL_TRIANGLES;      break;
		case TriangleStrip: mode = GL_TRIANGLE_STRIP; break;
		case TriangleFan:   mode = GL_TRIANGLE_FAN;   break;
		case Quads:         mode = GL_QUADS;          break;
	}

	// Texture coordinates in SFML's vertex arrays are in pixels, not
	// normalised, so they need scaling by the texture size here.
	float sx = 1.f, sy = 1.f;
	if (states.texture) {
		const Vector2u ts = states.texture->getSize();
		if (ts.x) sx = 1.f / ts.x;
		if (ts.y) sy = 1.f / ts.y;
	}

	glBegin(mode);
	for (const Vertex& v : m_vertices) {
		glColor4ub(v.color.r, v.color.g, v.color.b, v.color.a);
		glTexCoord2f(v.texCoords.x * sx, v.texCoords.y * sy);
		glVertex2f(v.position.x, v.position.y);
	}
	glEnd();
}

// --------------------------------------------------------------------
//	RenderTarget
// --------------------------------------------------------------------

void RenderTarget::clear(const Color& color) {
	bindTarget();
	glClearColor(color.r / 255.f, color.g / 255.f, color.b / 255.f,
	             color.a / 255.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void RenderTarget::draw(const Drawable& drawable, const RenderStates& states) {
	bindTarget();
	drawable.draw(*this, states);
}

void RenderTarget::draw(const Vertex* vertices, std::size_t count,
                        PrimitiveType type, const RenderStates& states) {
	if (!vertices || count == 0) return;
	VertexArray arr(type, count);
	for (std::size_t i = 0; i < count; i++) arr[i] = vertices[i];
	draw(arr, states);
}

void RenderTarget::pushGLStates() {
	if (m_states_pushed) return;
	m_states_pushed = true;

	bindTarget();

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	// SFML's default view: origin top-left, y down.
	const Vector2u size = getSize();
	glOrtho(0.0, static_cast<double>(size.x), static_cast<double>(size.y), 0.0,
	        -1.0, 1.0);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_FOG);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f(1.f, 1.f, 1.f, 1.f);
}

void RenderTarget::popGLStates() {
	if (!m_states_pushed) return;
	m_states_pushed = false;

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();

	glEnable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
}

void RenderTarget::resetGLStates() { etrgl_InvalidateState(); }

// --------------------------------------------------------------------
//	RenderWindow
// --------------------------------------------------------------------

RenderWindow::RenderWindow(VideoMode mode, const String& title, Uint32 style,
                           const ContextSettings& settings) {
	create(mode, title, style, settings);
}

void RenderWindow::bindTarget() {
	// Framebuffer 0 is the SDL window; in VR the XR layer rebinds an eye
	// framebuffer around whole frames instead, and the 2D overlay is drawn
	// into whatever is currently bound.
}

// --------------------------------------------------------------------
//	RenderTexture
// --------------------------------------------------------------------

RenderTexture::RenderTexture() : m_fbo(0), m_depth_rb(0) {}

RenderTexture::~RenderTexture() {
	if (m_fbo) {
		GLuint f = m_fbo;
		glDeleteFramebuffers(1, &f);
	}
	if (m_depth_rb) {
		GLuint r = m_depth_rb;
		glDeleteRenderbuffers(1, &r);
	}
}

bool RenderTexture::create(unsigned int width, unsigned int height,
                           bool depthBuffer) {
	if (!m_texture.create(width, height)) return false;

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	m_fbo = fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       m_texture.getNativeHandle(), 0);

	if (depthBuffer) {
		GLuint rb = 0;
		glGenRenderbuffers(1, &rb);
		m_depth_rb = rb;
		glBindRenderbuffer(GL_RENDERBUFFER, rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width,
		                      height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
		                          GL_RENDERBUFFER, rb);
	}

	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		LOGE("RenderTexture framebuffer incomplete: 0x%x", status);
		return false;
	}
	return true;
}

void RenderTexture::bindTarget() {
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	const Vector2u s = m_texture.getSize();
	glViewport(0, 0, static_cast<GLsizei>(s.x), static_cast<GLsizei>(s.y));
}

void RenderTexture::display() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	unsigned int w, h;
	etr_platform::GetDrawableSize(w, h);
	glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
}

}  // namespace sf
