/*
  Previous - dlgMouse.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/
const char DlgMouse_fileid[] = "Previous dlgMouse.c";

#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "sdlgui.h"
#include "file.h"
#include "paths.h"


#define DLGMOUSE_CUSTOMISE       3
#define DLGMOUSE_UNLOCK_VERYSLOW 6
#define DLGMOUSE_UNLOCK_SLOW     7
#define DLGMOUSE_UNLOCK_NORMAL   8
#define DLGMOUSE_UNLOCK_FAST     9
#define DLGMOUSE_UNLOCK_VERYFAST 10
#define DLGMOUSE_UNLOCK_CUSTOM   11

#define DLGMOUSE_LOCK_VERYSLOW   14
#define DLGMOUSE_LOCK_SLOW       15
#define DLGMOUSE_LOCK_NORMAL     16
#define DLGMOUSE_LOCK_FAST       17
#define DLGMOUSE_LOCK_VERYFAST   18
#define DLGMOUSE_LOCK_CUSTOM     19

#define DLGMOUSE_CTRLCLCK        21
#define DLGMOUSE_MAPTOKEY        22
#define DLGMOUSE_AUTOLOCK        23
#define DLGMOUSE_EXIT            24

/* The mouse options dialog: */
static SGOBJ mousedlg[] =
{
	{ SGBOX, 0, 0, 0,0, 45,27, NULL },
	{ SGTEXT, 0, 0, 16,1, 13,1, "Mouse options" },
	
	{ SGTEXT, 0, 0, 2,4, 30,1, "Mouse motion speed adjustment:" },
	{ SGBUTTON, 0, 0, 33,4, 11,1, "Customise" },
	
	{ SGBOX, 0, 0, 1,6, 21,10, NULL },
	{ SGTEXT, 0, 0, 2,7, 30,1, "Slow movement:" },
	{ SGRADIOBUT, 0, 0, 3,9,  11,1, "Very slow" },
	{ SGRADIOBUT, 0, 0, 3,10,  6,1, "Slow" },
	{ SGRADIOBUT, 0, 0, 3,11,  8,1, "Normal" },
	{ SGRADIOBUT, 0, 0, 3,12,  6,1, "Fast" },
	{ SGRADIOBUT, 0, 0, 3,13, 11,1, "Very fast" },
	{ SGRADIOBUT, 0, 0, 3,14, 12,1, "Customised" },
	
	{ SGBOX, 0, 0, 23,6, 21,10, NULL },
	{ SGTEXT, 0, 0, 24,7, 30,1, "Fast movement:" },
	{ SGRADIOBUT, 0, 0, 25,9,  11,1, "Very slow" },
	{ SGRADIOBUT, 0, 0, 25,10,  6,1, "Slow" },
	{ SGRADIOBUT, 0, 0, 25,11,  8,1, "Normal" },
	{ SGRADIOBUT, 0, 0, 25,12,  6,1, "Fast" },
	{ SGRADIOBUT, 0, 0, 25,13, 11,1, "Very fast" },
	{ SGRADIOBUT, 0, 0, 25,14, 12,1, "Customised" },
	
	{ SGBOX, 0, 0, 1,17, 43,5, NULL },
	{ SGCHECKBOX, 0, 0, 2,18, 34,1, "Map control-click to right-click" },
	{ SGCHECKBOX, 0, 0, 2,19, 32,1, "Map scroll wheel to arrow keys" },
	{ SGCHECKBOX, 0, 0, 2,20, 21,1, "Enable auto-locking" },
	
	{ SGBUTTON, SG_DEFAULT, 0, 12,24, 21,1, "Back to main menu" },
	{ SGSTOP, 0, 0, 0,0, 0,0, NULL }
};

#define DLGSPEED_EXIT 18

static char n_lin_string[8];
static char n_exp_string[8];
static char l_lin_string[8];
static char l_exp_string[8];

