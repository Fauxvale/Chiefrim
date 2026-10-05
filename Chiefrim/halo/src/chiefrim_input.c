/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_INPUT.C

Chief's controls from Skyrim (Chiefrim/docs/DESIGN.md §7). The SKSE plugin
publishes actions in the link's input slot: Skyrim's ControlMap has already
turned the player's keys, mouse and gamepad into user events, so rebinding a
Skyrim action rebinds Chief's with it. This file hands those actions to the
port's own keyboard and mouse paths:

- chiefrim_input_keyboard_actions: the actions as the port's keyboard action
  bits (halo_keyboard.h), ORed into what the keyboard holds; the port turns
  them into the game controls' held ticks, reload and so on as for keys
  (input_abstraction.c, keyboard_controls_update);
- chiefrim_input_movement: forward and strafe, analog;
- chiefrim_input_look: the turn since the last call, in radians, as direct
  aim like the port's mouse (player_control.c).

Only local player 0, and only while the link is up and Skyrim routes input
to Chief. A press shorter than one Halo frame still counts once: the slot
carries a press counter per action.
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cseries/cseries_windows.h"
#include "halo_keyboard.h"

/* ---------- globals */

static struct
{
	uint8_t presses[CR_ACTION_SLOTS];
	boolean presses_valid;
	uint32_t look_session;
	double yaw_total;
	double pitch_total;
	boolean look_valid;
	uint32_t last_frame;
	unsigned long last_frame_change;
} chiefrim_input;

/* Skyrim publishes its input once a frame. Older than this, it stopped
(a menu, a loading screen, a stall): Chief gets none rather than the last
held keys. */
#define CHIEFRIM_INPUT_STALE_MS 150

/* ---------- private code */

/* The latest input, if Chief should take it now. */
static boolean chiefrim_input_read(short controller_index, cr_input *input)
{
	cr_shared *shm = chiefrim_shared();

	if (controller_index != 0 || !shm || !chiefrim_linked())
		return FALSE;
	if (!CR_SLOT_READ(&shm->input, input))
		return FALSE;
	{
		unsigned long now = system_milliseconds();

		if (input->frame != chiefrim_input.last_frame)
		{
			chiefrim_input.last_frame = input->frame;
			chiefrim_input.last_frame_change = now;
		}
		else if (now - chiefrim_input.last_frame_change > CHIEFRIM_INPUT_STALE_MS)
		{
			return FALSE;
		}
	}
	return input->routing == CR_ROUTE_HALO;
}

static unsigned long chiefrim_keyboard_bit(unsigned long action)
{
	switch (action)
	{
	case CR_ACTION_JUMP:           return FLAG(HALO_KEYBOARD_JUMP);
	case CR_ACTION_CROUCH:         return FLAG(HALO_KEYBOARD_CROUCH);
	case CR_ACTION_FIRE:           return FLAG(HALO_KEYBOARD_FIRE);
	case CR_ACTION_ZOOM:           return FLAG(HALO_KEYBOARD_ZOOM);
	case CR_ACTION_RELOAD:         return FLAG(HALO_KEYBOARD_RELOAD);
	case CR_ACTION_GRENADE:        return FLAG(HALO_KEYBOARD_THROW_GRENADE);
	case CR_ACTION_MELEE:          return FLAG(HALO_KEYBOARD_MELEE);
	case CR_ACTION_ACTION:         return FLAG(HALO_KEYBOARD_ACTION);
	case CR_ACTION_SWITCH_WEAPON:  return FLAG(HALO_KEYBOARD_SWITCH_WEAPON);
	case CR_ACTION_SWITCH_GRENADE: return FLAG(HALO_KEYBOARD_SWITCH_GRENADE);
	case CR_ACTION_FLASHLIGHT:     return FLAG(HALO_KEYBOARD_FLASHLIGHT);
	default:                       return 0;
	}
}

/* ---------- public code */

/* TRUE once for each press of Chiefrim's "mark stuck" hotkey. */
boolean chiefrim_input_mark(void)
{
	static boolean valid = FALSE;
	static uint8_t last;
	cr_input input;
	boolean pressed;

	if (!chiefrim_input_read(0, &input))
		return FALSE;
	pressed = valid && input.presses[CR_ACTION_MARK] != last;
	last = input.presses[CR_ACTION_MARK];
	valid = TRUE;
	return pressed;
}

unsigned long chiefrim_input_keyboard_actions(short controller_index)
{
	cr_input input;
	unsigned long held = 0;
	unsigned long action;

	if (!chiefrim_input_read(controller_index, &input))
	{
		chiefrim_input.presses_valid = FALSE;
		return 0;
	}

	for (action = 0; action < CR_ACTION_COUNT; action++)
	{
		boolean down = TEST_FLAG(input.held, action);

		/* pressed and let go again since the last frame: down for this one */
		if (chiefrim_input.presses_valid && input.presses[action] != chiefrim_input.presses[action])
			down = TRUE;
		if (down)
			held |= chiefrim_keyboard_bit(action);
	}
	memcpy(chiefrim_input.presses, input.presses, sizeof(chiefrim_input.presses));
	chiefrim_input.presses_valid = TRUE;
	return held;
}

boolean chiefrim_input_movement(short controller_index, real *forward, real *strafe)
{
	cr_input input;

	if (!chiefrim_input_read(controller_index, &input))
		return FALSE;
	if (input.forward == 0.0f && input.strafe == 0.0f)
		return FALSE;
	*forward = PIN(input.forward, -1.f, 1.f);
	*strafe = -PIN(input.strafe, -1.f, 1.f); /* Halo's strafe is +left */
	return TRUE;
}

boolean chiefrim_input_look(short gamepad_index, real *yaw, real *pitch)
{
	cr_input input;
	double yaw_delta;
	double pitch_delta;

	*yaw = 0.f;
	*pitch = 0.f;
	if (!chiefrim_input_read(gamepad_index, &input))
	{
		chiefrim_input.look_valid = FALSE;
		return FALSE;
	}
	if (!chiefrim_input.look_valid || input.session != chiefrim_input.look_session)
	{
		/* a new baseline: never turn by what happened before it */
		chiefrim_input.look_session = input.session;
		chiefrim_input.yaw_total = input.yaw_total;
		chiefrim_input.pitch_total = input.pitch_total;
		chiefrim_input.look_valid = TRUE;
		return FALSE;
	}

	yaw_delta = input.yaw_total - chiefrim_input.yaw_total;
	pitch_delta = input.pitch_total - chiefrim_input.pitch_total;
	chiefrim_input.yaw_total = input.yaw_total;
	chiefrim_input.pitch_total = input.pitch_total;
	if (yaw_delta == 0.0 && pitch_delta == 0.0)
		return FALSE;

	*yaw = (real)-yaw_delta;    /* Halo's yaw is +left */
	*pitch = (real)pitch_delta; /* both +up */
	return TRUE;
}

boolean chiefrim_input_driving(short gamepad_index)
{
	cr_input input;

	return chiefrim_input_read(gamepad_index, &input);
}
