/*
** i_joystick.cpp
**
**---------------------------------------------------------------------------
** Copyright 2005-2016 Randy Heit
** Copyright 2026 STEK fork contributors
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** Devices recognized by the SDL GameController mapping database are exposed
** with XInput-style semantics: stable Pad_A, LTrigger, DPad and similar keys,
** analog sticks mapped by function (not raw axis index), and triggers that
** rest at zero regardless of driver quirks. Everything else falls back to
** the legacy raw-joystick path. Config sections are keyed by the device
** GUID, not the volatile enumeration index, and devices hotplug at runtime.
*/

#include <SDL2/SDL.h>
#include <cstdint>
#include <cstdlib>

#include "basics.h"
#include "cmdlib.h"

#include "m_joy.h"
#include "keydef.h"
#include "d_eventbase.h"
#include "printf.h"

#define DEFAULT_DEADZONE 0.25f;

// Very small deadzone so that floating point magic doesn't happen
#define MIN_DEADZONE 0.000001f

void I_UpdateJoystickDevices();

CUSTOM_CVAR(Bool, joy_sdl_gamecontroller, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
{
	I_UpdateJoystickDevices();
}

static FString JoyGUIDString(int deviceindex)
{
	char guid[64];
	SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(deviceindex), guid, sizeof(guid));
	return guid;
}

// Identifier is "<prefix>:<guid>" plus an ordinal suffix when several
// identical devices are attached, so config sections survive re-plugs
// and enumeration order changes.
static FString JoyIdentifier(const char *prefix, const FString &guid, int ordinal)
{
	FString id;
	if (ordinal == 0)
		id.Format("%s:%s", prefix, guid.GetChars());
	else
		id.Format("%s:%s#%d", prefix, guid.GetChars(), ordinal + 1);
	return id;
}

//==========================================================================
//
// SDLInputGameController
//
// A device with a GameController mapping. Mirrors the win32 XInput backend:
// six functional axes, sixteen-bit button mask keyed at KEY_PAD_DPAD_UP.
//
//==========================================================================

class SDLInputGameController: public IJoystickConfig
{
public:
	enum
	{
		AXIS_ThumbLX,
		AXIS_ThumbLY,
		AXIS_ThumbRX,
		AXIS_ThumbRY,
		AXIS_LeftTrigger,
		AXIS_RightTrigger,
		NUM_AXES
	};