/* The mouse speed adjustment dialog */
static SGOBJ speeddlg[] =
{
	{ SGBOX, 0, 0, 0,0, 48,25, NULL },
	{ SGTEXT, 0, 0, 16,1, 13,1, "Mouse speed scale" },
	
	{ SGTEXT, 0, 0, 2,4, 32,1, "Unlocked window mode:" },
	{ SGBOX, 0, 0, 1,6, 46,5, NULL },
	{ SGTEXT, 0, 0, 2,7, 32,1, "Linear adjustment:" },
	{ SGEDITFIELD, 0, 0, 26,7, 5,1, n_lin_string },
	{ SGTEXT, 0, 0, 32,7, 32,1, "(0.01 to 10.0)" },
	{ SGTEXT, 0, 0, 2,9, 38,1, "Exponential adjustment:" },
	{ SGEDITFIELD, 0, 0, 26,9, 5,1, n_exp_string },
	{ SGTEXT, 0, 0, 32,9, 32,1, "(0.50 to 1.00)" },
	
	{ SGTEXT, 0, 0, 2,13, 32,1, "Locked or fullscreen mode:" },
	{ SGBOX, 0, 0, 1,15, 46,5, NULL },
	{ SGTEXT, 0, 0, 2,16, 32,1, "Linear adjustment:" },
	{ SGEDITFIELD, 0, 0, 26,16, 5,1, l_lin_string },
	{ SGTEXT, 0, 0, 32,16, 32,1, "(0.01 to 10.0)" },
	{ SGTEXT, 0, 0, 2,18, 38,1, "Exponential adjustment:" },
	{ SGEDITFIELD, 0, 0, 26,18, 5,1, l_exp_string },
	{ SGTEXT, 0, 0, 32,18, 32,1, "(0.50 to 1.00)" },

	{ SGBUTTON, SG_DEFAULT, 0, 19,22, 10,1, "Done" },
	{ SGSTOP, 0, 0, 0,0, 0,0, NULL }
};


static float read_float_string(char *s, float min, float max, int prec)
{
	int i;
	float result=0.0;
	
	for (i=0; i<8; i++) {
		if (*s>=(0+'0') && *s<=(9+'0')) {
			result *= 10.0;
			result += (float)(*s-'0');
			s++;
		} else {
			if (i==0 && *s!='.' && *s!=',') /* bad input, default to 1.0 */
				result=1.0;
			break;
		}
	}

	if (*s == '.' || *s == ',') {
		s++;
		for (i=1; i<=prec; i++) {
			if (*s>=(0+'0') && *s<=(9+'0')) {
				result += (float)(*s-'0')/pow(10.0, i);
				s++;
			} else {
				if (result==0.0) { /* bad input, default to 1.0 */
					result=1.0;
				}
				break;
			}
		}
		if (*s>=(0+'0') && *s<=(9+'0')) {
			if ((*s-'0')>=5) {
				result += 1.0/pow(10.0, i-1);
			}
		}
	}

	if (result<min)
		result=min;
	if (result>max)
		result=max;
	
	return result;
}

static void Dialog_SpeedDlg(float* lin, float* exp)
{
	int but;
	
	SDLGui_CenterDlg(speeddlg);
	
	/* Set up the dialog from actual values */
	snprintf(n_lin_string, sizeof(n_lin_string), "%#.2f", ConfigureParams.Mouse.fLinSpeedNormal);	
	snprintf(n_exp_string, sizeof(n_exp_string), "%#.2f", ConfigureParams.Mouse.fExpSpeedNormal);
	snprintf(l_lin_string, sizeof(l_lin_string), "%#.2f", ConfigureParams.Mouse.fLinSpeedLocked);	
	snprintf(l_exp_string, sizeof(l_exp_string), "%#.2f", ConfigureParams.Mouse.fExpSpeedLocked);
	
	/* Draw and process the dialog */
	do
	{
		but = SDLGui_DoDialog(speeddlg);
	}
	while (but != DLGSPEED_EXIT && but != SDLGUI_QUIT && but != SDLGUI_ERROR && !bQuitProgram);
	
	ConfigureParams.Mouse.fLinSpeedNormal = read_float_string(n_lin_string, MOUSE_LIN_MIN, MOUSE_LIN_MAX, 2);
	ConfigureParams.Mouse.fExpSpeedNormal = read_float_string(n_exp_string, MOUSE_EXP_MIN, MOUSE_EXP_MAX, 2);
	ConfigureParams.Mouse.fLinSpeedLocked = read_float_string(l_lin_string, MOUSE_LIN_MIN, MOUSE_LIN_MAX, 2);
	ConfigureParams.Mouse.fExpSpeedLocked = read_float_string(l_exp_string, MOUSE_EXP_MIN, MOUSE_EXP_MAX, 2);
}


