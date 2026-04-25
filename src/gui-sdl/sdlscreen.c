/*
  Previous - sdlscreen.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains the SDL interface for video output.
*/
const char SDLscreen_fileid[] = "Previous sdlscreen.c";

#include "main.h"
#include "configuration.h"
#include "log.h"
#include "screen.h"
#include "sdlscreen.h"
#include "statusbar.h"
#include "sdlstatusbar.h"
#include "event.h"
#include "dimension.hpp"
#include "nd_sdl.hpp"
#include "video.h"
#include "keymap.h"
#include "m68000.h"


SDL_Window*   sdlWindow;
SDL_Surface*  sdlscrn = NULL;        /* The SDL screen surface */

/* extern for shortcuts */
volatile bool bGrabMouse    = false; /* Grab the mouse cursor in the window */
volatile bool bInFullScreen = false; /* true if in full screen */

/* extern for tablet */
int screen_x = 1120;
int screen_y = 832;

static const int NeXT_SCRN_WIDTH  = 1120;
static const int NeXT_SCRN_HEIGHT = 832;
static int width;   /* guest framebuffer */
static int height;  /* guest framebuffer */

static SDL_Renderer* sdlRenderer;
static SDL_Texture*  uiTexture;
static SDL_Texture*  fbTexture;
static SDL_Texture*  fbGroupTexture[NUM_MONITORS];
static SDL_AtomicInt blitUI;
static bool          doUIblit;
static SDL_Rect      statusBar;
static SDL_FRect     screenRect;
static SDL_FRect     fbRect[NUM_MONITORS];
static SDL_Rect      saveWindowBounds; /* Window bounds before going fullscreen. Used to restore window size & position. */
static SCREENMODE    saveScreenMode;   /* Save screen mode to restore on return from fullscreen */
static SCREENMODE    initScreenMode;   /* Save screen mode present at last init */
static uint32_t      mask;             /* green screen mask for transparent UI areas */
static void*         uiBuffer;         /* uiBuffer used for user interface texture */
static SDL_SpinLock  uiBufferLock;     /* Lock for concurrent access to UI buffer between m68k thread and repainter */
#ifdef ENABLE_RENDERING_THREAD
static volatile bool doRepaint = true; /* Repaint thread runs while true */
static SDL_Thread*   repaintThread;
#endif


static uint32_t BW2RGB[0x400];
static uint32_t COL2RGB[0x10000];

static uint32_t bw2rgb(SDL_Surface* surf, int bw) {
	switch(bw & 3) {
		case 3:  return SDL_MapSurfaceRGB(surf, 0,   0,   0);
		case 2:  return SDL_MapSurfaceRGB(surf, 85,  85,  85);
		case 1:  return SDL_MapSurfaceRGB(surf, 170, 170, 170);
		case 0:  return SDL_MapSurfaceRGB(surf, 255, 255, 255);
		default: return 0;
	}
}

static uint32_t col2rgb(SDL_Surface* surf, int col) {
	int r = col & 0xF000; r >>= 12; r |= r << 4;
	int g = col & 0x0F00; g >>= 8;  g |= g << 4;
	int b = col & 0x00F0; b >>= 4;  b |= b << 4;
	return SDL_MapSurfaceRGB(surf, r, g, b);
}

/*
 BW format is 2 bit per pixel
 */
static void blitBW(SDL_Texture* tex) {
	void* pixels;
	uint32_t* dst;
	int src, idx, src_pitch, dst_pitch, x, y;

	src_pitch = (NeXT_SCRN_WIDTH + (ConfigureParams.System.bTurbo ? 0 : 32)) / 4;
	SDL_LockTexture(tex, NULL, &pixels, &dst_pitch);
	for (y = 0; y < NeXT_SCRN_HEIGHT; y++) {
		src = y * src_pitch;
		dst = (uint32_t*)((uint8_t*)pixels + (y * dst_pitch));
		for (x = 0; x < NeXT_SCRN_WIDTH / 4; x++) {
			idx = NEXTVideo[src++] * 4;
			*dst++ = BW2RGB[idx+0];
			*dst++ = BW2RGB[idx+1];
			*dst++ = BW2RGB[idx+2];
			*dst++ = BW2RGB[idx+3];
		}
	}
	SDL_UnlockTexture(tex);
}

