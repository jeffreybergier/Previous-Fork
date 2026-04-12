/*
  Previous - tablet.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This file contains a simulation of the SummaGraphics MM 1201 and MM 961 
  digitizers (later models also known as SummaSketch graphics tablets).
*/
const char Tablet_fileid[] = "Previous tablet.c";

#include "main.h"
#include "configuration.h"
#include "m68000.h"
#include "statusbar.h"
#include "scc.h"
#include "tablet.h"

#define LOG_TABLET_LEVEL      LOG_WARN
#define LOG_TABLET_DATA_LEVEL LOG_DEBUG


/* Flag indicating that host has initialised for tablet input */
bool bTabletEnabled = false;

/* Commands from host to tablet */
#define SUMMA_SETBAUDRATE  0x20 /* SP */

/* for model 961 */
#define SUMMA_HORIZONTAL   0x62 /* b */
#define SUMMA_VERTICAL     0x63 /* c */
/* for model 1201 */
#define SUMMA_UPPERLEFT    0x62 /* b */
#define SUMMA_LOWERLEFT    0x63 /* c */

#define SUMMA_COO_RATE2    0x54 /* T */
#define SUMMA_COO_RATE20   0x53 /* S */
#define SUMMA_COO_RATE50   0x52 /* R */
#define SUMMA_COO_RATE100  0x51 /* Q */

#define SUMMA_STREAM       0x40 /* @ */
#define SUMMA_SWITCHSTREAM 0x41 /* A */
#define SUMMA_POINT        0x42 /* B */
#define SUMMA_REMOTEREQ    0x44 /* D */
#define SUMMA_REQUEST      0x50 /* P */
#define SUMMA_DELTA        0x45 /* E */
#define SUMMA_AXISUPDATE   0x47 /* G followed by one byte update value plus 32 */
#define SUMMA_INCREMENT    0x49 /* I followed by one byte increment value plus 32 */

#define SUMMA_RES_10       0x66 /* f */
#define SUMMA_RES_20       0x69 /* i */
#define SUMMA_RES_40       0x71 /* q */
#define SUMMA_RES_100      0x64 /* d */
#define SUMMA_RES_200      0x65 /* e */
#define SUMMA_RES_400      0x67 /* g */
#define SUMMA_RES_500      0x68 /* h */
#define SUMMA_RES_1000     0x6A /* j */

#define SUMMA_ROUNDOFF_1   0x6C /* l */
#define SUMMA_ROUNDOFF_2   0x6E /* n */
#define SUMMA_ROUNDOFF_4   0x70 /* p */

#define SUMMA_SETXYSCALE   0x72 /* r followed by two byte X and two byte Y scale values */

#define SUMMA_STARTTRANS   0x11 /* DC1 (XON) */
#define SUMMA_STOPTRANS    0x13 /* DC3 (XOFF) */

#define SUMMA_IDENTIFY0    0x30 /* 0 */
#define SUMMA_IDENTIFY1    0x31 /* 1 */

#define SUMMA_RESET        0x00 /* NUL */

#define SUMMA_STATUS       0x61 /* a */

#define SUMMA_SELFTEST     0x74 /* t */
#define SUMMA_TESTRESULTS  0x77 /* w */
#define SUMMA_ECHO         0x6B /* k */
#define SUMMA_CHECKCODE    0x78 /* x */
#define SUMMA_FACTORYTEST  0x7A /* z */


/* Output data */
/* Byte 0: */
#define SUMMA_PHASINGBIT  0x80 /* always 1 */
#define SUMMA_PROXIMITY   0x40 /* 1 = in proximity, 0 = out of proximity */
#define SUMMA_IDENTIFIER  0x20 /* user selected tabled identifier (0 or 1) */
#define SUMMA_X_SIGN      0x10 /* X coordinate sign, 1 = positive, 0 = negative */
#define SUMMA_Y_SIGN      0x08 /* Y coordinate sign, 1 = positive, 0 = negative */
#define SUMMA_BUTTON3     0x04 /* cursor button #3 / stylus N/A pressed */
#define SUMMA_BUTTON2     0x02 /* cursor button #2 / stylus barrel switch pressed */
#define SUMMA_BUTTON1     0x01 /* cursor button #1 / stylus pen tip pressed */
/* Byte 1 to 4:
 * |  normal mode              |  delta mode
 * 1: X coordinate low  7 bit  |  X coordinate 7 bit
 * 2: X coordiante high 7 bit  |  Y cooridnate 7 bit
 * 3: X coordinate low  7 bit  |  - (not transmitted)
 * 4: X coordiante high 7 bit  |  - (not transmitted)
 */
#define SUMMA_COORD_MASK  0x7F
#define SUMMA_COORD_SHIFT 7

/* Orientation */
#define SUMMA_ORIG_LOWER  0
#define SUMMA_ORIG_UPPER  1

