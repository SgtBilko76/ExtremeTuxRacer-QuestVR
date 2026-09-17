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

#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "states.h"
#ifdef ETR_ANDROID
#include "vr/vr.h"
#include <android/log.h>
#endif
#include "ogl.h"
#include "winsys.h"

State::Manager State::manager(Winsys);

State::Manager::~Manager() {
	if (current)
		current->Exit();
}

void State::Manager::Run(State& entranceState) {
	current = &entranceState;
	current->Enter();
	while (!quit) {
		PollEvent();
		if (next)
			EnterNextState();
		CallLoopFunction();
	}
	current->Exit();
	previous = current;
	current = nullptr;
}

void State::Manager::EnterNextState() {
	current->Exit();
	previous = current;
	current = next;
	next = nullptr;
	current->Enter();
}

void State::Manager::PollEvent() {
	sf::Event event;
	sf::Keyboard::Key key;

	while (Winsys.PollEvent(event)) {
		if (!next) {
			switch (event.type) {
				case sf::Event::KeyPressed:
					key = event.key.code;
					current->Keyb(key, false, sf::Mouse::getPosition().x, sf::Mouse::getPosition().y);
					break;

				case sf::Event::KeyReleased:
					key = event.key.code;
					current->Keyb(key, true, sf::Mouse::getPosition().x, sf::Mouse::getPosition().y);
					break;

				case sf::Event::TextEntered:
					current->TextEntered(static_cast<char>(event.text.unicode));
					break;

				case sf::Event::MouseButtonPressed:
				case sf::Event::MouseButtonReleased:
					current->Mouse(event.mouseButton.button, event.type == sf::Event::MouseButtonPressed, event.mouseButton.x, event.mouseButton.y);
					break;

				case sf::Event::MouseMoved: {
					TVector2i old = cursor_pos;
					cursor_pos.x = event.mouseMove.x;
					cursor_pos.y = event.mouseMove.y;
					current->Motion(event.mouseMove.x - old.x, event.mouseMove.y - old.y);
					break;
				}

				case sf::Event::JoystickMoved: {
					float val = event.joystickMove.position / 100.f;
					current->Jaxis(event.joystickMove.axis == sf::Joystick::X ? 0 : 1, val);
					break;
				}
				case sf::Event::JoystickButtonPressed:
				case sf::Event::JoystickButtonReleased:
					current->Jbutt(event.joystickButton.button, event.type == sf::Event::JoystickButtonPressed);
					break;

				case sf::Event::Resized:
					if (Winsys.resolution.width != event.size.width || Winsys.resolution.height != event.size.height) {
						Winsys.resolution.width = event.size.width;
						Winsys.resolution.height = event.size.height;
						Winsys.SetupVideoMode(event.size.width, event.size.height);
					}
					break;

				case sf::Event::Closed:
					quit = true;
					break;

				default:
					break;
			}
		}
	}
}

void State::Manager::CallLoopFunction() {
	check_gl_error();

	g_game.time_step = std::max(0.0001f, timer.getElapsedTime().asSeconds());
	timer.restart();

#ifdef ETR_ANDROID
	if (vr::IsActive()) {
		VRLoop(g_game.time_step);
		return;
	}
#endif

	current->Loop(g_game.time_step);
}

#ifdef ETR_ANDROID
void State::Manager::VRLoop(float time_step) {
	if (vr::IsExitRequested()) {
		quit = true;
		return;
	}

	// Menu screens get d-pad style navigation from the thumbstick; the
	// race gets steering. This has to be decided before BeginFrame, which
	// is what syncs the controller state.
	vr::SetUiNavigation(!current->SupportsStereo());

	// BeginFrame also pumps XR events and syncs controller input, so it
	// still has to run on the ticks where the runtime tells us not to
	// draw (headset off the head, system menu up, ...).
	if (!vr::BeginFrame()) {
		vr::EndFrame();
		return;
	}

	// A heartbeat, so the log distinguishes "rendering happily" from
	// "stalled somewhere" without needing eyes on the headset.
	static int frame_count = 0;
	if (++frame_count % 180 == 1) {
		__android_log_print(ANDROID_LOG_INFO, "ETRVR",
		                    "frame %d, %s path, dt=%.1f ms", frame_count,
		                    current->SupportsStereo() ? "stereo" : "screen",
		                    time_step * 1000.f);
	}

	if (current->SupportsStereo()) {
		// Simulation runs once for the frame, drawing runs once per eye.
		current->Update(time_step);
		for (int eye = 0; eye < vr::NumEyes(); eye++) {
			vr::BeginEye(eye);
			current->Render(eye);
			vr::EndEye(eye);
		}
	} else {
		// Menu screens draw themselves monoscopically into the quad-layer
		// swapchain, unchanged.
		if (vr::BeginScreen()) {
			current->Loop(time_step);
			vr::EndScreen();
		}
	}

	vr::EndFrame();
}
#endif