/*
 Color format is 4 bit per pixel, big-endian: RGBX
 */
static void blitColor(SDL_Texture* tex) {
	void* pixels;
	uint16_t* src;
	uint32_t* dst;
	int src_pitch, dst_pitch, x, y;

	src_pitch = NeXT_SCRN_WIDTH + (ConfigureParams.System.bTurbo ? 0 : 32);
	SDL_LockTexture(tex, NULL, &pixels, &dst_pitch);
	for (y = 0; y < NeXT_SCRN_HEIGHT; y++) {
		src = (uint16_t*)NEXTVideo + (y * src_pitch);
		dst = (uint32_t*)((uint8_t*)pixels + (y * dst_pitch));
		for (x = 0; x < NeXT_SCRN_WIDTH; x++) {
			*dst++ = COL2RGB[*src++];
		}
	}
	SDL_UnlockTexture(tex);
}

/*
 Dimension format is 8 bit per pixel, big-endian: BBGGRRAA
 */
void Screen_BlitDimension(uint32_t* vram, SDL_Texture* tex) {
	void* src;
	void* dst;
	int src_pitch, dst_pitch;
	SDL_PixelFormat src_format, dst_format;

#if ND_STEP
	src = &vram[0];
#else
	src = &vram[4];
#endif
	src_pitch  = (NeXT_SCRN_WIDTH + 32) * 4;
	src_format = SDL_PIXELFORMAT_BGRA32;
	dst_format = tex->format;

	SDL_LockTexture(tex, NULL, &dst, &dst_pitch);
	SDL_ConvertPixels(NeXT_SCRN_WIDTH, NeXT_SCRN_HEIGHT, src_format, src, src_pitch, dst_format, dst, dst_pitch);
	SDL_UnlockTexture(tex);
}

/*
 Blank screen
 */
void Screen_Blank(SDL_Texture* tex) {
	void* pixels;
	int   pitch;
	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	SDL_memset4(pixels, COL2RGB[0], pitch * NeXT_SCRN_HEIGHT / 4);
	SDL_UnlockTexture(tex);
}

/*
 Blit NeXT framebuffer to texture.
 */
static bool blitScreen(int slot, SDL_Texture* tex) {
	if (slot > 0) {
		uint32_t* vram = nd_vram_for_slot(slot);
		if (vram) {
			if (nd_video_enabled(slot)) {
				Screen_BlitDimension(vram, tex);
			} else {
				Screen_Blank(tex);
			}
			return true;
		}
	} else {
		if (NEXTVideo) {
			if (Video_Enabled()) {
				if (ConfigureParams.System.bColor) {
					blitColor(tex);
				} else {
					blitBW(tex);
				}
			} else {
				Screen_Blank(tex);
			}
			return true;
		}
	}
	return false;
}

/*
 Blit user interface to texture.
 */
static void blitUserInterface(SDL_Texture* tex) {
	void* pixels;
	int   pitch;
	SDL_LockTexture(tex, NULL, &pixels, &pitch);
	SDL_LockSpinlock(&uiBufferLock);
	memcpy(pixels, uiBuffer, height * pitch);
	SDL_SetAtomicInt(&blitUI, 0);
	SDL_UnlockSpinlock(&uiBufferLock);
	SDL_UnlockTexture(tex);
}

/*
 Blits the NeXT framebuffer to the fbTexture, blends with the GUI surface and shows it.
 */