/* Tablet size in tenth of an inch (avoid decimal numbers) */
#define SUMMA_1201_SIZE   117  /* 11.7 inch */
#define SUMMA_961_XSIZE   60   /* 6 inch */
#define SUMMA_961_YSIZE   90   /* 9 inch */

static struct summa_tablet {
	int enabled;
	int xresolution;
	int yresolution;
	int outputrate;
	uint8_t mode;
	uint8_t increment;
	uint8_t axisupdate;
	uint8_t origin;
	uint8_t selftest;
	uint8_t id;
	int baudrate;
	
	uint8_t command[10];
	uint8_t data[10];
	int cmdsize;
	int received;
	int statsize;
	int count;
	
	int32_t xsize;
	int32_t ysize;
	int32_t xmax;
	int32_t ymax;
	int32_t xpos;
	int32_t ypos;
	int32_t xdelta;
	int32_t ydelta;
	uint8_t flags;
	
	int32_t xscreen;
	int32_t yscreen;
} tablet;

static void tablet_set_bounds(void) {
	tablet.xmax = (tablet.xsize * tablet.xresolution) / 10;
	tablet.ymax = (tablet.ysize * tablet.yresolution) / 10;
}

static void tablet_reset(void) {
	/* Set default values: */
	tablet.enabled     = 1;
	tablet.xresolution = 500;
	tablet.yresolution = 500;
	tablet.outputrate  = 100;
	tablet.mode        = SUMMA_SWITCHSTREAM;
	tablet.increment   = 0;
	tablet.axisupdate  = 0;
	tablet.origin      = SUMMA_ORIG_LOWER;
	tablet.selftest    = 0x4F;
	tablet.id          = 0;
	tablet.baudrate    = 9600;
	
	/* Set bounds: */
	if (ConfigureParams.Tablet.nTabletType == TABLET_MM961) {
		tablet.xsize = SUMMA_961_XSIZE;
		tablet.ysize = SUMMA_961_YSIZE;
	} else { /* MM1201 */
		tablet.xsize = SUMMA_1201_SIZE;
		tablet.ysize = SUMMA_1201_SIZE;
	}
	tablet.xscreen = 1120;
	tablet.yscreen = 832;
	tablet_set_bounds();
	
	/* Set constant flags: */
	tablet.flags = SUMMA_PHASINGBIT;
	if (tablet.id) {
		tablet.flags |= SUMMA_IDENTIFIER;
	}
}

static void tablet_send(uint8_t val) {
	Log_Printf(LOG_TABLET_DATA_LEVEL, "[Tablet] Sending %02x", val);
	scc_receive(1, val);
}

static void tablet_send_data(int size) {
	tablet.statsize = tablet.count = size;
	CycInt_AddTimeEvent(1000, 0, EVENT_TABLET_IO);
}

static void tablet_set_origin(uint8_t origin) {
	if (ConfigureParams.Tablet.nTabletType == TABLET_MM961) {
		tablet.origin = SUMMA_ORIG_LOWER;
	} else {
		if (origin == SUMMA_LOWERLEFT) {
			tablet.origin = SUMMA_ORIG_LOWER;
		} else {
			tablet.origin = SUMMA_ORIG_UPPER;
		}
	}
	tablet_set_bounds();
}

static void tablet_set_resolution(int resolution) {
	tablet.xresolution = resolution;
	tablet.yresolution = resolution;
	tablet_set_bounds();
}

static void tablet_set_increment(void) {
	uint8_t val;
	val = tablet.command[1] & SUMMA_COORD_MASK;
	if (val >= 32) {
		tablet.increment = val - 32;
	}
}

static void tablet_set_axisupdate(void) {
	uint8_t val;
	val = tablet.command[1] & SUMMA_COORD_MASK;
	if (val >= 32) {
		tablet.axisupdate = val - 32;
	}
}

static void tablet_set_xy_scale(void) {
	uint32_t x, y, xlpi, ylpi;
	
	x = tablet.command[1] | ((uint32_t)tablet.command[2] << 8);
	y = tablet.command[3] | ((uint32_t)tablet.command[4] << 8);
	
	xlpi = (x * 10) / tablet.xsize;
	ylpi = (y * 10) / tablet.ysize;
	
	if (xlpi >= 1 && xlpi <= 508) {
		tablet.xresolution = x;
	}
	if (ylpi >= 1 && ylpi <= 508) {
		tablet.yresolution = y;
	}
	tablet_set_bounds();
}

