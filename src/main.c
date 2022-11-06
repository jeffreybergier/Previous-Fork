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

volatile bool   bQuitProgram = false;            /* Flag to quit program cleanly */

volatile bool   bEmulationActive = true;         /* Run emulation when started */
static bool     bAccurateDelays;                 /* Host system has an accurate SDL_Delay()? */
static bool     bIgnoreNextMouseMotion = false;  /* Next mouse motion will be ignored (needed after SDL_WarpMouse) */

static SDL_Thread* nextThread;
static SDL_sem*    pauseFlag;

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
	
	SDL_SemWait(pauseFlag); /* Wait until 68k thread is paused */
	
	host_pause_time(!(bEmulationActive));
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

	bEmulationActive = true;
	host_pause_time(!(bEmulationActive));
	Sound_Pause(false);
	NextBus_Pause(false);

	/* Set mouse pointer to the middle of the screen and hide it */
	Main_WarpMouse(sdlscrn->w/2, sdlscrn->h/2);
	SDL_ShowCursor(SDL_DISABLE);

	Main_SetMouseGrab(bGrabMouse);
	
	return true;
}

/*-----------------------------------------------------------------------*/
/**
 * Optionally ask user whether to quit and set bQuitProgram accordingly
 */
void Main_RequestQuit(void) {
	if (ConfigureParams.Log.bConfirmQuit) {
		Main_PauseEmulation(true);
		bQuitProgram = false;	/* if set true, dialog exits */
		bQuitProgram = DlgAlert_Query("All unsaved data will be lost.\nDo you really want to quit?");
		Main_UnPauseEmulation();
	}
	else {
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
 * Save an event and make it available for the 68k thread
 **/
static SDL_Event    mainEvent;
static SDL_SpinLock mainEventLock;
static bool         mainEventValid;

static void Main_PutEvent(SDL_Event* event) {
	SDL_AtomicLock(&mainEventLock);
	mainEvent      = *event;
	mainEventValid = true;
	SDL_AtomicUnlock(&mainEventLock);
}

static bool Main_GetEvent(SDL_Event* event) {
	bool valid;
    
	SDL_AtomicLock(&mainEventLock);
    valid = mainEventValid;
	if (valid) {
		*event         = mainEvent;
		mainEventValid = false;
	}
	SDL_AtomicUnlock(&mainEventLock);
	
	return valid;
}

/* ----------------------------------------------------------------------- */
/**
 * Handle mouse motion event.
 */
static void Main_HandleMouseMotion(SDL_Event event) {
	SDL_Event mouse_event[100];

	int i,nb;

	static bool s_left=false;
	static bool s_up=false;
	static float s_fdx=0.0;
	static float s_fdy=0.0;
	
	bool left=false;
	bool up=false;
	float fdx;
	float fdy;
	
	float exp = bGrabMouse ? ConfigureParams.Mouse.fExpSpeedLocked : ConfigureParams.Mouse.fExpSpeedNormal;
	float lin = bGrabMouse ? ConfigureParams.Mouse.fLinSpeedLocked : ConfigureParams.Mouse.fLinSpeedNormal;

	if (bIgnoreNextMouseMotion) {
		bIgnoreNextMouseMotion = false;
		return;
	}

	/* get all mouse event to clean the queue and sum them */
	nb=SDL_PeepEvents(&mouse_event[0], 100, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION);

	for (i=0;i<nb;i++) {
		event.motion.xrel += mouse_event[i].motion.xrel;
		event.motion.yrel += mouse_event[i].motion.yrel;
	}
	
	if (event.motion.xrel || event.motion.yrel) {
		/* Remove the sign */
		if (event.motion.xrel < 0) {
			event.motion.xrel = -event.motion.xrel;
			left = true;
		}
		if (event.motion.yrel < 0) {
			event.motion.yrel = -event.motion.yrel;
			up = true;
		}
		/* Exponential adjustmend */
		fdx = pow(event.motion.xrel, exp);
		fdy = pow(event.motion.yrel, exp);
		
		/* Linear adjustment */
		fdx *= lin;
		fdy *= lin;
		
		/* Add residuals */
		if (left == s_left) {
			s_fdx += fdx;
		} else {
			s_fdx  = fdx;
			s_left = left;
		}
		if (up == s_up) {
			s_fdy += fdy;
		} else {
			s_fdy  = fdy;
			s_up   = up;
		}
		
		/* Convert to integer and save residuals */
		event.motion.xrel = s_fdx;
		s_fdx -= event.motion.xrel;
		event.motion.yrel = s_fdy;
		s_fdy -= event.motion.yrel;
		
		/* Re-add signs */
		if (left) {
			event.motion.xrel = -event.motion.xrel;
		}
		if (up) {
			event.motion.yrel = -event.motion.yrel;
		}
	}
	
	Main_PutEvent(&event);
}

static int statusBarUpdate;

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

		ShortCut_ActKey();

		if (bEmulationActive) {
			events = SDL_PollEvent(&event);
		} else {
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
                        Main_RequestQuit();
                        break;
                    case SDL_WINDOWEVENT_RESIZED:
                        Screen_SizeChanged();
                        break;
                    default:
                        break;
                }
                continue;

            case SDL_QUIT:
                Main_RequestQuit();
                break;
                
            case SDL_MOUSEMOTION:               /* Read/Update internal mouse position */
                Main_HandleMouseMotion(event);
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
                    
					Main_PutEvent(&event);
                }
                else if (event.button.button == SDL_BUTTON_RIGHT)
                {
					Main_PutEvent(&event);
                }
                break;
                
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
					Main_PutEvent(&event);
                }
                else if (event.button.button == SDL_BUTTON_RIGHT)
                {
					Main_PutEvent(&event);
                }
                break;
                
            case SDL_MOUSEWHEEL:
				Main_PutEvent(&event);
                break;
                
            case SDL_KEYDOWN:
				if (ShortCut_CheckKeys(event.key.keysym.mod, event.key.keysym.sym, 1)) {
					ShortCut_ActKey();
					break;
				}
				if (event.key.repeat) {
					break;
				}
				Main_PutEvent(&event);
                break;
                
            case SDL_KEYUP:
				if (ShortCut_CheckKeys(event.key.keysym.mod, event.key.keysym.sym, 0)) {
					break;
				}
                Main_PutEvent(&event);
                break;
                
                
            default:
                /* don't let unknown events delay event processing */
                bContinueProcessing = true;
                break;
        }
    } while (bContinueProcessing || !(bEmulationActive || bQuitProgram));
}

