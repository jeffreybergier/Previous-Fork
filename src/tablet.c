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
#include "scc.h"
#include "tablet.h"

#define LOG_TABLET_LEVEL      LOG_WARN
#define LOG_TABLET_DATA_LEVEL LOG_DEBUG


#define SUMMA_SETBAUDRATE 0x20 /* SP */

/* for model 961 */
#define SUMMA_HORIZONTAL  0x62 /* b */
#define SUMMA_VERTICAL    0x63 /* c */
/* for model 1201 */
#define SUMMA_UPPERLEF    0x62 /* b */
#define SUMMA_LOWERLEFT   0x63 /* c */

#define SUMMA_COO_RATE2   0x54 /* T */
#define SUMMA_COO_RATE20  0x53 /* S */
#define SUMMA_COO_RATE50  0x52 /* R */
#define SUMMA_COO_RATE100 0x51 /* Q */

#define SUMMA_STREAM      0x40 /* @ */
#define SUMMA_SWITCHSTRAM 0x41 /* A */
#define SUMMA_POINT       0x42 /* B */
#define SUMMA_REMOTEREQ   0x44 /* D */
#define SUMMA_REQUEST     0x50 /* P */
#define SUMMA_DELTA       0x45 /* E */
#define SUMMA_AXISUPDATE  0x47 /* G followed by one byte update value plus 32 */
#define SUMMA_INCREMENT   0x49 /* I followed by one byte increment value plus 32 */

#define SUMMA_RES_10      0x66 /* f */
#define SUMMA_RES_20      0x69 /* i */
#define SUMMA_RES_40      0x71 /* q */
#define SUMMA_RES_100     0x64 /* d */
#define SUMMA_RES_200     0x65 /* e */
#define SUMMA_RES_400     0x67 /* g */
#define SUMMA_RES_500     0x68 /* h */
#define SUMMA_RES_1000    0x6A /* j */

#define SUMMA_ROUNDOFF_1  0x6C /* l */
#define SUMMA_ROUNDOFF_2  0x6E /* n */
#define SUMMA_ROUNDOFF_4  0x70 /* p */

#define SUMMA_SETXYSCALE  0x72 /* r followed by two byte X and two byte Y scale values */

#define SUMMA_STARTTRANS  0x11 /* DC1 (XON) */
#define SUMMA_STOPTRANS   0x13 /* DC3 (XOFF) */

#define SUMMA_IDENTIFY0   0x30 /* 0 */
#define SUMMA_IDENTIFY1   0x31 /* 1 */

#define SUMMA_RESET       0x00 /* NUL */

#define SUMMA_STATUS      0x61 /* a */

#define SUMMA_SELFTEST    0x74 /* t */
#define SUMMA_TESTRESULTS 0x77 /* w */
#define SUMMA_ECHO        0x6B /* k */
#define SUMMA_CHECKCODE   0x78 /* x */
#define SUMMA_FACTORYTEST 0x7A /* z */


/* Resolution */
#define SUMMA_961_RES 0
#define SUMMA_1201_RES 5850


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
 * 3: X coordinate low  7 bit  |  0
 * 4: X coordiante high 7 bit  |  0
 */
#define SUMMA_COORD_MASK  0x7F
#define SUMMA_COORD_SHIFT 7


struct summa_tablet {
	uint8_t output_data[5];
	
} tablet;

static void tablet_send(uint8_t val) {
	Log_Printf(LOG_TABLET_DATA_LEVEL, "[Tablet] Sending %02x", val);
	scc_receive(1, val);
}

void tablet_receive(uint8_t val) {
	Log_Printf(LOG_TABLET_DATA_LEVEL, "[Tablet] Receiving %02x", val);

	switch (val) {
		case SUMMA_SETBAUDRATE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Automatic baud rate");
			break;
		case SUMMA_HORIZONTAL:
			if (1) { /* model 1201 */
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: upper left");
			} else { /* model 961 */
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: horizontal");
			}
			break;
		case SUMMA_VERTICAL:
			if (1) { /* model 1201 */
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: lower left");
			} else { /* model 961 */
				Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Tablet origin: vertical");
			}
			break;
		case SUMMA_STREAM:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: stream");
			break;
		case SUMMA_SWITCHSTRAM:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: switch stream");
			break;
		case SUMMA_POINT:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: point");
			break;
		case SUMMA_REMOTEREQ:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: remote request");
			break;
		case SUMMA_REQUEST:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Remote request");
			break;
		case SUMMA_DELTA:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: delta");
			break;
		case SUMMA_INCREMENT:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: incremental");
			break;
		case SUMMA_AXISUPDATE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Data collection mode: axis update");
			break;
		case SUMMA_COO_RATE2:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 2 pairs per second");
			break;
		case SUMMA_COO_RATE20:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 20 pairs per second");
			break;
		case SUMMA_COO_RATE50:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 50 pairs per second");
			break;
		case SUMMA_COO_RATE100:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Coordinate output rate: 100 pairs per second");
			break;
		case SUMMA_RES_10:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 10 lpmm");
			break;
		case SUMMA_RES_20:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 20 lpmm");
			break;
		case SUMMA_RES_40:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 40 lpmm");
			break;
		case SUMMA_RES_100:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 100 lpi");
			break;
		case SUMMA_RES_200:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 200 lpi");
			break;
		case SUMMA_RES_400:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 400 lpi");
			break;
		case SUMMA_RES_500:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 500 lpi");
			break;
		case SUMMA_RES_1000:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Resolution: 1000 lpi");
			break;
		case SUMMA_ROUNDOFF_1:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Grid roundoff: 1 lpi");
			break;
		case SUMMA_ROUNDOFF_2:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Grid roundoff: 2 lpi");
			break;
		case SUMMA_ROUNDOFF_4:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Grid roundoff: 4 lpi");
			break;
		case SUMMA_SETXYSCALE:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Set X,Y scale");
			break;
		case SUMMA_STOPTRANS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Stop transmission");
			break;
		case SUMMA_STARTTRANS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Start transmission");
			break;
		case SUMMA_IDENTIFY0:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Identifier: zero");
			break;
		case SUMMA_IDENTIFY1:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Identifier: one");
			break;
		case SUMMA_RESET:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Reset");
			break;
		case SUMMA_STATUS:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Send configuration");
			tablet_send(58); /* FIXME: Just a simple hack */
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
			break;
		case SUMMA_FACTORYTEST:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Factory test");
			break;

		default:
			Log_Printf(LOG_TABLET_LEVEL, "[Tablet] Unknown input: %c", (char)val);
			break;
	}
}


void tablet_reset(void) {
	Log_Printf(LOG_WARN, "[Tablet] Reset");
}