static void tablet_send_configuration(void) {
	tablet.data[0] = SUMMA_PHASINGBIT;
	tablet.data[0] |= tablet.id ? SUMMA_IDENTIFIER : 0;
	tablet.data[0] |= SUMMA_X_SIGN | SUMMA_Y_SIGN;
	tablet.data[1] = tablet.xmax & SUMMA_COORD_MASK;
	tablet.data[2] = (tablet.xmax >> SUMMA_COORD_SHIFT) & SUMMA_COORD_MASK;
	tablet.data[3] = tablet.ymax & SUMMA_COORD_MASK;
	tablet.data[4] = (tablet.ymax >> SUMMA_COORD_SHIFT) & SUMMA_COORD_MASK;
	
	tablet_send_data(5);
}

static void tablet_send_state(void) {
	tablet.data[0] = tablet.flags;
	if (tablet.mode == SUMMA_DELTA) {
		tablet.data[1] = tablet.xdelta & SUMMA_COORD_MASK;
		tablet.data[2] = tablet.ydelta & SUMMA_COORD_MASK;
		tablet_send_data(3);
		tablet.xdelta = tablet.ydelta = 0;
	} else {
		tablet.data[1] = tablet.xpos & SUMMA_COORD_MASK;
		tablet.data[2] = (tablet.xpos >> SUMMA_COORD_SHIFT) & SUMMA_COORD_MASK;
		tablet.data[3] = tablet.ypos & SUMMA_COORD_MASK;
		tablet.data[4] = (tablet.ypos >> SUMMA_COORD_SHIFT) & SUMMA_COORD_MASK;
		tablet_send_data(5);
	}
}

static void tablet_send_checksum(void) {
	snprintf((char*)&tablet.data, sizeof(tablet.data), ".#%04X", 0xb044);
	tablet_send_data(6);
}

void tablet_receive(uint8_t val) {
	Log_Printf(LOG_TABLET_DATA_LEVEL, "[Tablet] Receiving %02x", val);
	
	if (tablet.command[0] == SUMMA_ECHO && val != SUMMA_RESET) {
		tablet.data[0] = val;
		tablet_send_data(1);
		return;
	}
	
	if (tablet.received == 0) {
		if (val == SUMMA_INCREMENT || val == SUMMA_AXISUPDATE) {
			tablet.cmdsize = 2;
		} else if (val == SUMMA_SETXYSCALE) {
			tablet.cmdsize = 5;
		} else {
			tablet.cmdsize = 1;
		}
	}
	
	tablet.command[tablet.received++] = val;
	
	if (tablet.cmdsize > tablet.received) {
		return;
	}

	switch (tablet.command[0]) {
		case SUMMA_SETBAUDRATE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Automatic baud rate");
			break;
		case SUMMA_HORIZONTAL:
			if (ConfigureParams.Tablet.nTabletType == TABLET_MM961) {
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: horizontal");
			} else { /* MM1201 */
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: upper left");
			}
			tablet_set_origin(SUMMA_HORIZONTAL);
			break;
		case SUMMA_VERTICAL:
			if (ConfigureParams.Tablet.nTabletType == TABLET_MM961) {
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: vertical");
			} else { /* MM1201 */
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: lower left");
			}
			tablet_set_origin(SUMMA_VERTICAL);
			break;
		case SUMMA_STREAM:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: stream");
			tablet.mode = SUMMA_STREAM;
			Statusbar_AddMessage("Starting tablet input (absolute mode)", 0);
			bTabletEnabled = true;
			break;
		case SUMMA_SWITCHSTREAM:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: switch stream");
			tablet.mode = SUMMA_SWITCHSTREAM;
			Statusbar_AddMessage("Stopping tablet input", 0);
			bTabletEnabled = false;
			break;
		case SUMMA_POINT:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: point");
			tablet.mode = SUMMA_POINT;
			break;
		case SUMMA_REMOTEREQ:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: remote request");
			tablet.mode = SUMMA_REMOTEREQ;
			break;
		case SUMMA_REQUEST:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Remote request");
			tablet_send_state();
			break;
		case SUMMA_DELTA:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: delta");
			tablet.mode = SUMMA_DELTA;
			Statusbar_AddMessage("Starting tablet input (relative mode)", 0);
			bTabletEnabled = true;
			break;
		case SUMMA_INCREMENT:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: incremental");
			tablet_set_increment();
			break;
		case SUMMA_AXISUPDATE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: axis update");
			tablet_set_axisupdate();
			break;
		case SUMMA_COO_RATE2:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 2 pairs per second");
			tablet.outputrate = 2;
			break;
		case SUMMA_COO_RATE20:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 20 pairs per second");
			tablet.outputrate = 20;
			break;
		case SUMMA_COO_RATE50:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 50 pairs per second");
			tablet.outputrate = 50;
			break;
		case SUMMA_COO_RATE100:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 100 pairs per second");
			tablet.outputrate = 100;
			break;
		case SUMMA_RES_10:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 10 lpmm");
			tablet_set_resolution(254);
			break;
		case SUMMA_RES_20:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 20 lpmm");
			tablet_set_resolution(508);
			break;
		case SUMMA_RES_40:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 40 lpmm");
			tablet_set_resolution(1016);
			break;
		case SUMMA_RES_100:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 100 lpi");
			tablet_set_resolution(100);
			break;
		case SUMMA_RES_200:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 200 lpi");
			tablet_set_resolution(200);
			break;
		case SUMMA_RES_400:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 400 lpi");
			tablet_set_resolution(400);
			break;
		case SUMMA_RES_500:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 500 lpi");
			tablet_set_resolution(500);
			break;
		case SUMMA_RES_1000:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 1000 lpi");
			tablet_set_resolution(1000);
			break;
		case SUMMA_ROUNDOFF_1:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Grid roundoff: 1 lpi");
			tablet_set_resolution(1);
			break;
		case SUMMA_ROUNDOFF_2:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Grid roundoff: 2 lpi");
			tablet_set_resolution(2);
			break;
		case SUMMA_ROUNDOFF_4:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Grid roundoff: 4 lpi");
			tablet_set_resolution(4);
			break;
		case SUMMA_SETXYSCALE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Set X,Y scale");
			tablet_set_xy_scale();
			break;
		case SUMMA_STOPTRANS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Stop transmission");
			tablet.enabled = 0;
			break;
		case SUMMA_STARTTRANS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Start transmission");
			tablet.enabled = 1;
			break;
		case SUMMA_IDENTIFY0:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Identifier: zero");
			tablet.id = 0;
			break;
		case SUMMA_IDENTIFY1:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Identifier: one");
			tablet.id = 1;
			break;
		case SUMMA_RESET:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Reset");
			tablet_reset();
			break;
		case SUMMA_STATUS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Send configuration");
			tablet_send_configuration();
			break;
		case SUMMA_SELFTEST:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Self-test");
			break;
		case SUMMA_TESTRESULTS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Send test results");
			break;
		case SUMMA_ECHO:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Echo");
			break;
		case SUMMA_CHECKCODE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Check code");
			tablet_send_checksum();
			break;
		case SUMMA_FACTORYTEST:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Factory test");
			break;

		default:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Unknown input: %c", (char)val);
			break;
	}
	
	tablet.cmdsize = tablet.received =  0;
}