static bool Screen_SingleRepaint(void) {
	bool updateScreen = false;

	/* Blit the NeXT framebuffer to texture */
	if (bEmulationActive) {
		updateScreen = blitScreen(ConfigureParams.Screen.nSingleModeSlot, fbTexture);
	}

	/* Copy UI surface to texture */
	if (SDL_GetAtomicInt(&blitUI)) {
		blitUserInterface(uiTexture);
		updateScreen = true;
	}

	if (updateScreen) {
		SDL_RenderClear(sdlRenderer);
		/* Render NeXT framebuffer texture */
		SDL_RenderTexture(sdlRenderer, fbTexture, NULL, &screenRect);
		SDL_RenderTexture(sdlRenderer, uiTexture, NULL, &screenRect);
		/* Sleeps until next VSYNC if enabled in ScreenInit */
		SDL_RenderPresent(sdlRenderer);
	}

	return updateScreen;
}

static bool Screen_GroupRepaint(void) {
	bool updateScreen = false;
	int i;
	
	/* Blit the NeXT framebuffer to texture */
	if (bEmulationActive) {
		for (i = 0; i < NUM_MONITORS; i++) {
			if (fbGroupTexture[i]) {
				if (blitScreen(i * 2, fbGroupTexture[i])) {
					updateScreen = true;
				}
			}
		}
	}
	
	/* Copy UI surface to texture */
	if (SDL_GetAtomicInt(&blitUI)) {
		blitUserInterface(uiTexture);
		updateScreen = true;
	}
	
	if (updateScreen) {
		SDL_RenderClear(sdlRenderer);
		/* Render NeXT framebuffer texture */
		for (i = 0; i < NUM_MONITORS; i++) {
			if (fbGroupTexture[i]) {
				SDL_RenderTexture(sdlRenderer, fbGroupTexture[i], NULL, &fbRect[i]);
			}
		}
		SDL_RenderTexture(sdlRenderer, uiTexture, NULL, &screenRect);
		/* Sleeps until next VSYNC if enabled in ScreenInit */
		SDL_RenderPresent(sdlRenderer);
	}
	
	return updateScreen;
}

bool Screen_Repaint(void) {
	if (initScreenMode == SCREEN_GROUP) {
		return Screen_GroupRepaint();
	}
	return Screen_SingleRepaint();
}

#ifdef ENABLE_RENDERING_THREAD
static int repainter(void* unused) {
	SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);

	/* Enter repaint loop */
	while (doRepaint) {
		if (!Screen_SingleRepaint()) {
			SDL_Delay(10);
		}
	}
	return 0;
}

static int group_repainter(void* unused) {
	SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
	
	while (doRepaint) {
		if (!Screen_GroupRepaint()) {
			SDL_Delay(10);
		}
	}
	return 0;
}
#endif

/*-----------------------------------------------------------------------*/
/**
 * Set Previous window title. Use NULL for default
 */
static void Screen_SetTitle(const char *title) {
	if (title)
		SDL_SetWindowTitle(sdlWindow, title);
	else
		SDL_SetWindowTitle(sdlWindow, PROG_NAME);
}

/*-----------------------------------------------------------------------*/
/**
 * Create texture with default parameters.
 */
