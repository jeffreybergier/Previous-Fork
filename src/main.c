/*
  Hatari - main.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Main initialization and event handling routines.
*/
const char Main_fileid[] = "Hatari main.c";

#include <time.h>
#include <errno.h>
#include <signal.h>

#include <SDL.h>

#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "ioMem.h"
#include "keymap.h"
#include "log.h"
#include "m68000.h"
#include "paths.h"
#include "reset.h"
#include "screen.h"
#include "sdlgui.h"
#include "shortcut.h"
#include "snd.h"
#include "statusbar.h"
#include "str.h"
#include "video.h"
#include "audio.h"
#include "debugui.h"
#include "file.h"
#include "dsp.h"
#include "host.h"
#include "dimension.hpp"

#include "hatari-glue.h"
#include "NextBus.hpp"

#if HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif

int nFrameSkips;

bool          bQuitProgram = false;            /* Flag to quit program cleanly */
static bool   bEmulationActive = false;        /* Do not run emulation during initialization */
static bool   bAccurateDelays;                 /* Host system has an accurate SDL_Delay()? */
static bool   bIgnoreNextMouseMotion = false;  /* Next mouse motion will be ignored (needed after SDL_WarpMouse) */

volatile int mainPauseEmulation;

typedef const char* (*report_func)(uint64_t realTime, uint64_t hostTime);

typedef struct {
	const char*       label;
	const report_func report;
} report_t;

static uint64_t lastRT;
static uint64_t lastCycles;
static double   speedFactor;
static char     speedMsg[32];

static void Main_Speed(uint64_t realTime, uint64_t hostTime) {
	uint64_t dRT  = realTime - lastRT;
	speedFactor   = (nCyclesMainCounter - lastCycles);
	speedFactor  /= ConfigureParams.System.nCpuFreq;
	speedFactor  /= dRT;
	lastRT        = realTime;
	lastCycles    = nCyclesMainCounter;
}

void Main_SpeedReset(void) {
	uint64_t realTime, hostTime;
	host_time(&realTime, &hostTime);
	lastRT     = realTime;
	lastCycles = nCyclesMainCounter;

	Log_Printf(LOG_WARN, "Realtime mode %s.\n", ConfigureParams.System.bRealtime ? "enabled" : "disabled");
}

const char* Main_SpeedMsg() {
	speedMsg[0] = 0;
	if(speedFactor > 0) {
		if(ConfigureParams.System.bRealtime) {
			snprintf(speedMsg, sizeof(speedMsg), "%dMHz/", (int)(ConfigureParams.System.nCpuFreq * speedFactor + 0.5));
		} else {
			if ((speedFactor < 0.9) || (speedFactor > 1.1))
				snprintf(speedMsg, sizeof(speedMsg), "%.1fx%dMHz/", speedFactor, ConfigureParams.System.nCpuFreq);
			else
				snprintf(speedMsg, sizeof(speedMsg), "%dMHz/",                   ConfigureParams.System.nCpuFreq);
		}
	}
	return speedMsg;
}

#if ENABLE_TESTING
static const report_t reports[] = {
	{"ND",    nd_reports},
	{"Host",  host_report},
};
#endif

/*-----------------------------------------------------------------------*/
/**
 * Pause emulation, stop sound.  'visualize' should be set true,
 * unless unpause will be called immediately afterwards.
 * 
 * @return true if paused now, false if was already paused
 */
bool Main_PauseEmulation(bool visualize) {
	if ( !bEmulationActive )
		return false;

	bEmulationActive = false;
	host_pause_time(true);
	Screen_Pause(true);
	Sound_Pause(true);
	NextBus_Pause(true);

	if (visualize) {
		Statusbar_AddMessage("Emulation paused", 100);
		/* make sure msg gets shown */
		Statusbar_Update(sdlscrn);
		
		/* Un-grab mouse pointer */
		Main_SetMouseGrab(false);
	}

	/* Show mouse pointer and set it to the middle of the screen */
	SDL_ShowCursor(SDL_ENABLE);
	Main_WarpMouse(sdlscrn->w/2, sdlscrn->h/2);

	return true;
}

/*-----------------------------------------------------------------------*/
/**
 * Start/continue emulation
 * 
 * @return true if continued, false if was already running
 */
