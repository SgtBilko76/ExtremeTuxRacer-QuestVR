/* --------------------------------------------------------------------
EXTREME TUXRACER

Copyright (C) 1999-2001 Jasmin F. Patry (Tuxracer)
Copyright (C) 2010 Extreme Tuxracer Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
---------------------------------------------------------------------*/

#ifndef RACING_H
#define RACING_H

#include "bh.h"
#include "states.h"

class CRacing : public State {
	void Enter();
	void Loop(float time_step);

	// Racing is the one screen drawn in true stereo: simulation runs once
	// per frame, drawing runs once per eye.
	bool SupportsStereo() const override { return true; }
	void Update(float time_step) override;
	void Render(int eye) override;
	void Keyb(sf::Keyboard::Key key, bool release, int x, int y);
	void Jaxis(int axis, float value);
	void Jbutt(int button, bool pressed);
	void Exit();
public:
};

extern CRacing Racing;

#endif