static SDL_Texture* Screen_CreateFramebufferTexture(SDL_PixelFormat format, int width, int height) {
	SDL_Texture* tex;
	
	tex = SDL_CreateTexture(sdlRenderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
	if (tex == NULL) {
		Main_ErrorExit("Failed to create texture:", SDL_GetError(), -1);
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
	
	return tex;
}

/*-----------------------------------------------------------------------*/
/**
 * Init Screen, create window, renderer and textures
 */
void Screen_Init(void) {
	SDL_PixelFormat format;
	uint32_t r, g, b, a;
	int      d, i;
	
	initScreenMode = ConfigureParams.Screen.nMode;
	
	/* Set initial window resolution */
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		const int w = NeXT_SCRN_WIDTH;
		const int h = NeXT_SCRN_HEIGHT;
		int xmax = 0;
		int ymax = 0;
		
		int xpos, ypos;
		
		for (i = 0; i < NUM_MONITORS; i++) {
			if (ConfigureParams.Screen.nGroupModePos[i] >= 0) {
				assert(ConfigureParams.Screen.nGroupModePos[i] < (NUM_MONITORS * NUM_MONITORS));
				
				xpos = ConfigureParams.Screen.nGroupModePos[i] % NUM_MONITORS;
				ypos = ConfigureParams.Screen.nGroupModePos[i] / NUM_MONITORS;
				
				xmax = xpos > xmax ? xpos : xmax;
				ymax = ypos > ymax ? ypos : ymax;
				
				fbRect[i].w = w;
				fbRect[i].h = h;
				fbRect[i].x = xpos * w;
				fbRect[i].y = ypos * h;
			}
		}
		screen_x = (xmax + 1) * w;
		screen_y = (ymax + 1) * h;
	} else {
		screen_x = NeXT_SCRN_WIDTH;
		screen_y = NeXT_SCRN_HEIGHT;
	}

	width  = screen_x;
	height = screen_y;
	
	/* Statusbar */
	Statusbar_SetHeight(width, height, true);
	statusBar.x = 0;
	statusBar.y = height;
	statusBar.w = width;
	statusBar.h = Statusbar_GetHeight();
	/* Grow to fit statusbar */
	height += Statusbar_GetHeight();

	/* Screen */
	screenRect.x = 0;
	screenRect.y = 0;
	screenRect.h = height;
	screenRect.w = width;

	/* Set new video mode */
	fprintf(stderr, "SDL screen request: %d x %d (%s)\n", width, height, bInFullScreen ? "fullscreen" : "windowed");

	sdlWindow = SDL_CreateWindow(PROG_NAME, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!sdlWindow) {
		Main_ErrorExit("Failed to create window:", SDL_GetError(), -1);
	}
	SDL_SetWindowAspectRatio(sdlWindow, (float)width/height, (float)width/height);

	sdlRenderer = SDL_CreateRenderer(sdlWindow, NULL);
	if (!sdlRenderer) {
		Main_ErrorExit("Failed to create renderer:", SDL_GetError(), -1);
	}
#ifdef ENABLE_RENDERING_THREAD
	SDL_SetRenderVSync(sdlRenderer, 1);
#endif
	SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_STRETCH);

	format = SDL_PIXELFORMAT_BGRA32;

	uiTexture = SDL_CreateTexture(sdlRenderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
	if (!uiTexture) {
		Main_ErrorExit("Failed to create texture:", SDL_GetError(), -1);
	}
	SDL_SetTextureBlendMode(uiTexture, SDL_BLENDMODE_BLEND);

	SDL_GetMasksForPixelFormat(format, &d, &r, &g, &b, &a);

	sdlscrn = SDL_CreateSurface(width, height, format);

	/* Exit if we can not open a screen */
	if (!sdlscrn) {
		Main_ErrorExit("Could not set video mode:", SDL_GetError(), -2);
	}

	/* Clear UI with mask */
	mask = g | a;
	SDL_FillSurfaceRect(sdlscrn, NULL, mask);

	/* Allocate buffers for copy routines */
	uiBuffer = calloc(1, sdlscrn->h * sdlscrn->pitch);

	/* Initialize statusbar */
	Statusbar_Init(sdlscrn);

	/* Setup lookup tables */
	/* initialize BW lookup table */
	for (i = 0; i < 0x100; i++) {
		BW2RGB[i*4+0] = bw2rgb(sdlscrn, i>>6);
		BW2RGB[i*4+1] = bw2rgb(sdlscrn, i>>4);
		BW2RGB[i*4+2] = bw2rgb(sdlscrn, i>>2);
		BW2RGB[i*4+3] = bw2rgb(sdlscrn, i>>0);
	}
	/* initialize color lookup table */
	for (i = 0; i < 0x10000; i++)
		COL2RGB[SDL_BYTEORDER == SDL_BIG_ENDIAN ? i : SDL_Swap16(i)] = col2rgb(sdlscrn, i);

	/* Create framebuffer textures and start with blank screen */
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		for (i = 0; i < NUM_MONITORS; i++) {
			if (ConfigureParams.Screen.nGroupModePos[i] >= 0) {
				fbGroupTexture[i] = Screen_CreateFramebufferTexture(format, NeXT_SCRN_WIDTH, NeXT_SCRN_HEIGHT);
				Screen_Blank(fbGroupTexture[i]);
			}
		}
	} else {
		fbTexture = Screen_CreateFramebufferTexture(format, width, height);
		Screen_Blank(fbTexture);
	}

#ifdef ENABLE_RENDERING_THREAD
	/* Start repaint thread */
	doRepaint = true;
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP) {
		repaintThread = SDL_CreateThread(group_repainter, "[Previous] Screen group", NULL);
	} else {
		repaintThread = SDL_CreateThread(repainter, "[Previous] Screen at slot 0", NULL);
	}