bool Main_UnPauseEmulation(void) {
	if ( bEmulationActive )
		return false;

	NextBus_Pause(false);
	Sound_Pause(false);
	Screen_Pause(false);
	host_pause_time(false);

	/* Set mouse pointer to the middle of the screen and hide it */
	Main_WarpMouse(sdlscrn->w/2, sdlscrn->h/2);
	SDL_ShowCursor(SDL_DISABLE);

	Main_SetMouseGrab(bGrabMouse);

	bEmulationActive = true;

	return true;
}

/*-----------------------------------------------------------------------*/
/**
 * Optionally ask user whether to quit and set bQuitProgram accordingly
 */
void Main_RequestQuit(bool confirm) {
	bool bWasActive;

	if (confirm) {
		bWasActive = Main_PauseEmulation(true);
		bQuitProgram = false;	/* if set true, dialog exits */
		bQuitProgram = DlgAlert_Query("All unsaved data will be lost.\nDo you really want to quit?");
		if (bWasActive) {
			Main_UnPauseEmulation();
		}
	} else {
		bQuitProgram = true;
	}

	if (bQuitProgram) {
		/* Assure that CPU core shuts down */
		M68000_Stop();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Since SDL_Delay and friends are very inaccurate on some systems, we have
 * to check if we can rely on this delay function.
 */
static void Main_CheckForAccurateDelays(void) {
	int nStartTicks, nEndTicks;

	/* Force a task switch now, so we have a longer timeslice afterwards */
	SDL_Delay(10);

	nStartTicks = SDL_GetTicks();
	SDL_Delay(1);
	nEndTicks = SDL_GetTicks();

	/* If the delay took longer than 10ms, we are on an inaccurate system! */
	bAccurateDelays = ((nEndTicks - nStartTicks) < 9);

	if (bAccurateDelays)
		Log_Printf(LOG_WARN, "Host system has accurate delays. (%d)\n", nEndTicks - nStartTicks);
	else
		Log_Printf(LOG_WARN, "Host system does not have accurate delays. (%d)\n", nEndTicks - nStartTicks);
}


/* ----------------------------------------------------------------------- */
/**
 * Set mouse pointer to new coordinates and set flag to ignore the mouse event
 * that is generated by SDL_WarpMouse().
 */
void Main_WarpMouse(int x, int y) {
	SDL_WarpMouseInWindow(sdlWindow, x, y); /* Set mouse pointer to new position */
	bIgnoreNextMouseMotion = true;          /* Ignore mouse motion event from SDL_WarpMouse */
}


/* ----------------------------------------------------------------------- */
/**
 * Set mouse grab.
 */
void Main_SetMouseGrab(bool grab) {
	/* If emulation is active, set the mouse cursor mode now: */
	if (grab) {
		if (bEmulationActive) {
			Main_WarpMouse(sdlscrn->w/2, sdlscrn->h/2); /* Cursor must be inside window */
			SDL_SetRelativeMouseMode(SDL_TRUE);
			SDL_SetWindowGrab(sdlWindow, SDL_TRUE);
			Main_SetTitle(MOUSE_LOCK_MSG);
		}
	} else {
		SDL_SetRelativeMouseMode(SDL_FALSE);
		SDL_SetWindowGrab(sdlWindow, SDL_FALSE);
		Main_SetTitle(NULL);
	}
}

/* ----------------------------------------------------------------------- */
/**
 * Handle mouse motion event.
 */
static void Main_HandleMouseMotion(SDL_Event *pEvent) {
	SDL_Event mouse_event[100];

	int nEvents;

	static float fSavedDeltaX = 0.0;
	static float fSavedDeltaY = 0.0;
	
	float fDeltaX;
	float fDeltaY;
	int   nDeltaX;
	int   nDeltaY;

	float fExp = bGrabMouse ? ConfigureParams.Mouse.fExpSpeedLocked : ConfigureParams.Mouse.fExpSpeedNormal;
	float fLin = bGrabMouse ? ConfigureParams.Mouse.fLinSpeedLocked : ConfigureParams.Mouse.fLinSpeedNormal;

	if (bIgnoreNextMouseMotion) {
		bIgnoreNextMouseMotion = false;
		return;
	}

	nDeltaX = pEvent->motion.xrel;
	nDeltaY = pEvent->motion.yrel;

	/* get all mouse event to clean the queue and sum them */
	nEvents = SDL_PeepEvents(mouse_event, 100, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION);

	for (int i = 0; i < nEvents; i++) {
		nDeltaX += mouse_event[i].motion.xrel;
		nDeltaY += mouse_event[i].motion.yrel;
	}

	if (nDeltaX || nDeltaY) {
		/* Exponential adjustmend */
		if (fExp != 1.0) {
			fDeltaX = (nDeltaX < 0) ? -pow(-nDeltaX, fExp) : pow(nDeltaX, fExp);
			fDeltaY = (nDeltaY < 0) ? -pow(-nDeltaY, fExp) : pow(nDeltaY, fExp);
		}

		/* Linear adjustment */
		if (fLin != 1.0) {
			fDeltaX *= fLin;
			fDeltaY *= fLin;
		}

		/* Add residuals */
		if ((fDeltaX < 0.0) == (fSavedDeltaX < 0.0)) {
			fSavedDeltaX += fDeltaX;
		} else {
			fSavedDeltaX  = fDeltaX;
		}
		if ((fDeltaY < 0.0) == (fSavedDeltaY < 0.0)) {
			fSavedDeltaY += fDeltaY;
		} else {
			fSavedDeltaY  = fDeltaY;
		}

		/* Convert to integer and save residuals */
		nDeltaX = fSavedDeltaX;
		fSavedDeltaX -= nDeltaX;
		nDeltaY = fSavedDeltaY;
		fSavedDeltaY -= nDeltaY;

		/* Done */
		Keymap_MouseMove(nDeltaX, nDeltaY);
	}
}


/* ----------------------------------------------------------------------- */
/**
 * Emulator message handler. Called from emulator.
 */
void Main_EventHandlerInterrupt(void) {
	static int statusBarUpdate = 0;

	CycInt_AcknowledgeInterrupt();

	if (++statusBarUpdate > 400) {
		uint64_t vt;
		uint64_t rt;
		host_time(&rt, &vt);
#if ENABLE_TESTING
		fprintf(stderr, "[reports]");
		for(int i = 0; i < ARRAY_SIZE(reports); i++) {
			const char* msg = reports[i].report(rt, vt);
			if(msg[0]) fprintf(stderr, " %s:%s", reports[i].label, msg);
		}
		fprintf(stderr, "\n");
		fflush(stderr);
#endif
		Main_Speed(rt, vt);
		Statusbar_UpdateInfo();
		statusBarUpdate = 0;
	}

	Main_EventHandler();

	CycInt_AddRelativeInterruptUs((1000*1000)/200, 0, INTERRUPT_EVENT_LOOP); // poll events with 200 Hz
}

/* ----------------------------------------------------------------------- */
/**
 * SDL message handler.
 * Here we process the SDL events (keyboard, mouse, ...)
 */
void Main_EventHandler(void) {
	bool bContinueProcessing;
	SDL_Event event;
	int events;

	do {
		bContinueProcessing = false;

		/* check remote process control from different thread (e.g. i860) */
		switch(mainPauseEmulation) {
			case PAUSE_EMULATION:
				mainPauseEmulation = PAUSE_NONE;
				Main_PauseEmulation(true);
				break;
			case UNPAUSE_EMULATION:
				mainPauseEmulation = PAUSE_NONE;
				Main_UnPauseEmulation();
				break;
		}

		if (bEmulationActive) {
			int64_t time_offset = host_real_time_offset() / 1000;
			if (time_offset > 10)
				events = SDL_WaitEventTimeout(&event, time_offset);
			else
				events = SDL_PollEvent(&event);
		}
		else {
			ShortCut_ActKey();
			/* last (shortcut) event activated emulation? */
			if ( bEmulationActive )
				break;
			events = SDL_WaitEvent(&event);
		}
		if (!events) {
			/* no events -> if emulation is active or
			 * user is quitting -> return from function.
			 */
			continue;
		}
		switch (event.type) {
			case SDL_WINDOWEVENT:
				switch(event.window.event) {
					case SDL_WINDOWEVENT_CLOSE:
						SDL_FlushEvent(SDL_QUIT); // remove SDL_Quit if pending
						Main_RequestQuit(true);
						break;
					case SDL_WINDOWEVENT_RESIZED:
						Screen_SizeChanged();
						break;
					default:
						break;
				}
				continue;

			case SDL_QUIT:
				Main_RequestQuit(true);
				break;

			case SDL_MOUSEMOTION:               /* Read/Update internal mouse position */
				Main_HandleMouseMotion(&event);
				bContinueProcessing = false;
				break;

			case SDL_MOUSEBUTTONDOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
					if (bGrabMouse) {
						if (SDL_GetModState() & KMOD_CTRL) {
							bGrabMouse = false;
							Main_SetMouseGrab(bGrabMouse);
							break;
						}
					} else {
						if (ConfigureParams.Mouse.bEnableAutoGrab) {
							bGrabMouse = true;
							Main_SetMouseGrab(bGrabMouse);
							break;
						}
					}
					Keymap_MouseDown(true);
				}
				else if (event.button.button == SDL_BUTTON_RIGHT)
				{
					Keymap_MouseDown(false);
				}
				break;

			case SDL_MOUSEBUTTONUP:
				if (event.button.button == SDL_BUTTON_LEFT) {
					Keymap_MouseUp(true);
				}
				else if (event.button.button == SDL_BUTTON_RIGHT)
				{
					Keymap_MouseUp(false);
				}
				break;

			case SDL_MOUSEWHEEL:
				Keymap_MouseWheel(&event.wheel);
				break;

			case SDL_KEYDOWN:
				if (event.key.repeat) {
					break;
				}
				if (ShortCut_CheckKeys(event.key.keysym.mod, event.key.keysym.sym, 1)) {
					ShortCut_ActKey();
					break;
				}
				Keymap_KeyDown(&event.key.keysym);
				break;

			case SDL_KEYUP:
				if (ShortCut_CheckKeys(event.key.keysym.mod, event.key.keysym.sym, 0)) {
					break;
				}
				Keymap_KeyUp(&event.key.keysym);
				break;

			default:
				/* don't let unknown events delay event processing */
				bContinueProcessing = true;
				break;
		}
	} while (bContinueProcessing || !(bEmulationActive || bQuitProgram));
}

/* ----------------------------------------------------------------------- */
/**
 * Main loop. Start emulation and loop.
 */
static void Main_Loop(void) {
	/* Start EventHandler */
	CycInt_AddRelativeInterruptUs(500*1000, 0, INTERRUPT_EVENT_LOOP);
	/* Start Emulation */
	Main_UnPauseEmulation();
	M68000_Start();
}

/* ----------------------------------------------------------------------- */
/**
 * Statusbar update with reduced update frequency to save CPU cycles.
 * Call this on emulated machine VBL.
 */
void Main_UpdateStatusbar(void) {
	static int i = 0;
	if (++i > 9) {
		Statusbar_Update(sdlscrn);
		i = 0;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Set Previous window title. Use NULL for default
 */
void Main_SetTitle(const char *title) {
	if (title)
		SDL_SetWindowTitle(sdlWindow, title);
	else
		SDL_SetWindowTitle(sdlWindow, PROG_NAME);
}

/*-----------------------------------------------------------------------*/
/**
 * Show dialog at start.
 * 
 * @return true if configuration is ready, false if we need to quit
 */
static bool Main_StartMenu(void) {
	if (!File_Exists(sConfigFileName) || ConfigureParams.ConfigDialog.bShowConfigDialogAtStartup) {
		Dialog_DoProperty();
	}
	if (!bQuitProgram) {
		Dialog_CheckFiles();
	}
	return !bQuitProgram;
}

/*-----------------------------------------------------------------------*/
/**
 * Initialize emulation
 * 
 * @return true if initialization succeeded, false if it failed
 */
static bool Main_Init(void) {
	/* Open debug log file */
	if (!Log_Init()) {
		fprintf(stderr, "Logging/tracing initialization failed\n");
		exit(-1);
	}
	Log_Printf(LOG_INFO, PROG_NAME ", compiled on:  " __DATE__ ", " __TIME__ "\n");

	/* Init SDL's video and timer subsystems. Note: Audio subsystem
	   will be initialized later (failure not fatal). */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0)
	{
		fprintf(stderr, "Could not initialize the SDL library:\n %s\n", SDL_GetError() );
		exit(-1);
	}
	SDLGui_Init();
	Screen_Init();
	Keymap_Init();
	Main_SetTitle(NULL);

	/* Init emulation */
	M68000_Init();
	DSP_Init();
	IoMem_Init();
	/* Done as last, needs CPU & DSP running... */
	DebugUI_Init();

	/* Call menu at startup */
	if (Main_StartMenu()) {
		Reset_Cold();
		return true;
	}
	return false;
}


/*-----------------------------------------------------------------------*/
/**
 * Un-Initialise emulation
 */
static void Main_UnInit(void) {
	Screen_ReturnFromFullScreen();
	IoMem_UnInit();
	SDLGui_UnInit();
	Screen_UnInit();
	Exit680x0();

	/* SDL uninit: */
	SDL_Quit();

	/* Close debug log file */
	DebugUI_UnInit();
	Log_UnInit();

	Paths_UnInit();
}


/*-----------------------------------------------------------------------*/
/**
 * Load initial configuration file(s)
 */
static void Main_LoadInitialConfig(void) {
	char *psGlobalConfig;

	psGlobalConfig = malloc(FILENAME_MAX);
	if (psGlobalConfig)
	{
		File_MakePathBuf(psGlobalConfig, FILENAME_MAX, CONFDIR,
		                 "previous", "cfg");
		/* Try to load the global configuration file */
		Configuration_Load(psGlobalConfig);

		free(psGlobalConfig);
	}

	/* Now try the users configuration file */
	Configuration_Load(NULL);
}

/*-----------------------------------------------------------------------*/
/**
 * Set TOS etc information and initial help message
 */
static void Main_StatusbarSetup(void) {
	const char *name = NULL;
	SDL_Keycode key;

	key = ConfigureParams.Shortcut.withoutModifier[SHORTCUT_OPTIONS];
	if (!key)
		key = ConfigureParams.Shortcut.withModifier[SHORTCUT_OPTIONS];
	if (key)
		name = SDL_GetKeyName(key);
	if (name)
	{
		char message[24], *keyname;

		keyname = Str_ToUpper(strdup(name));
		snprintf(message, sizeof(message), "Press %s for Options", keyname);
		free(keyname);

		Statusbar_AddMessage(message, 6000);
	}
	/* update information loaded by Main_Init() */
	Statusbar_UpdateInfo();
}

#ifdef WIN32
	extern void Win_OpenCon(void);
#endif

/*-----------------------------------------------------------------------*/
/**
 * Set signal handlers to catch signals
 */
static void Main_SetSignalHandlers(void) {
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
	signal(SIGFPE, SIG_IGN);
}


/*-----------------------------------------------------------------------*/
/**
 * Main
 * 
 * Note: 'argv' cannot be declared const, MinGW would then fail to link.
 */
int main(int argc, char *argv[])
{
	/* Generate random seed */
	srand(time(NULL));

	/* Set signal handlers */
	Main_SetSignalHandlers();

	/* Initialize directory strings */
	Paths_Init(argv[0]);

	/* Set default configuration values */
	Configuration_SetDefault();

	/* Now load the values from the configuration file */
	Main_LoadInitialConfig();

	/* monitor type option might require "reset" -> true */
	Configuration_Apply(true);

#ifdef WIN32
	Win_OpenCon();
#endif

#if HAVE_SETENV
	/* Needed on maemo but useful also with normal X11 window managers for
	 * window grouping when you have multiple Previous SDL windows open */
	setenv("SDL_VIDEO_X11_WMCLASS", "previous", 1);

	/* Needed for proper behavior of Caps Lock on some systems */
	setenv("SDL_DISABLE_LOCK_KEYS", "1", 1);
#endif

	/* Init emulator system */
	if (Main_Init()) {
		/* Set initial Statusbar information */
		Main_StatusbarSetup();
		
		/* Check if SDL_Delay is accurate */
		Main_CheckForAccurateDelays();
		
		/* Run emulation */
		Main_Loop();
	}

	/* Un-init emulation system */
	Main_UnInit();

	return 0;
}
