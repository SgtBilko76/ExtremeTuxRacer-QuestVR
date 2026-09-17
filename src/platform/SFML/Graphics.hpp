/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer for Android

Graphics module: colours, rectangles, images, textures, sprites, text and
the render targets. Everything draws as textured quads through the GLES 3
shim in src/gles/, in SFML's screen-space convention (origin top-left,
y increasing downwards).

Image decoding uses stb_image, glyph rasterisation uses stb_truetype.
---------------------------------------------------------------------*/

#ifndef ETR_SFML_GRAPHICS_HPP
#define ETR_SFML_GRAPHICS_HPP

#include "System.hpp"
#include "Window.hpp"

#include <map>
#include <memory>
#include <vector>

namespace sf {

// --------------------------------------------------------------------
//	Color
// --------------------------------------------------------------------

class Color {
public:
	Uint8 r, g, b, a;

	constexpr Color() : r(0), g(0), b(0), a(255) {}
	constexpr Color(Uint8 r_, Uint8 g_, Uint8 b_, Uint8 a_ = 255)
		: r(r_), g(g_), b(b_), a(a_) {}

	static const Color Black;
	static const Color White;
	static const Color Red;
	static const Color Green;
	static const Color Blue;
	static const Color Yellow;
	static const Color Magenta;
	static const Color Cyan;
	static const Color Transparent;

	friend bool operator==(const Color& a, const Color& b) {
		return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
	}
	friend bool operator!=(const Color& a, const Color& b) { return !(a == b); }
};

// --------------------------------------------------------------------
//	Rect
// --------------------------------------------------------------------

template <typename T>
class Rect {
public:
	T left, top, width, height;

	Rect() : left(0), top(0), width(0), height(0) {}
	Rect(T l, T t, T w, T h) : left(l), top(t), width(w), height(h) {}

	template <typename U>
	explicit Rect(const Rect<U>& r)
		: left(static_cast<T>(r.left)), top(static_cast<T>(r.top)),
		  width(static_cast<T>(r.width)), height(static_cast<T>(r.height)) {}

	bool contains(T x, T y) const {
		return x >= left && x < left + width && y >= top && y < top + height;
	}
};

typedef Rect<int>   IntRect;
typedef Rect<float> FloatRect;

// --------------------------------------------------------------------
//	Primitive types / blending / render states
// --------------------------------------------------------------------

enum PrimitiveType {
	Points, Lines, LineStrip, Triangles, TriangleStrip, TriangleFan, Quads
};

class BlendMode {
public:
	enum Mode { Alpha, Add, Multiply, None };
	Mode mode;
	BlendMode(Mode m = Alpha) : mode(m) {}
};

extern const BlendMode BlendAlpha;
extern const BlendMode BlendAdd;
extern const BlendMode BlendNone;

class Texture;

class RenderStates {
public:
	BlendMode blendMode;
	const Texture* texture;

	RenderStates() : blendMode(BlendMode::Alpha), texture(nullptr) {}
	RenderStates(const BlendMode& bm) : blendMode(bm), texture(nullptr) {}
	RenderStates(const Texture* tex) : blendMode(BlendMode::Alpha), texture(tex) {}

	static const RenderStates Default;
};

// --------------------------------------------------------------------
//	Vertex
// --------------------------------------------------------------------

class Vertex {
public:
	Vector2f position;
	Color    color;
	Vector2f texCoords;

	Vertex() : color(Color::White) {}
	Vertex(const Vector2f& pos) : position(pos), color(Color::White) {}
	Vertex(const Vector2f& pos, const Color& col) : position(pos), color(col) {}
	Vertex(const Vector2f& pos, const Color& col, const Vector2f& tex)
		: position(pos), color(col), texCoords(tex) {}
};

// --------------------------------------------------------------------
//	Image
// --------------------------------------------------------------------

/** CPU-side RGBA8 pixel buffer. course.cpp reads elevation and terrain
 *  maps through this, so getPixelsPtr() must stay tightly packed. */
class Image {
public:
	Image() : m_size(0, 0) {}

	bool loadFromFile(const std::string& filename);
	bool saveToFile(const std::string& filename) const;
	void create(unsigned int w, unsigned int h,
	            const Color& color = Color(0, 0, 0));

	Vector2u getSize() const { return m_size; }
	const Uint8* getPixelsPtr() const { return m_pixels.data(); }
	void flipVertically();