#define LIN_BASE     0.125
#define LIN_VERYFAST (8.0 * LIN_BASE)
#define LIN_FAST     (7.0 * LIN_BASE)
#define LIN_NORMAL   (6.0 * LIN_BASE)
#define LIN_SLOW     (5.0 * LIN_BASE)
#define LIN_VERYSLOW (4.0 * LIN_BASE)

#define EXP_BASE     0.125
#define EXP_VERYFAST (8.0 * EXP_BASE)
#define EXP_FAST     (7.0 * EXP_BASE)
#define EXP_NORMAL   (6.0 * EXP_BASE)
#define EXP_SLOW     (5.0 * EXP_BASE)
#define EXP_VERYSLOW (4.0 * EXP_BASE)

/* Set up the dialog from actual values */
static void DlgMouseSetup(void)
{
	int i;
	
	for (i = DLGMOUSE_UNLOCK_VERYSLOW; i <= DLGMOUSE_UNLOCK_CUSTOM; i++) {
		mousedlg[i].state &= ~SG_SELECTED;
	}
	for (i = DLGMOUSE_LOCK_VERYSLOW; i <= DLGMOUSE_LOCK_CUSTOM; i++) {
		mousedlg[i].state &= ~SG_SELECTED;
	}
	mousedlg[DLGMOUSE_CTRLCLCK].state &= ~SG_SELECTED;
	mousedlg[DLGMOUSE_MAPTOKEY].state &= ~SG_SELECTED;
	mousedlg[DLGMOUSE_AUTOLOCK].state &= ~SG_SELECTED;
	
	if (ConfigureParams.Mouse.fLinSpeedLocked == LIN_VERYSLOW) {
		mousedlg[DLGMOUSE_UNLOCK_VERYSLOW].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fLinSpeedLocked == LIN_SLOW) {
		mousedlg[DLGMOUSE_UNLOCK_SLOW].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fLinSpeedLocked == LIN_NORMAL) {
		mousedlg[DLGMOUSE_UNLOCK_NORMAL].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fLinSpeedLocked == LIN_FAST) {
		mousedlg[DLGMOUSE_UNLOCK_FAST].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fLinSpeedLocked == LIN_VERYFAST) {
		mousedlg[DLGMOUSE_UNLOCK_VERYFAST].state |= SG_SELECTED;
	} else {
		mousedlg[DLGMOUSE_UNLOCK_CUSTOM].state |= SG_SELECTED;
	}
	
	if (ConfigureParams.Mouse.fExpSpeedLocked == EXP_VERYSLOW) {
		mousedlg[DLGMOUSE_LOCK_VERYSLOW].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fExpSpeedLocked == EXP_SLOW) {
		mousedlg[DLGMOUSE_LOCK_SLOW].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fExpSpeedLocked == EXP_NORMAL) {
		mousedlg[DLGMOUSE_LOCK_NORMAL].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fExpSpeedLocked == EXP_FAST) {
		mousedlg[DLGMOUSE_LOCK_FAST].state |= SG_SELECTED;
	} else if (ConfigureParams.Mouse.fExpSpeedLocked == EXP_VERYFAST) {
		mousedlg[DLGMOUSE_LOCK_VERYFAST].state |= SG_SELECTED;
	} else {
		mousedlg[DLGMOUSE_LOCK_CUSTOM].state |= SG_SELECTED;
	}
	
	if (ConfigureParams.Mouse.bEnableMacClick) {
		mousedlg[DLGMOUSE_CTRLCLCK].state |= SG_SELECTED;
	}
	if (ConfigureParams.Mouse.bEnableMapToKey) {
		mousedlg[DLGMOUSE_MAPTOKEY].state |= SG_SELECTED;
	}
	if (ConfigureParams.Mouse.bEnableAutoGrab) {
		mousedlg[DLGMOUSE_AUTOLOCK].state |= SG_SELECTED;
	}
}