void tablet_pen_move(int xrel, int yrel, int x, int y) {
	if (tablet.mode == SUMMA_DELTA) {
		if (tablet.origin == SUMMA_ORIG_LOWER) {
			yrel = -yrel;
		}
		if (xrel >= 0) {
			tablet.flags |= SUMMA_X_SIGN;
		} else {
			tablet.flags &= ~SUMMA_X_SIGN;
			xrel = -xrel;
		}
		if (yrel >= 0) {
			tablet.flags |= SUMMA_Y_SIGN;
		} else {
			tablet.flags &= ~SUMMA_Y_SIGN;
			yrel = -yrel;
		}
		tablet.xdelta = (xrel * tablet.xmax) / tablet.xscreen;
		tablet.ydelta = (yrel * tablet.xmax) / tablet.xscreen; /* yes, really */
		if (tablet.xdelta > SUMMA_COORD_MASK) {
			tablet.xdelta = SUMMA_COORD_MASK;
		}
		if (tablet.ydelta > SUMMA_COORD_MASK) {
			tablet.ydelta = SUMMA_COORD_MASK;
		}
	} else {
		x++;
		y++;
		if (x > tablet.xscreen) x = tablet.xscreen;
		else if (x < 0)         x = 0;
		if (y > tablet.yscreen) y = tablet.yscreen;
		else if (y < 0)         y = 0;
		tablet.xpos = (x * tablet.xmax) / tablet.xscreen;
		tablet.ypos = (y * tablet.ymax) / tablet.yscreen;
		if (tablet.origin == SUMMA_ORIG_LOWER) {
			tablet.ypos = tablet.ymax - tablet.ypos;
		}
		tablet.flags |= SUMMA_X_SIGN | SUMMA_Y_SIGN;
	}
	if (tablet.mode != SUMMA_REMOTEREQ) {
		tablet_send_state();
	}
}

void tablet_pen_tip(int pressed) {
	if (pressed) {
		tablet.flags |= SUMMA_BUTTON1;
	} else {
		tablet.flags &= ~SUMMA_BUTTON1;
	}
	if (tablet.mode != SUMMA_REMOTEREQ) {
		tablet_send_state();
	}
}

void Tablet_IO_Handler(void) {
	tablet_send(tablet.data[tablet.statsize - tablet.count]);
	if (--tablet.count > 0) {
		CycInt_AddTimeEvent(1000, 0, EVENT_TABLET_IO);
	}
}

void Tablet_Reset(void) {
	Log_Printf(LOG_WARN, "[Tablet] Reset");
	tablet_reset();
	/* Uninit tablet */
	bTabletEnabled = false;
}
