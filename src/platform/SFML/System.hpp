/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer for Android

Extreme Tux Racer is written against SFML 2.4, which has no usable Meta
Quest target. Rather than rewrite ~35 files' worth of call sites, this
directory reimplements exactly the slice of the SFML API the game touches,
on top of SDL2 + GLES 3 + stb.

Because these headers live at src/platform/SFML/, the game's existing
  #include <SFML/System.hpp>
lines resolve here on Android with no source changes at all.

This header covers the System module: vectors, the unicode string type,
and the clock.
---------------------------------------------------------------------*/

#ifndef ETR_SFML_SYSTEM_HPP
#define ETR_SFML_SYSTEM_HPP

#include <cstdint>
#include <cstddef>
#include <string>

namespace sf {

typedef std::uint8_t  Uint8;
typedef std::uint16_t Uint16;
typedef std::uint32_t Uint32;
typedef std::uint64_t Uint64;
typedef std::int8_t   Int8;
typedef std::int16_t  Int16;
typedef std::int32_t  Int32;
typedef std::int64_t  Int64;

// --------------------------------------------------------------------
//	Vectors
// --------------------------------------------------------------------

template <typename T>
class Vector2 {
public:
	T x, y;

	Vector2() : x(0), y(0) {}
	Vector2(T x_, T y_) : x(x_), y(y_) {}

	template <typename U>
	explicit Vector2(const Vector2<U>& v)
		: x(static_cast<T>(v.x)), y(static_cast<T>(v.y)) {}

	Vector2 operator+(const Vector2& r) const { return Vector2(x + r.x, y + r.y); }
	Vector2 operator-(const Vector2& r) const { return Vector2(x - r.x, y - r.y); }
	Vector2 operator*(T s) const { return Vector2(x * s, y * s); }
	Vector2& operator+=(const Vector2& r) { x += r.x; y += r.y; return *this; }
	Vector2& operator-=(const Vector2& r) { x -= r.x; y -= r.y; return *this; }
	bool operator==(const Vector2& r) const { return x == r.x && y == r.y; }
	bool operator!=(const Vector2& r) const { return !(*this == r); }
};

template <typename T>
class Vector3 {
public:
	T x, y, z;

	Vector3() : x(0), y(0), z(0) {}
	Vector3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}

	Vector3 operator+(const Vector3& r) const { return Vector3(x + r.x, y + r.y, z + r.z); }
	Vector3 operator-(const Vector3& r) const { return Vector3(x - r.x, y - r.y, z - r.z); }
	Vector3 operator*(T s) const { return Vector3(x * s, y * s, z * s); }
	bool operator==(const Vector3& r) const { return x == r.x && y == r.y && z == r.z; }
	bool operator!=(const Vector3& r) const { return !(*this == r); }
};

typedef Vector2<int>          Vector2i;
typedef Vector2<unsigned int> Vector2u;
typedef Vector2<float>        Vector2f;
typedef Vector3<int>          Vector3i;
typedef Vector3<float>        Vector3f;

// --------------------------------------------------------------------
//	String
// --------------------------------------------------------------------

/** UTF-32 string, matching sf::String's semantics closely enough for the
 *  game's menus and translations. Conversions to and from std::string go
 *  through UTF-8, which is what the translation files are encoded in. */
class String {
public:
	static const std::size_t InvalidPos = static_cast<std::size_t>(-1);

	String() {}
	String(char c) { m_data.push_back(static_cast<char32_t>(c)); }
	String(char32_t c) { m_data.push_back(c); }
	String(const char* s) { fromUtf8(s ? s : ""); }
	String(const std::string& s) { fromUtf8(s); }
	String(const std::u32string& s) : m_data(s) {}
	String(const wchar_t* s) { fromWide(s ? std::wstring(s) : std::wstring()); }
	String(const std::wstring& s) { fromWide(s); }

	// SFML's String converts implicitly to the standard string types, and
	// the game leans on that: translation entries are sf::String but are
	// passed straight into functions taking std::string.
	operator std::string() const { return toUtf8(); }
	operator std::wstring() const { return toWide(); }

	std::size_t getSize() const { return m_data.size(); }
	bool isEmpty() const { return m_data.empty(); }
	void clear() { m_data.clear(); }

	char32_t operator[](std::size_t i) const { return m_data[i]; }
	char32_t& operator[](std::size_t i) { return m_data[i]; }

	void insert(std::size_t pos, const String& s) {
		if (pos > m_data.size()) pos = m_data.size();
		m_data.insert(pos, s.m_data);
	}
	void erase(std::size_t pos, std::size_t count = 1) {
		if (pos >= m_data.size()) return;
		m_data.erase(pos, count);
	}
	String substring(std::size_t pos, std::size_t count = InvalidPos) const {
		if (pos >= m_data.size()) return String();
		return String(m_data.substr(pos, count));
	}
	std::size_t find(const String& s, std::size_t start = 0) const {
		std::size_t r = m_data.find(s.m_data, start);
		return r == std::u32string::npos ? InvalidPos : r;
	}

	std::string toAnsiString() const { return toUtf8(); }
	std::string toUtf8() const;
	std::wstring toWide() const;

	const std::u32string& toUtf32() const { return m_data; }

	String& operator+=(const String& r) { m_data += r.m_data; return *this; }

	friend String operator+(const String& a, const String& b) {
		String r(a);
		r += b;
		return r;
	}
	friend bool operator==(const String& a, const String& b) {
		return a.m_data == b.m_data;
	}
	friend bool operator!=(const String& a, const String& b) {
		return !(a == b);
	}
	friend bool operator<(const String& a, const String& b) {
		return a.m_data < b.m_data;
	}

private:
	void fromUtf8(const std::string& s);
	void fromWide(const std::wstring& s);

	std::u32string m_data;
};

// --------------------------------------------------------------------
//	Time / Clock
// --------------------------------------------------------------------

class Time {
public:
	Time() : m_us(0) {}
	explicit Time(Int64 microseconds) : m_us(microseconds) {}

	float asSeconds() const { return static_cast<float>(m_us) / 1000000.f; }
	Int32 asMilliseconds() const { return static_cast<Int32>(m_us / 1000); }
	Int64 asMicroseconds() const { return m_us; }

private:
	Int64 m_us;
};

inline Time seconds(float s) {
	return Time(static_cast<Int64>(s * 1000000.f));
}
inline Time milliseconds(Int32 ms) {
	return Time(static_cast<Int64>(ms) * 1000);
}

class Clock {
public:
	Clock();
	Time getElapsedTime() const;
	Time restart();

private:
	Uint64 m_start;
};

/** Suspends the calling thread. Only used by the game's loading screens. */
void sleep(Time duration);

}  // namespace sf

#endif  // ETR_SFML_SYSTEM_HPP