#endif

	/* Configure some SDL stuff: */
	Screen_SetTitle(NULL);
	Screen_ShowCursor(false);
	Screen_SetMouseGrab(bGrabMouse);

	if (!ConfigureParams.Screen.bShowStatusbar) {
		Screen_StatusbarChanged();
	}
	if (!ConfigureParams.Screen.bShowTitlebar) {
		Screen_TitlebarChanged();
	}
	if (ConfigureParams.Screen.bFullScreen) {
		Screen_EnterFullScreen();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Free screen bitmap and allocated resources
 */
void Screen_UnInit(void) {
	int i;
#ifdef ENABLE_RENDERING_THREAD
	int s;
	doRepaint = false; /* stop repaint thread */
	SDL_WaitThread(repaintThread, &s);
	repaintThread = NULL;
#endif
	free(uiBuffer);
	SDL_DestroySurface(sdlscrn);
	sdlscrn = NULL;
	SDL_DestroyTexture(uiTexture);
	SDL_DestroyTexture(fbTexture);
	uiTexture = fbTexture = NULL;
	for (i = 0; i < NUM_MONITORS; i++) {
		SDL_DestroyTexture(fbGroupTexture[i]);
		fbGroupTexture[i] = NULL;
	}
	SDL_DestroyRenderer(sdlRenderer);
	sdlRenderer = NULL;
	SDL_DestroyWindow(sdlWindow);
	sdlWindow = NULL;
}

/*-----------------------------------------------------------------------*/
/**
 * Re-initialise screen group or handle mode change 
 */
void Screen_Reset(void) {
	if (ConfigureParams.Screen.nMode == SCREEN_GROUP || initScreenMode == SCREEN_GROUP) {
		Screen_UnInit();
		Screen_Init();
	} else {
		Screen_ModeChanged();
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Enter Full screen mode
 */
void Screen_EnterFullScreen(void) {
	bool bWasRunning;

	if (!bInFullScreen) {
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = true;

		SDL_GetWindowPosition(sdlWindow, &saveWindowBounds.x, &saveWindowBounds.y);
		SDL_GetWindowSize(sdlWindow, &saveWindowBounds.w, &saveWindowBounds.h);
		SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetWindowFullscreen(sdlWindow, true);
		SDL_Delay(100);                  /* To give monitor time to change to new resolution */

		/* If using multiple screen windows, save and go to single window mode */
		saveScreenMode = ConfigureParams.Screen.nMode;
		if (ConfigureParams.Screen.nMode == SCREEN_ALL) {
			ConfigureParams.Screen.nMode = SCREEN_SINGLE;
			Screen_ModeChanged();
		}

		if (bWasRunning) {
			/* And off we go... */
			Main_UnPauseEmulation();
		}

		/* Always grab mouse pointer in full screen mode */
		Screen_SetMouseGrab(true);

		/* Make sure screen is painted in case emulation is paused */
		SDL_SetAtomicInt(&blitUI, 1);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Return from Full screen mode back to a window
 */
void Screen_ReturnFromFullScreen(void) {
	bool bWasRunning;

	if (bInFullScreen) {
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = false;

		SDL_SetWindowFullscreen(sdlWindow, false);
		SDL_Delay(100);                /* To give monitor time to switch resolution */
		SDL_SetWindowSize(sdlWindow, saveWindowBounds.w, saveWindowBounds.h);
		SDL_SetWindowPosition(sdlWindow, saveWindowBounds.x, saveWindowBounds.y);
		SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, SDL_LOGICAL_PRESENTATION_STRETCH);

		/* Return to windowed monitor mode */
		if (saveScreenMode == SCREEN_ALL) {
			ConfigureParams.Screen.nMode = saveScreenMode;
			Screen_ModeChanged();
		}

		if (bWasRunning) {
			/* And off we go... */
			Main_UnPauseEmulation();
		}

		/* Go back to windowed mode mouse grab settings */
		Screen_SetMouseGrab(bGrabMouse);

		/* Make sure screen is painted in case emulation is paused */
		SDL_SetAtomicInt(&blitUI, 1);
	}
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse grab.
 */
void Screen_SetMouseGrab(bool grab) {
	/* If emulation is active, set the mouse cursor mode now: */
	if (grab) {
		if (bEmulationActive) {
			Screen_CenterCursor(); /* Cursor must be inside window */
			SDL_SetWindowRelativeMouseMode(sdlWindow, true);
			SDL_SetWindowKeyboardGrab(sdlWindow, true);
			SDL_SetWindowMouseGrab(sdlWindow, true);
			if (ConfigureParams.Mouse.bEnableAutoGrab) {
				Screen_SetTitle("Mouse is locked. Ctrl-click to release.");
			} else {
				char message[64];
				
				snprintf(message, sizeof(message), "Mouse is locked. Press ctrl-alt-%s to release.", 
						 Keymap_GetKeyName(ConfigureParams.Shortcut.withModifier[SHORTCUT_MOUSEGRAB]));
				Screen_SetTitle(message);
			}
		}
	} else {
		SDL_SetWindowRelativeMouseMode(sdlWindow, false);
		SDL_SetWindowKeyboardGrab(sdlWindow, false);
		SDL_SetWindowMouseGrab(sdlWindow, false);
		Screen_SetTitle(NULL);
	}
}

void Screen_StatusbarUpdate(void) {
	Statusbar_Update(sdlscrn);
}

/*-----------------------------------------------------------------------*/
/**
 * Show main window
 */
void Screen_ShowMainWindow(void) {
	if (!bInFullScreen) {
		SDL_RestoreWindow(sdlWindow);
		SDL_RaiseWindow(sdlWindow);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing screen size
 */
void Screen_SizeChanged(void) {
	int h;

	if (!bInFullScreen) {
		SDL_GetWindowSize(sdlWindow, NULL, &h);
		nd_sdl_resize((float)h/height);
	}

	/* Make sure screen is painted in case emulation is paused */
	SDL_SetAtomicInt(&blitUI, 1);
}


/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing between fullscreen/windowed
 */
void Screen_ModeChanged(void) {
	if (!sdlscrn) {
		/* screen not yet initialized */
		return;
	}

	/* Do not use multiple windows in full screen mode */
	if (ConfigureParams.Screen.nMode == SCREEN_ALL && bInFullScreen) {
		saveScreenMode = ConfigureParams.Screen.nMode;
		ConfigureParams.Screen.nMode = SCREEN_SINGLE;
	}
	if (ConfigureParams.Screen.nMode == SCREEN_ALL) {
		nd_sdl_show();
	} else {
		nd_sdl_hide();
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Set visibilty of title bar.
 */
void Screen_TitlebarChanged(void) {
	if (sdlscrn && !bInFullScreen) {
		SDL_SetWindowBordered(sdlWindow, ConfigureParams.Screen.bShowTitlebar);
		nd_sdl_titlebar(ConfigureParams.Screen.bShowTitlebar);
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Force things associated with changing statusbar visibility
 */
void Screen_StatusbarChanged(void) {
	int w;
	SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_STRETCH;

	if (!sdlscrn) {
		/* screen not yet initialized */
		return;
	}

	/* Get new heigt for our window */
	height = screen_y + Statusbar_SetHeight(width, height, false);

	if (bInFullScreen) {
		saveWindowBounds.h = (height * saveWindowBounds.w) / width;
		mode = SDL_LOGICAL_PRESENTATION_LETTERBOX;
	}
	SDL_SetRenderLogicalPresentation(sdlRenderer, width, height, mode);
	SDL_GetWindowSize(sdlWindow, &w, NULL);
	SDL_SetWindowAspectRatio(sdlWindow, (float)width/height, (float)width/height);
	SDL_SetWindowSize(sdlWindow, w, (int)SDL_lroundf((float)(height*w)/width));

	/* Make sure screen is painted in case emulation is paused */
	SDL_SetAtomicInt(&blitUI, 1);
}

/*-----------------------------------------------------------------------*/
/**
 * Wrapper for Statusbar_AddMessage() and Statusbar_Update() in one go.
 */
void Screen_StatusbarMessage(const char *msg, uint32_t msecs)
{
	Statusbar_AddMessage(msg, msecs);
	Statusbar_Update(sdlscrn);
}

/*-----------------------------------------------------------------------*/
/**
 * Update status bar and force screen repaint.
 */
static void statusBarUpdate(void) {
	SDL_LockSurface(sdlscrn);
	SDL_LockSpinlock(&uiBufferLock);
	memcpy(&((uint8_t*)uiBuffer)[statusBar.y*sdlscrn->pitch], &((uint8_t*)sdlscrn->pixels)[statusBar.y*sdlscrn->pitch], statusBar.h * sdlscrn->pitch);
	SDL_SetAtomicInt(&blitUI, 1);
	SDL_UnlockSpinlock(&uiBufferLock);
	SDL_UnlockSurface(sdlscrn);
}

/*-----------------------------------------------------------------------*/
/**
 * Copy UI surface to buffer and replace mask pixels with transparent 
 * pixels for UI blending with framebuffer texture.
 */
static void uiUpdate(void) {
	SDL_LockSurface(sdlscrn);
	int     count = sdlscrn->w * sdlscrn->h;
	uint32_t* dst = (uint32_t*)uiBuffer;
	uint32_t* src = (uint32_t*)sdlscrn->pixels;
	SDL_LockSpinlock(&uiBufferLock);
	/* poor man's green-screen - would be nice if SDL had more blending modes... */
	for(int i = count; --i >= 0; src++)
		*dst++ = *src == mask ? 0 : *src;
	SDL_SetAtomicInt(&blitUI, 1);
	SDL_UnlockSpinlock(&uiBufferLock);
	SDL_UnlockSurface(sdlscrn);
}

void Screen_UpdateRects(SDL_Surface *screen, int numrects, SDL_Rect *rects) {
	while(numrects--) {
		if(rects->y < NeXT_SCRN_HEIGHT) {
			uiUpdate();
			doUIblit = true;
		} else {
			if(doUIblit) {
				uiUpdate();
				doUIblit = false;
			} else {
				statusBarUpdate();
			}
		}
	}
#ifndef ENABLE_RENDERING_THREAD
	if (!bEmulationActive) {
		Screen_Repaint();
	}
#endif
}

void Screen_UpdateRect(SDL_Surface *screen, int32_t x, int32_t y, int32_t w, int32_t h) {
	SDL_Rect rect = { x, y, w, h };
	Screen_UpdateRects(screen, 1, &rect);
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse cursor visibility and return if it was visible before.
 */
bool Screen_ShowCursor(bool show) {
	bool bOldVisibility;
	
	bOldVisibility = SDL_CursorVisible();
	if (bOldVisibility != show) {
		if (show) {
			SDL_ShowCursor();
		} else {
			SDL_HideCursor();
		}
	}
	return bOldVisibility;
}

/* ----------------------------------------------------------------------- */
/**
 * Set mouse cursor to the center of the screen.
 */
void Screen_CenterCursor(void) {
	SDL_WarpMouseInWindow(sdlWindow, sdlscrn->w/2, sdlscrn->h/2);
	GuiEvent_WarpMouse();
}
