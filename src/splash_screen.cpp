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

#include "splash_screen.h"
#include "ogl.h"
#include "textures.h"
#include "audio.h"
#include "gui.h"
#include "course.h"
#include "env.h"
#include "particles.h"
#include "font.h"
#include "game_ctrl.h"
#include "translation.h"
#include "score.h"
#include "regist.h"
#include "winsys.h"

#ifdef ETR_ANDROID
#include "loading.h"

namespace {

/** Picks a default player, character and course so the headset build can
 *  go straight into a race. The menus are drawn as a flat panel in VR but
 *  are not navigable yet -- there is no pointer to click them with -- so
 *  until that lands (see the VR menu milestone) this is how the game is
 *  reachable at all.
 *
 *  Called only once every resource list has been loaded, so all of these
 *  collections are already populated. */
bool SetupDirectRace() {
	if (Players.numPlayers() == 0) return false;

	// The same sequence QuitRegistration() performs in regist.cpp. Without
	// AllocControl the player has no CControl, and the course loader then
	// dereferences a null ctrl->viewpos.
	Players.ResetControls();
	Players.AllocControl(0);

	g_game.player = Players.GetPlayer(0);
	g_game.start_player = 0;
	if (g_game.player->ctrl == nullptr) return false;

	if (Char.CharList.empty()) return false;
	g_game.character = &Char.CharList[0];
	Char.FreeCharacterPreviews();

	if (!Course.currentCourseList || Course.currentCourseList->size() == 0)
		return false;
	g_game.course = &(*Course.currentCourseList)[0];
	g_game.theme_id = (*Course.currentCourseList)[0].music_theme;

	g_game.mirrorred = false;
	g_game.game_type = PRACTICING;
	return true;
}

}  // namespace
#endif

CSplashScreen SplashScreen;
sf::Text* Failure = nullptr;
sf::String reason;


void CSplashScreen::Enter() {
	Winsys.ShowCursor(!param.ice_cursor);
	Music.Play(param.menu_music, true);
}

void CSplashScreen::Loop(float timestep) {
	ScopedRenderMode rm(GUI);
	Winsys.clear();
	Trans.LoadTranslations(param.language);  // Before first texts are being displayed

	sf::Sprite logo(Tex.GetSFTexture(TEXLOGO));
	logo.setScale(Winsys.scale/2.f, Winsys.scale/2.f);
	logo.setPosition((Winsys.resolution.width - logo.getTextureRect().width*(Winsys.scale / 2)) / 2, 60);

	if (!Failure) {
		FT.AutoSizeN(6);
		sf::Text t1(Trans.Text(67), FT.getCurrentFont(), FT.GetSize());
		int top = AutoYPosN(60);
		t1.setPosition((Winsys.resolution.width - t1.getLocalBounds().width) / 2, top);
		sf::Text t2(Trans.Text(68), FT.getCurrentFont(), FT.GetSize());
		int dist = FT.AutoDistanceN(3);
		t2.setPosition((Winsys.resolution.width - t2.getLocalBounds().width) / 2, top + dist);

		Winsys.draw(t1);
		Winsys.draw(t2);
	} else {
		Winsys.draw(*Failure);
	}
	Winsys.draw(logo);
	Winsys.SwapBuffers();

	if (!Failure) {
		init_ui_snow();

		Course.MakeStandardPolyhedrons();
		Sound.LoadSoundList();
		if (!Char.LoadCharacterList())
			reason += Trans.Text(93) + "\n";
		Course.LoadObjectTypes();
		if (!Course.LoadTerrainTypes())
			reason += Trans.Text(95) + "\n";
		if (Env.LoadEnvironmentList()) {
			if (Course.LoadCourseList()) {
				Score.LoadHighScore();  // after LoadCourseList !!!
				Events.LoadEventList();

				if (Players.LoadAvatars()) {  // before LoadPlayers !!!
					Players.LoadPlayers();
				} else
					reason += Trans.Text(96) + "\n";
			} else
				reason += Trans.Text(92) + "\n";
		} else
			reason += Trans.Text(94) + "\n";

		if (reason.isEmpty())
#ifdef ETR_ANDROID
			if (SetupDirectRace())
				State::manager.RequestEnterState(Loading);
			else
				State::manager.RequestEnterState(Regist);
#else
			State::manager.RequestEnterState(Regist);
#endif
		else { // Failure
			FT.AutoSizeN(6);
			int top = AutoYPosN(60);
			Failure = new sf::Text(reason, FT.getCurrentFont(), FT.GetSize());
			Failure->setFillColor(colDRed);
                        Failure->setOutlineColor(colDRed);
			Failure->setPosition((Winsys.resolution.width - Failure->getLocalBounds().width) / 2, top);
		}
	}
}