	Color getPixel(unsigned int x, unsigned int y) const;
	void setPixel(unsigned int x, unsigned int y, const Color& c);

private:
	friend class Texture;
	Vector2u m_size;
	std::vector<Uint8> m_pixels;
};

// --------------------------------------------------------------------
//	Texture
// --------------------------------------------------------------------

class Texture {
public:
	Texture();
	Texture(const Texture& other);
	Texture& operator=(const Texture& other);
	~Texture();

	bool loadFromFile(const std::string& filename,
	                  const IntRect& area = IntRect());
	bool loadFromImage(const Image& image);
	bool create(unsigned int w, unsigned int h);
	void update(const Uint8* pixels);
	/** Grabs the current framebuffer contents. Used only by the
	 *  screenshot path. */
	void update(const Window& window);

	Vector2u getSize() const { return m_size; }
	unsigned int getNativeHandle() const { return m_handle; }

	void setSmooth(bool smooth);
	void setRepeated(bool repeated);
	bool isSmooth() const { return m_smooth; }
	bool isRepeated() const { return m_repeated; }

	Image copyToImage() const;

	static void bind(const Texture* texture);

private:
	void applyParameters() const;
	void release();

	unsigned int m_handle;
	Vector2u m_size;
	bool m_smooth;
	bool m_repeated;
	// The CPU copy backs copyToImage(), which GLES cannot do natively.
	std::vector<Uint8> m_pixels;
};

// --------------------------------------------------------------------
//	Font / glyphs
// --------------------------------------------------------------------

struct Glyph {
	FloatRect bounds;      // offset from the pen position, in pixels
	IntRect   textureRect; // location in the atlas
	float     advance = 0.f;
};

class RenderTarget;

/** TrueType face, rasterised on demand by stb_truetype into one atlas per
 *  character size. */
class Font {
public:
	Font();
	~Font();

	bool loadFromFile(const std::string& filename);

	const Glyph& getGlyph(Uint32 codepoint, unsigned int characterSize) const;
	float getKerning(Uint32 first, Uint32 second,
	                 unsigned int characterSize) const;
	float getLineSpacing(unsigned int characterSize) const;
	float getAscent(unsigned int characterSize) const;
	const Texture* getTexture(unsigned int characterSize) const;

private:
	struct Page;
	Page& getPage(unsigned int characterSize) const;

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// --------------------------------------------------------------------
//	Drawables
// --------------------------------------------------------------------

class Drawable {
public:
	virtual ~Drawable() {}
	virtual void draw(RenderTarget& target, const RenderStates& states) const = 0;
};

class Transformable {
public:
	Transformable() : m_position(0.f, 0.f), m_scale(1.f, 1.f) {}

	void setPosition(float x, float y) { m_position = Vector2f(x, y); }
	void setPosition(const Vector2f& p) { m_position = p; }
	const Vector2f& getPosition() const { return m_position; }

	void setScale(float x, float y) { m_scale = Vector2f(x, y); }
	void setScale(const Vector2f& s) { m_scale = s; }
	const Vector2f& getScale() const { return m_scale; }

	void move(float dx, float dy) { m_position.x += dx; m_position.y += dy; }

protected:
	Vector2f m_position;
	Vector2f m_scale;
};

class Sprite : public Drawable, public Transformable {
public:
	Sprite() : m_texture(nullptr), m_color(Color::White) {}
	explicit Sprite(const Texture& texture) : m_texture(nullptr),
	                                          m_color(Color::White) {
		setTexture(texture, true);
	}

	void setTexture(const Texture& texture, bool resetRect = false);
	void setTextureRect(const IntRect& rect) { m_rect = rect; }
	void setColor(const Color& color) { m_color = color; }

	const Texture* getTexture() const { return m_texture; }
	const IntRect& getTextureRect() const { return m_rect; }
	const Color& getColor() const { return m_color; }

	FloatRect getLocalBounds() const;
	FloatRect getGlobalBounds() const;

	void draw(RenderTarget& target, const RenderStates& states) const override;

private:
	const Texture* m_texture;
	IntRect m_rect;
	Color m_color;
};

class RectangleShape : public Drawable, public Transformable {
public:
	RectangleShape() : m_size(0.f, 0.f), m_fill(Color::White),
	                   m_outline(Color::Transparent), m_thickness(0.f) {}
	explicit RectangleShape(const Vector2f& size)
		: m_size(size), m_fill(Color::White),
		  m_outline(Color::Transparent), m_thickness(0.f) {}

	void setSize(const Vector2f& size) { m_size = size; }
	const Vector2f& getSize() const { return m_size; }

	void setFillColor(const Color& c) { m_fill = c; }
	void setOutlineColor(const Color& c) { m_outline = c; }
	void setOutlineThickness(float t) { m_thickness = t; }

