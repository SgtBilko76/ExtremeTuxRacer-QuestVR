/* Single translation unit for the stb libraries that need one.
   stb_image and stb_truetype are compiled inside sfml_graphics.cpp; only
   the Vorbis decoder needs its own, because sfml_audio.cpp includes it
   header-only. */
#include "stb_vorbis.c"