static void Main_Loop(void) {
    int i = 0;
    
	while (!bQuitProgram) {
		Main_EventHandler();
		SDL_Delay(5);
        if (++i > 200) {
            Statusbar_Update(sdlscrn);
            i = 0;
        }
		Screen_Update();
	}
}

static int Main_Thread(void* unused) {
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);

	/* done as last, needs CPU & DSP running... */
	DebugUI_Init();

	while (!bQuitProgram) {
		CycInt_AddRelativeInterruptUs(1000, 0, INTERRUPT_EVENT_LOOP);
		M68000_Start();               /* Start emulation */
	}

	bEmulationActive = false;

	return 0;
}

void Main_EventHandlerInterrupt(void) {
	SDL_Event event;
	int64_t time_offset;
	
    CycInt_AcknowledgeInterrupt();
	
	if (!bEmulationActive) {
		SDL_SemPost(pauseFlag);
		do {
			host_sleep_ms(20);
		} while(!bEmulationActive);
	}
	if (++statusBarUpdate > 400) {
		uint64_t vt;
		uint64_t rt;
		host_time(&rt, &vt);
#if ENABLE_TESTING
		fprintf(stderr, "[reports]");
		for(int i = 0; i < sizeof(reports)/sizeof(report_t); i++) {
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

	if (Main_GetEvent(&event)) {
		switch (event.type) {
			case SDL_MOUSEMOTION:
				Keymap_MouseMove(event.motion.xrel, event.motion.yrel);
				break;
				
			case SDL_MOUSEBUTTONDOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
					Keymap_MouseDown(true);
				}
				else if (event.button.button == SDL_BUTTON_RIGHT) {
					Keymap_MouseDown(false);
				}
				break;
				
			case SDL_MOUSEBUTTONUP:
				if (event.button.button == SDL_BUTTON_LEFT) {
					Keymap_MouseUp(true);
				}
				else if (event.button.button == SDL_BUTTON_RIGHT) {
					Keymap_MouseUp(false);
				}
				break;
				
			case SDL_MOUSEWHEEL:
				Keymap_MouseWheel(&event.wheel);
				break;
				
			case SDL_KEYDOWN:
				Keymap_KeyDown(&event.key.keysym);
				break;
				
			case SDL_KEYUP:
				Keymap_KeyUp(&event.key.keysym);
				break;
				
			default:
				break;
		}
	}
	
	time_offset = host_real_time_offset();
	if (time_offset > 0) {
		host_sleep_us(time_offset);
	}
	
	CycInt_AddRelativeInterruptUs((1000*1000)/200, 0, INTERRUPT_EVENT_LOOP); // poll events with 200 Hz
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


static void Main_StartMenu(void) {
	if (!File_Exists(sConfigFileName) || ConfigureParams.ConfigDialog.bShowConfigDialogAtStartup) {
		Dialog_DoProperty();
		if (bQuitProgram) {
			SDL_Quit();
			exit(-2);
		}
	}

	Dialog_CheckFiles();
	
	if (bQuitProgram) {
		SDL_Quit();
		exit(-2);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Initialise emulation
 */
static void Main_Init(void) {
	/* Open debug log file */
	if (!Log_Init()) {
		fprintf(stderr, "Logging/tracing initialization failed\n");
		exit(-1);
	}
	Log_Printf(LOG_INFO, PROG_NAME ", compiled on:  " __DATE__ ", " __TIME__ "\n");

	/* Init SDL's video subsystem. Note: Audio and joystick subsystems
	   will be initialized later (failures there are not fatal). */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0)
	{
		fprintf(stderr, "Could not initialize the SDL library:\n %s\n", SDL_GetError() );
		exit(-1);
	}
	SDLGui_Init();
	Screen_Init();
	Main_SetTitle(NULL);
	
	/* Init emulation */
	M68000_Init();
	DSP_Init();
	Reset_Cold();
	IoMem_Init();

	/* Call menu at startup */
	Main_StartMenu();
	
	pauseFlag  = SDL_CreateSemaphore(0);
	
    /* Start emulator thread */
	nextThread = SDL_CreateThread(Main_Thread, "[Previous] 68k at slot 0", NULL);
}


/*-----------------------------------------------------------------------*/
/**
 * Un-Initialise emulation
 */
static void Main_UnInit(void) {
	int d;
	SDL_WaitThread(nextThread, &d);
	
	Screen_ReturnFromFullScreen();
	IoMem_UnInit();
	SDLGui_UnInit();
	Screen_UnInit();
	Exit680x0();

	/* SDL uninit: */
	SDL_Quit();

	/* Close debug log file */
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
#if defined(__AMIGAOS4__)
		strncpy(psGlobalConfig, CONFDIR"previous.cfg", FILENAME_MAX);
#else
		snprintf(psGlobalConfig, FILENAME_MAX, CONFDIR"%cprevious.cfg", PATHSEP);
#endif
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
#ifdef _MUDFLAP
		__mf_register(name, 32, __MF_TYPE_GUESS, "SDL keyname");
#endif
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
	Main_Init();

	/* Set initial Statusbar information */
	Main_StatusbarSetup();
	
	/* Check if SDL_Delay is accurate */
	Main_CheckForAccurateDelays();

	/* Run emulation */
	Main_UnPauseEmulation();

	Main_Loop();

	/* Un-init emulation system */
	Main_UnInit();

	return 0;
}