	const Color& getFillColor() const { return m_fill; }
	const Color& getOutlineColor() const { return m_outline; }
	float getOutlineThickness() const { return m_thickness; }

	FloatRect getLocalBounds() const;
	FloatRect getGlobalBounds() const;

	void draw(RenderTarget& target, const RenderStates& states) const override;

private:
	Vector2f m_size;
	Color m_fill, m_outline;
	float m_thickness;
};

class Text : public Drawable, public Transformable {
public:
	enum Style { Regular = 0, Bold = 1, Italic = 2, Underlined = 4 };

	Text() : m_font(nullptr), m_size(30), m_fill(Color::White),
	         m_outline(Color::Black) {}
	Text(const String& string, const Font& font, unsigned int characterSize = 30)
		: m_string(string), m_font(&font), m_size(characterSize),
		  m_fill(Color::White), m_outline(Color::Black) {}

	void setString(const String& s) { m_string = s; }
	const String& getString() const { return m_string; }

	void setFont(const Font& font) { m_font = &font; }
	void setCharacterSize(unsigned int s) { m_size = s; }
	unsigned int getCharacterSize() const { return m_size; }

	void setFillColor(const Color& c) { m_fill = c; }
	void setOutlineColor(const Color& c) { m_outline = c; }
	void setColor(const Color& c) { m_fill = c; m_outline = c; }

	FloatRect getLocalBounds() const;
	FloatRect getGlobalBounds() const;

	void draw(RenderTarget& target, const RenderStates& states) const override;

private:
	String m_string;
	const Font* m_font;
	unsigned int m_size;
	Color m_fill, m_outline;
};

class VertexArray : public Drawable {
public:
	VertexArray() : m_type(Points) {}
	VertexArray(PrimitiveType type, std::size_t count = 0)
		: m_vertices(count), m_type(type) {}

	Vertex& operator[](std::size_t i) { return m_vertices[i]; }
	const Vertex& operator[](std::size_t i) const { return m_vertices[i]; }

	std::size_t getVertexCount() const { return m_vertices.size(); }
	void resize(std::size_t n) { m_vertices.resize(n); }
	void append(const Vertex& v) { m_vertices.push_back(v); }
	void clear() { m_vertices.clear(); }
	void setPrimitiveType(PrimitiveType t) { m_type = t; }

	void draw(RenderTarget& target, const RenderStates& states) const override;

private:
	std::vector<Vertex> m_vertices;
	PrimitiveType m_type;
};

// --------------------------------------------------------------------
//	Render targets
// --------------------------------------------------------------------

class RenderTarget {
public:
	virtual ~RenderTarget() {}

	void clear(const Color& color = Color(0, 0, 0, 255));
	void draw(const Drawable& drawable,
	          const RenderStates& states = RenderStates::Default);
	void draw(const Vertex* vertices, std::size_t count, PrimitiveType type,
	          const RenderStates& states = RenderStates::Default);

	virtual Vector2u getSize() const = 0;

	/** Saves the GL state the game set up and installs our 2D pipeline;
	 *  popGLStates() puts it back. The game brackets every text draw with
	 *  these, so they are the seam between its 3D rendering and ours. */
	void pushGLStates();
	void popGLStates();
	void resetGLStates();

protected:
	/** Makes this target's framebuffer current before drawing. */
	virtual void bindTarget() = 0;

	bool m_states_pushed = false;
};

class RenderWindow : public Window, public RenderTarget {
public:
	RenderWindow() {}
	RenderWindow(VideoMode mode, const String& title,
	             Uint32 style = Style::Default,
	             const ContextSettings& settings = ContextSettings());

	Vector2u getSize() const override { return Window::getSize(); }

protected:
	void bindTarget() override;
};

/** Off-screen target. Used by the credits screen, and by the VR HUD panel
 *  which renders the 2D overlay once and shows it on a quad in both eyes. */
class RenderTexture : public RenderTarget {
public:
	RenderTexture();
	~RenderTexture();

	bool create(unsigned int width, unsigned int height,
	            bool depthBuffer = false);
	void display();

	const Texture& getTexture() const { return m_texture; }
	Vector2u getSize() const override { return m_texture.getSize(); }

	void setSmooth(bool smooth) { m_texture.setSmooth(smooth); }
	void setRepeated(bool repeated) { m_texture.setRepeated(repeated); }

protected:
	void bindTarget() override;

private:
	unsigned int m_fbo;
	unsigned int m_depth_rb;
	Texture m_texture;
};

}  // namespace sf

#endif  // ETR_SFML_GRAPHICS_HPP