	SDLInputGameController(int DeviceIndex, int Ordinal)
		: Multiplier(1.0f), Enabled(true), LastButtons(0)
	{
		Guid = JoyGUIDString(DeviceIndex);
		OrdinalInGuid = Ordinal;
		Device = SDL_GameControllerOpen(DeviceIndex);
		if (Device != NULL)
		{
			const char *name = SDL_GameControllerName(Device);
			Name = name != NULL ? name : "Game Controller";
			InstanceID = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(Device));
			SetDefaultConfig();
			M_LoadJoystickConfig(this);
		}
	}
	~SDLInputGameController()
	{
		if (Device != NULL)
		{
			// Release anything still held before the device goes away.
			ProcessThumbstick(0, &Axes[AXIS_ThumbLX], 0, &Axes[AXIS_ThumbLY], KEY_PAD_LTHUMB_RIGHT);
			ProcessThumbstick(0, &Axes[AXIS_ThumbRX], 0, &Axes[AXIS_ThumbRY], KEY_PAD_RTHUMB_RIGHT);
			ProcessTrigger(0, &Axes[AXIS_LeftTrigger], KEY_PAD_LTRIGGER);
			ProcessTrigger(0, &Axes[AXIS_RightTrigger], KEY_PAD_RTRIGGER);
			Joy_GenerateButtonEvents(LastButtons, 0, 16, KEY_PAD_DPAD_UP);
			M_SaveJoystickConfig(this);
			SDL_GameControllerClose(Device);
		}
	}

	bool IsValid() const
	{
		return Device != NULL;
	}

	FString GetName()
	{
		return Name;
	}
	float GetSensitivity()
	{
		return Multiplier;
	}
	void SetSensitivity(float scale)
	{
		Multiplier = scale;
	}

	int GetNumAxes()
	{
		return NUM_AXES;
	}
	float GetAxisDeadZone(int axis)
	{
		return unsigned(axis) < NUM_AXES ? Axes[axis].DeadZone : 0.f;
	}
	EJoyAxis GetAxisMap(int axis)
	{
		return unsigned(axis) < NUM_AXES ? Axes[axis].GameAxis : JOYAXIS_None;
	}
	const char *GetAxisName(int axis)
	{
		return unsigned(axis) < NUM_AXES ? AxisNames[axis] : "Invalid";
	}
	float GetAxisScale(int axis)
	{
		return unsigned(axis) < NUM_AXES ? Axes[axis].Multiplier : 0.f;
	}

	void SetAxisDeadZone(int axis, float zone)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			Axes[axis].DeadZone = clamp(zone, MIN_DEADZONE, 1.f);
		}
	}
	void SetAxisMap(int axis, EJoyAxis gameaxis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			Axes[axis].GameAxis = (unsigned(gameaxis) < NUM_JOYAXIS) ? gameaxis : JOYAXIS_None;
		}
	}
	void SetAxisScale(int axis, float scale)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			Axes[axis].Multiplier = scale;
		}
	}

	bool IsSensitivityDefault()
	{
		return Multiplier == 1.0f;
	}
	bool IsAxisDeadZoneDefault(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].DeadZone == DefaultAxes[axis].DeadZone;
		}
		return true;
	}
	bool IsAxisMapDefault(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].GameAxis == DefaultAxes[axis].GameAxis;
		}
		return true;
	}
	bool IsAxisScaleDefault(int axis)
	{
		if (unsigned(axis) < NUM_AXES)
		{
			return Axes[axis].Multiplier == DefaultAxes[axis].Multiplier;
		}
		return true;
	}

	void SetDefaultConfig()
	{
		Multiplier = 1.0f;
		for (int i = 0; i < NUM_AXES; ++i)
		{
			Axes[i].DeadZone = DefaultAxes[i].DeadZone;
			Axes[i].GameAxis = DefaultAxes[i].GameAxis;
			Axes[i].Multiplier = DefaultAxes[i].Multiplier;
			Axes[i].Value = 0;
			Axes[i].ButtonValue = 0;
		}
	}

	bool GetEnabled()
	{
		return Enabled;
	}
	void SetEnabled(bool enabled)
	{
		Enabled = enabled;
	}

	bool AllowsEnabledInBackground() { return false; }
	bool GetEnabledInBackground() { return false; }
	void SetEnabledInBackground(bool enabled) {}

	FString GetIdentifier()
	{
		return JoyIdentifier("GC", Guid, OrdinalInGuid);
	}

	SDL_JoystickID GetInstanceID() const
	{
		return InstanceID;
	}

	void AddAxes(float axes[NUM_JOYAXIS])
	{
		for (int i = 0; i < NUM_AXES; ++i)
		{
			if (Axes[i].GameAxis != JOYAXIS_None)
				axes[Axes[i].GameAxis] -= float(Axes[i].Value * Multiplier * Axes[i].Multiplier);
		}
	}

	void ProcessInput()
	{
		// Sticks: analog into the mapped game axes plus digital
		// direction events for menu navigation.
		ProcessThumbstick(SDL_GameControllerGetAxis(Device, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0,
			&Axes[AXIS_ThumbLX],
			SDL_GameControllerGetAxis(Device, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0,
			&Axes[AXIS_ThumbLY], KEY_PAD_LTHUMB_RIGHT);
		ProcessThumbstick(SDL_GameControllerGetAxis(Device, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0,
			&Axes[AXIS_ThumbRX],
			SDL_GameControllerGetAxis(Device, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0,
			&Axes[AXIS_ThumbRY], KEY_PAD_RTHUMB_RIGHT);

		// Triggers: the GameController API guarantees these rest at zero,
		// which is the root fix for phantom look-up/fire from idle pads.
		ProcessTrigger(SDL_GameControllerGetAxis(Device, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0,
			&Axes[AXIS_LeftTrigger], KEY_PAD_LTRIGGER);
		ProcessTrigger(SDL_GameControllerGetAxis(Device, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0,
			&Axes[AXIS_RightTrigger], KEY_PAD_RTRIGGER);

		// Buttons, in XInput bit order so the shared KEY_PAD_* block lines up.
		int buttons = 0;
		for (const auto &bm : ButtonMap)
		{
			if (SDL_GameControllerGetButton(Device, bm.Button))
				buttons |= 1 << bm.Bit;
		}
		Joy_GenerateButtonEvents(LastButtons, buttons, 16, KEY_PAD_DPAD_UP);
		LastButtons = buttons;
	}

protected:
	struct AxisInfo
	{
		float DeadZone;
		float Multiplier;
		EJoyAxis GameAxis;
		double Value;
		uint8_t ButtonValue;
	};
	struct DefaultAxisConfig
	{
		float DeadZone;
		EJoyAxis GameAxis;
		float Multiplier;
	};
	struct ButtonMapping
	{
		SDL_GameControllerButton Button;
		int Bit;
	};

	static void ProcessThumbstick(double value1, AxisInfo *axis1, double value2, AxisInfo *axis2, int base)
	{
		uint8_t buttonstate;

		value1 = Joy_RemoveDeadZone(value1, axis1->DeadZone, NULL);
		value2 = Joy_RemoveDeadZone(value2, axis2->DeadZone, NULL);
		axis1->Value = value1;
		axis2->Value = value2;

		// All four direction buttons are stored on the first axis.
		buttonstate = Joy_XYAxesToButtons(value1, value2);
		Joy_GenerateButtonEvents(axis1->ButtonValue, buttonstate, 4, base);
		axis1->ButtonValue = buttonstate;
	}

	static void ProcessTrigger(double value, AxisInfo *axis, int base)
	{
		uint8_t buttonstate;

		value = Joy_RemoveDeadZone(value, axis->DeadZone, &buttonstate);
		Joy_GenerateButtonEvents(axis->ButtonValue, buttonstate, 1, base);
		axis->ButtonValue = buttonstate;
		axis->Value = value;
	}

	static const char *AxisNames[NUM_AXES];
	static const DefaultAxisConfig DefaultAxes[NUM_AXES];
	static const ButtonMapping ButtonMap[14];

	SDL_GameController	*Device;
	SDL_JoystickID		InstanceID;
	FString				Name;
	FString				Guid;
	int					OrdinalInGuid;

	float				Multiplier;
	bool				Enabled;
	AxisInfo			Axes[NUM_AXES];
	int					LastButtons;

	friend class SDLInputJoystickManager;
};

const char *SDLInputGameController::AxisNames[] =
{
	"Left Stick X",
	"Left Stick Y",
	"Right Stick X",
	"Right Stick Y",
	"Left Trigger",
	"Right Trigger"
};

const SDLInputGameController::DefaultAxisConfig SDLInputGameController::DefaultAxes[NUM_AXES] =
{
	// Dead zone, game axis, multiplier. Matches the win32 XInput defaults.
	{ 7849 / 32768.f, JOYAXIS_Side, 1 },		// ThumbLX
	{ 7849 / 32768.f, JOYAXIS_Forward, 1 },		// ThumbLY
	{ 8689 / 32768.f, JOYAXIS_Yaw, 1 },			// ThumbRX
	{ 8689 / 32768.f, JOYAXIS_Pitch, 0.75 },	// ThumbRY
	{ 30 / 256.f, JOYAXIS_None, 0 },			// LeftTrigger
	{ 30 / 256.f, JOYAXIS_None, 0 }				// RightTrigger
};

// Bit indexes follow the XInput wButtons layout that the KEY_PAD_* block
// was designed around; 10 and 11 are reserved for the trigger pseudo-buttons
// generated by ProcessTrigger.
const SDLInputGameController::ButtonMapping SDLInputGameController::ButtonMap[14] =
{
	{ SDL_CONTROLLER_BUTTON_DPAD_UP,		0 },
	{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,		1 },
	{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,		2 },
	{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,		3 },
	{ SDL_CONTROLLER_BUTTON_START,			4 },
	{ SDL_CONTROLLER_BUTTON_BACK,			5 },
	{ SDL_CONTROLLER_BUTTON_LEFTSTICK,		6 },
	{ SDL_CONTROLLER_BUTTON_RIGHTSTICK,		7 },
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,	8 },
	{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,	9 },
	{ SDL_CONTROLLER_BUTTON_A,				12 },
	{ SDL_CONTROLLER_BUTTON_B,				13 },
	{ SDL_CONTROLLER_BUTTON_X,				14 },
	{ SDL_CONTROLLER_BUTTON_Y,				15 },
};

//==========================================================================
//
// SDLInputJoystick
//
// Legacy fallback for devices without a GameController mapping (flight
// sticks, wheels, oddities). Raw axes and JoyN buttons, as before, but the
// config section is keyed by GUID instead of enumeration index.
//
//==========================================================================

class SDLInputJoystick: public IJoystickConfig
{
public:
	SDLInputJoystick(int DeviceIndex, int Ordinal) : Multiplier(1.0f) , Enabled(true)
	{
		Guid = JoyGUIDString(DeviceIndex);
		OrdinalInGuid = Ordinal;
		Device = SDL_JoystickOpen(DeviceIndex);
		if(Device != NULL)
		{
			NumAxes = SDL_JoystickNumAxes(Device);
			NumHats = SDL_JoystickNumHats(Device);
			InstanceID = SDL_JoystickInstanceID(Device);

			SetDefaultConfig();
			M_LoadJoystickConfig(this);
		}
	}
	~SDLInputJoystick()
	{
		if(Device != NULL)
		{
			M_SaveJoystickConfig(this);
			SDL_JoystickClose(Device);
		}
	}

	bool IsValid() const
	{
		return Device != NULL;
	}

	FString GetName()
	{
		return SDL_JoystickName(Device);
	}
	float GetSensitivity()
	{
		return Multiplier;
	}
	void SetSensitivity(float scale)
	{
		Multiplier = scale;
	}

	int GetNumAxes()
	{
		return NumAxes + NumHats*2;
	}
	float GetAxisDeadZone(int axis)
	{
		return Axes[axis].DeadZone;
	}
	EJoyAxis GetAxisMap(int axis)
	{
		return Axes[axis].GameAxis;
	}
	const char *GetAxisName(int axis)
	{
		return Axes[axis].Name.GetChars();
	}
	float GetAxisScale(int axis)
	{
		return Axes[axis].Multiplier;
	}

	void SetAxisDeadZone(int axis, float zone)
	{
		Axes[axis].DeadZone = clamp(zone, MIN_DEADZONE, 1.f);
	}
	void SetAxisMap(int axis, EJoyAxis gameaxis)
	{
		Axes[axis].GameAxis = gameaxis;
	}
	void SetAxisScale(int axis, float scale)
	{
		Axes[axis].Multiplier = scale;
	}

	// Used by the saver to not save properties that are at their defaults.
	bool IsSensitivityDefault()
	{
		return Multiplier == 1.0f;
	}
	bool IsAxisDeadZoneDefault(int axis)
	{
		return Axes[axis].DeadZone <= MIN_DEADZONE;
	}
	bool IsAxisMapDefault(int axis)
	{
		if(axis >= 5)
			return Axes[axis].GameAxis == JOYAXIS_None;
		return Axes[axis].GameAxis == DefaultAxes[axis];
	}
	bool IsAxisScaleDefault(int axis)
	{
		return Axes[axis].Multiplier == 1.0f;
	}

	void SetDefaultConfig()
	{
		Axes.Clear();
		for(int i = 0;i < GetNumAxes();i++)
		{
			AxisInfo info;
			if(i < NumAxes)
				info.Name.Format("Axis %d", i+1);
			else
				info.Name.Format("Hat %d (%c)", (i-NumAxes)/2 + 1, (i-NumAxes)%2 == 0 ? 'x' : 'y');
			info.DeadZone = DEFAULT_DEADZONE;
			info.Multiplier = 1.0f;
			info.Value = 0.0;
			info.ButtonValue = 0;
			if(i >= 5)
				info.GameAxis = JOYAXIS_None;
			else
				info.GameAxis = DefaultAxes[i];
			Axes.Push(info);
		}
	}

	bool GetEnabled()
	{
		return Enabled;
	}

	void SetEnabled(bool enabled)
	{
		Enabled = enabled;
	}

	bool AllowsEnabledInBackground() { return false; }
	bool GetEnabledInBackground() { return false; }
	void SetEnabledInBackground(bool enabled) {}

	FString GetIdentifier()
	{
		return JoyIdentifier("JS", Guid, OrdinalInGuid);
	}

	SDL_JoystickID GetInstanceID() const
	{
		return InstanceID;
	}

	void AddAxes(float axes[NUM_JOYAXIS])
	{
		// Add to game axes.
		for (int i = 0; i < GetNumAxes(); ++i)
		{
			if(Axes[i].GameAxis != JOYAXIS_None)
				axes[Axes[i].GameAxis] -= float(Axes[i].Value * Multiplier * Axes[i].Multiplier);
		}
	}

	void ProcessInput()
	{
		uint8_t buttonstate;

		for (int i = 0; i < NumAxes; ++i)
		{
			buttonstate = 0;

			Axes[i].Value = SDL_JoystickGetAxis(Device, i)/32767.0;
			Axes[i].Value = Joy_RemoveDeadZone(Axes[i].Value, Axes[i].DeadZone, &buttonstate);

			// Map button to axis
			// X and Y are handled differently so if we have 2 or more axes then we'll use that code instead.
			if (NumAxes == 1 || (i >= 2 && i < NUM_JOYAXISBUTTONS))
			{
				Joy_GenerateButtonEvents(Axes[i].ButtonValue, buttonstate, 2, KEY_JOYAXIS1PLUS + i*2);
				Axes[i].ButtonValue = buttonstate;
			}
		}

		if(NumAxes > 1)
		{
			buttonstate = Joy_XYAxesToButtons(Axes[0].Value, Axes[1].Value);
			Joy_GenerateButtonEvents(Axes[0].ButtonValue, buttonstate, 4, KEY_JOYAXIS1PLUS);
			Axes[0].ButtonValue = buttonstate;
		}

		// Map POV hats to buttons and axes.  Why axes?  Well apparently I have
		// a gamepad where the left control stick is a POV hat (instead of the
		// d-pad like you would expect, no that's pressure sensitive).  Also
		// KDE's joystick dialog maps them to axes as well.
		for (int i = 0; i < NumHats; ++i)
		{
			AxisInfo &x = Axes[NumAxes + i*2];
			AxisInfo &y = Axes[NumAxes + i*2 + 1];

			buttonstate = SDL_JoystickGetHat(Device, i);

			// If we're going to assume that we can pass SDL's value into
			// Joy_GenerateButtonEvents then we might as well assume the format here.
			if(buttonstate & 0x1) // Up
				y.Value = -1.0;
			else if(buttonstate & 0x4) // Down
				y.Value = 1.0;
			else
				y.Value = 0.0;
			if(buttonstate & 0x2) // Left
				x.Value = 1.0;
			else if(buttonstate & 0x8) // Right
				x.Value = -1.0;
			else
				x.Value = 0.0;

			if(i < 4)
			{
				Joy_GenerateButtonEvents(x.ButtonValue, buttonstate, 4, KEY_JOYPOV1_UP + i*4);
				x.ButtonValue = buttonstate;
			}
		}
	}

protected:
	struct AxisInfo
	{
		FString Name;
		float DeadZone;
		float Multiplier;
		EJoyAxis GameAxis;
		double Value;
		uint8_t ButtonValue;
	};
	static const EJoyAxis DefaultAxes[5];

	SDL_Joystick		*Device;
	SDL_JoystickID		InstanceID;
	FString				Guid;
	int					OrdinalInGuid;

	float				Multiplier;
	bool				Enabled;
	TArray<AxisInfo>	Axes;
	int					NumAxes;
	int					NumHats;

	friend class SDLInputJoystickManager;
};

// [Nash 4 Feb 2024] seems like on Linux, the third axis is actually the Left Trigger, resulting in the player uncontrollably looking upwards.
const EJoyAxis SDLInputJoystick::DefaultAxes[5] = {JOYAXIS_Side, JOYAXIS_Forward, JOYAXIS_None, JOYAXIS_Yaw, JOYAXIS_Pitch};

//==========================================================================
//
// SDLInputJoystickManager
//
//==========================================================================

class SDLInputJoystickManager
{
public:
	// The initial scan is deferred: I_StartupJoysticks runs before the
	// cvar system is up, and Rescan reads joy_sdl_gamecontroller. Every
	// entry point below runs post-init and calls EnsureScan first.
	SDLInputJoystickManager() : NeedScan(true)
	{
	}
	~SDLInputJoystickManager()
	{
		Clear();
	}

	void EnsureScan()
	{
		if (NeedScan)
		{
			NeedScan = false;
			Rescan(false);
		}
	}

	void Clear()
	{
		for(unsigned int i = 0;i < Controllers.Size();i++)
			delete Controllers[i];
		Controllers.Clear();
		for(unsigned int i = 0;i < Joysticks.Size();i++)
			delete Joysticks[i];
		Joysticks.Clear();
	}

	void Rescan(bool verbose = true)
	{
		NeedScan = false;
		Clear();

		TArray<FString> seenguids;
		for(int i = 0;i < SDL_NumJoysticks();i++)
		{
			// Count previous devices with the same GUID so identical
			// hardware gets distinct config sections.
			FString guid = JoyGUIDString(i);
			int ordinal = 0;
			for(unsigned int j = 0;j < seenguids.Size();j++)
			{
				if(seenguids[j].Compare(guid) == 0) ordinal++;
			}
			seenguids.Push(guid);

			if(joy_sdl_gamecontroller && SDL_IsGameController(i))
			{
				SDLInputGameController *device = new SDLInputGameController(i, ordinal);
				if(device->IsValid())
				{
					if(verbose)
						Printf("Game controller: %s [%s]\n", device->GetName().GetChars(), device->GetIdentifier().GetChars());
					Controllers.Push(device);
				}
				else
					delete device;
			}
			else
			{
				SDLInputJoystick *device = new SDLInputJoystick(i, ordinal);
				if(device->IsValid())
				{
					if(verbose)
						Printf("Legacy joystick: %s [%s]\n", device->GetName().GetChars(), device->GetIdentifier().GetChars());
					Joysticks.Push(device);
				}
				else
					delete device;
			}
		}
	}

	void AddAxes(float axes[NUM_JOYAXIS])
	{
		EnsureScan();
		for(unsigned int i = 0;i < Controllers.Size();i++)
			if(Controllers[i]->Enabled) Controllers[i]->AddAxes(axes);
		for(unsigned int i = 0;i < Joysticks.Size();i++)
			if(Joysticks[i]->Enabled) Joysticks[i]->AddAxes(axes);
	}
	void GetDevices(TArray<IJoystickConfig *> &sticks)
	{
		EnsureScan();
		for(unsigned int i = 0;i < Controllers.Size();i++)
			sticks.Push(Controllers[i]);
		for(unsigned int i = 0;i < Joysticks.Size();i++)
			sticks.Push(Joysticks[i]);
	}

	bool IsGameControllerInstance(SDL_JoystickID instance)
	{
		EnsureScan();
		for(unsigned int i = 0;i < Controllers.Size();i++)
			if(Controllers[i]->GetInstanceID() == instance) return true;
		return false;
	}

	void ProcessInput()
	{
		EnsureScan();
		for(unsigned int i = 0;i < Controllers.Size();++i)
			if(Controllers[i]->Enabled) Controllers[i]->ProcessInput();
		for(unsigned int i = 0;i < Joysticks.Size();++i)
			if(Joysticks[i]->Enabled) Joysticks[i]->ProcessInput();
	}
protected:
	bool NeedScan;
	TArray<SDLInputGameController *> Controllers;
	TArray<SDLInputJoystick *> Joysticks;
};
static SDLInputJoystickManager *JoystickManager;

void I_StartupJoysticks()
{
#ifndef NO_SDL_JOYSTICK
	if(SDL_InitSubSystem(SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER) >= 0)
		JoystickManager = new SDLInputJoystickManager();
#endif
}
void I_ShutdownInput()
{
	if(JoystickManager)
	{
		delete JoystickManager;
		JoystickManager = NULL;
		SDL_QuitSubSystem(SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER);
	}
}

void I_GetJoysticks(TArray<IJoystickConfig *> &sticks)
{
	sticks.Clear();

	if (JoystickManager)
		JoystickManager->GetDevices(sticks);
}

void I_GetAxes(float axes[NUM_JOYAXIS])
{
	for (int i = 0; i < NUM_JOYAXIS; ++i)
	{
		axes[i] = 0;
	}
	if (use_joystick && JoystickManager)
	{
		JoystickManager->AddAxes(axes);
	}
}

void I_ProcessJoysticks()
{
	if (use_joystick && JoystickManager)
		JoystickManager->ProcessInput();
}

// True if this SDL joystick instance is being serviced through the
// GameController path, so raw SDL_JOYBUTTON events must be ignored for it.
bool I_IsGameControllerInstance(int instance)
{
	return JoystickManager != NULL && JoystickManager->IsGameControllerInstance(instance);
}

// Hotplug: rebuild the device list. Config is keyed by GUID, so devices
// keep their settings across replug/rescan.
void I_UpdateJoystickDevices()
{
	if (JoystickManager)
	{
		JoystickManager->Rescan();
		UpdateJoystickMenu(NULL);
		event_t ev = { EV_DeviceChange };
		D_PostEvent(&ev);
	}
}

IJoystickConfig *I_UpdateDeviceList()
{
	return NULL;
}
