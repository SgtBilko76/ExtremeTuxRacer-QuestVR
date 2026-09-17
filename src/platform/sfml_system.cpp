/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer: System module
---------------------------------------------------------------------*/

#include "SFML/System.hpp"

#include <ctime>
#include <unistd.h>

namespace sf {

// --------------------------------------------------------------------
//	String
// --------------------------------------------------------------------

void String::fromUtf8(const std::string& s) {
	m_data.clear();
	m_data.reserve(s.size());

	std::size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		char32_t cp;
		int extra;

		if (c < 0x80)        { cp = c;        extra = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
		else {
			// Invalid lead byte: emit a replacement char and resynchronise
			// rather than dropping the rest of the string.
			m_data.push_back(0xFFFD);
			i++;
			continue;
		}

		if (i + extra >= s.size()) {
			m_data.push_back(0xFFFD);
			break;
		}
		for (int k = 1; k <= extra; k++) {
			const unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; break; }
			cp = (cp << 6) | (cc & 0x3F);
		}
		m_data.push_back(cp);
		i += extra + 1;
	}
}

std::string String::toUtf8() const {
	std::string out;
	out.reserve(m_data.size());

	for (char32_t cp : m_data) {
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

void String::fromWide(const std::wstring& s) {
	// wchar_t is 32-bit on Android, so this is a straight widening copy.
	m_data.assign(s.begin(), s.end());
}

std::wstring String::toWide() const {
	return std::wstring(m_data.begin(), m_data.end());
}

// --------------------------------------------------------------------
//	Clock
// --------------------------------------------------------------------

namespace {
Uint64 nowMicroseconds() {
	// CLOCK_MONOTONIC rather than the wall clock, so the frame timer cannot
	// jump when the system time is adjusted.
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<Uint64>(ts.tv_sec) * 1000000ULL +
	       static_cast<Uint64>(ts.tv_nsec) / 1000ULL;
}
}  // namespace

Clock::Clock() : m_start(nowMicroseconds()) {}

Time Clock::getElapsedTime() const {
	return Time(static_cast<Int64>(nowMicroseconds() - m_start));
}

Time Clock::restart() {
	const Uint64 now = nowMicroseconds();
	const Time elapsed(static_cast<Int64>(now - m_start));
	m_start = now;
	return elapsed;
}

void sleep(Time duration) {
	const Int64 us = duration.asMicroseconds();
	if (us > 0) usleep(static_cast<useconds_t>(us));
}

}  // namespace sf