/* Read values from dialog */
static void DlgMouseRead(void)
{
	if (mousedlg[DLGMOUSE_UNLOCK_VERYSLOW].state&SG_SELECTED) {
		ConfigureParams.Mouse.fLinSpeedLocked = LIN_VERYSLOW;
	} else if (mousedlg[DLGMOUSE_UNLOCK_SLOW].state&SG_SELECTED) {
		ConfigureParams.Mouse.fLinSpeedLocked = LIN_SLOW;
	} else if (mousedlg[DLGMOUSE_UNLOCK_NORMAL].state&SG_SELECTED) {
		ConfigureParams.Mouse.fLinSpeedLocked = LIN_NORMAL;
	} else if (mousedlg[DLGMOUSE_UNLOCK_FAST].state&SG_SELECTED) {
		ConfigureParams.Mouse.fLinSpeedLocked = LIN_FAST;
	} else if (mousedlg[DLGMOUSE_UNLOCK_VERYFAST].state&SG_SELECTED) {
		ConfigureParams.Mouse.fLinSpeedLocked = LIN_VERYFAST;
	}
	if (mousedlg[DLGMOUSE_LOCK_VERYSLOW].state&SG_SELECTED) {
		ConfigureParams.Mouse.fExpSpeedLocked = EXP_VERYSLOW;
	} else if (mousedlg[DLGMOUSE_LOCK_SLOW].state&SG_SELECTED) {
		ConfigureParams.Mouse.fExpSpeedLocked = EXP_SLOW;
	} else if (mousedlg[DLGMOUSE_LOCK_NORMAL].state&SG_SELECTED) {
		ConfigureParams.Mouse.fExpSpeedLocked = EXP_NORMAL;
	} else if (mousedlg[DLGMOUSE_LOCK_FAST].state&SG_SELECTED) {
		ConfigureParams.Mouse.fExpSpeedLocked = EXP_FAST;
	} else if (mousedlg[DLGMOUSE_LOCK_VERYFAST].state&SG_SELECTED) {
		ConfigureParams.Mouse.fExpSpeedLocked = EXP_VERYFAST;
	}
	ConfigureParams.Mouse.bEnableMacClick = mousedlg[DLGMOUSE_CTRLCLCK].state&SG_SELECTED ? true : false;
	ConfigureParams.Mouse.bEnableMapToKey = mousedlg[DLGMOUSE_MAPTOKEY].state&SG_SELECTED ? true : false;
	ConfigureParams.Mouse.bEnableAutoGrab = mousedlg[DLGMOUSE_AUTOLOCK].state&SG_SELECTED ? true : false;
}

/*-----------------------------------------------------------------------*/
/**
 * Show and process the Mouse options dialog.
 */
void Dialog_MouseDlg(void)
{
	int but;

	SDLGui_CenterDlg(mousedlg);

	/* Draw and process the dialog */
	do
	{
		DlgMouseSetup();
		
		but = SDLGui_DoDialog(mousedlg);
		
		DlgMouseRead();
		
		if (but == DLGMOUSE_CUSTOMISE) {
			Dialog_SpeedDlg(NULL, NULL);
		}
	}
	while (but != DLGMOUSE_EXIT && but != SDLGUI_QUIT && but != SDLGUI_ERROR && !bQuitProgram);
}
