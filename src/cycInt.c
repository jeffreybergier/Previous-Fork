/*
  Previous - cycInt.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This code handles cycle accurate program interruption. We add any pending
  callback handler into a queue so that we do not need to test for every
  possible interrupt event.
  We support two time units: CPU cycles and microseconds. Microseconds are
  either bound to the host CPU's performance counter in realtime mode or to
  the emulated CPU cycles if non-realtime mode.
*/

const char CycInt_fileid[] = "Previous cycInt.c";

#include "main.h"
#include "timing.h"
#include "cycInt.h"
#include "m68000.h"
#include "screen.h"
#include "video.h"
#include "sysReg.h"
#include "esp.h"
#include "mo.h"
#include "ethernet.h"
#include "dma.h"
#include "floppy.h"
#include "snd.h"
#include "printer.h"
#include "kms.h"
#include "scc.h"
#include "configuration.h"
#include "dimension.hpp"


#define CHECK_INTERVAL 100

uint64_t nCyclesMainCounter; /* Main cycles counter, counts emulated CPU cycles since reset */

static int          nCheckCycles;
static uint64_t     nTimeNow;
static interrupt_id nCyclesFirst;
static interrupt_id nTimeFirst;

/* List of possible interrupt handlers to be store in 'PendingInterruptTable' */
static void (* const pIntHandlerFunctions[NUM_INTERRUPTS])(void) =
{
	NULL,
	Video_InterruptHandler,
	Hardclock_InterruptHandler,
	KMS_MouseHandler,
	ESP_InterruptHandler,
	ESP_IO_Handler,
	M2MDMA_IO_Handler,
	MO_InterruptHandler,
	MO_IO_Handler,
	ECC_IO_Handler,
	ENET_IO_Handler,
	FLP_IO_Handler,
	SND_Out_Handler,
	SND_In_Handler,
	Printer_IO_Handler,
	SCC_IO_Handler,
	Main_EventHandler,
	nd_display_vbl_handler,
	nd_video_vbl_handler
};

static INTERRUPTHANDLER InterruptHandlers[NUM_INTERRUPTS];


/*-----------------------------------------------------------------------*/
/**
 * Reset interrupts and handlers.
 */
void CycInt_Reset(void) {
	interrupt_id i;

	/* Reset counts */
	nCyclesMainCounter = 0;
	nCheckCycles       = 0;
	nTimeNow           = 0;
	
	/* Reset entry points */
	nCyclesFirst = INTERRUPT_NULL;
	nTimeFirst   = INTERRUPT_NULL;

	/* Reset interrupt table */
	for (i = INTERRUPT_NULL; i < NUM_INTERRUPTS; i++) {
		InterruptHandlers[i].func = pIntHandlerFunctions[i];
		InterruptHandlers[i].type = TYPE_NONE;
		InterruptHandlers[i].time = UINT64_MAX;
		InterruptHandlers[i].prev = INTERRUPT_NULL;
		InterruptHandlers[i].next = INTERRUPT_NULL;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Add cycles and process pending interrupts.
 */
void CycInt_AddCycles(int cycles) {
	nCyclesMainCounter += cycles;
	while (InterruptHandlers[nCyclesFirst].time <= nCyclesMainCounter) {
		interrupt_id i = nCyclesFirst;
		InterruptHandlers[i].type = TYPE_NONE;
		nCyclesFirst = InterruptHandlers[i].next;
		InterruptHandlers[nCyclesFirst].prev = INTERRUPT_NULL;
		InterruptHandlers[i].func();
	}
	if (nCheckCycles > 0) {
		nCheckCycles -= cycles;
	} else {
		nTimeNow = Timing_GetTime();
		while (nTimeFirst) {
			int64_t diff = InterruptHandlers[nTimeFirst].time - nTimeNow;
			if (diff > 0) {
				if (diff < CHECK_INTERVAL) {
					nCheckCycles = diff * ConfigureParams.System.nCpuFreq;
					return;
				}
				break;
			} else {
				interrupt_id i = nTimeFirst;
				InterruptHandlers[i].type = TYPE_NONE;
				nTimeFirst = InterruptHandlers[i].next;
				InterruptHandlers[nTimeFirst].prev = INTERRUPT_NULL;
				InterruptHandlers[i].func();
			}
		}
		nCheckCycles = CHECK_INTERVAL * ConfigureParams.System.nCpuFreq;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Add interrupt to the queue.
 */
static inline interrupt_id CycInt_AddInterrupt(interrupt_id first, interrupt_id i) {
	interrupt_id next, prev;

	next = first;
	prev = INTERRUPT_NULL;

	while (InterruptHandlers[next].time < InterruptHandlers[i].time) {
		prev = next;
		next = InterruptHandlers[next].next;
	}
	if (next == first) {
		first = i;
	}
	InterruptHandlers[i].prev = prev;
	InterruptHandlers[i].next = next;
	if (prev) {
		InterruptHandlers[prev].next = i;
	}
	if (next) {
		InterruptHandlers[next].prev = i;
	}
	return first;
}

/*-----------------------------------------------------------------------*/
/**
 * Set or update cycle interrupt and add it to the queue.
 */
void CycInt_AddCyclesInterrupt(int64_t CycleTime, interrupt_id i) {
	if (InterruptHandlers[i].type) {
		CycInt_RemovePendingInterrupt(i);
	}
	InterruptHandlers[i].type = TYPE_CYCLES;
	InterruptHandlers[i].time = nCyclesMainCounter + CycleTime;
	nCyclesFirst = CycInt_AddInterrupt(nCyclesFirst, i);
}
void CycInt_UpdateCyclesInterrupt(int64_t CycleTime, interrupt_id i) {
	if (InterruptHandlers[i].type) {
		CycInt_RemovePendingInterrupt(i);
	}
	InterruptHandlers[i].type = TYPE_CYCLES;
	InterruptHandlers[i].time += CycleTime;
	nCyclesFirst = CycInt_AddInterrupt(nCyclesFirst, i);
}

/*-----------------------------------------------------------------------*/
/**
 * Set or update microsecond time interrupt and add it to the queue.
 */
void CycInt_AddTimeInterrupt(int64_t RealTime, int64_t FastTime, interrupt_id i) {
	if (InterruptHandlers[i].type) {
		CycInt_RemovePendingInterrupt(i);
	}
	if (ConfigureParams.System.bRealtime) {
		RealTime = FastTime ? FastTime : RealTime;
		InterruptHandlers[i].type = TYPE_TIME;
		InterruptHandlers[i].time = Timing_GetTime() + RealTime;
		nTimeFirst = CycInt_AddInterrupt(nTimeFirst, i);
		if (RealTime < CHECK_INTERVAL && i == nTimeFirst) {
			nCheckCycles = RealTime * ConfigureParams.System.nCpuFreq;
		}
	} else {
		InterruptHandlers[i].type = TYPE_CYCLES;
		InterruptHandlers[i].time = nCyclesMainCounter + RealTime * ConfigureParams.System.nCpuFreq;
		nCyclesFirst = CycInt_AddInterrupt(nCyclesFirst, i);
	}
}
void CycInt_UpdateTimeInterrupt(int64_t RealTime, int64_t FastTime, interrupt_id i) {
	if (InterruptHandlers[i].type) {
		CycInt_RemovePendingInterrupt(i);
	}
	if (ConfigureParams.System.bRealtime) {
		nTimeNow = Timing_GetTime();
		if ((nTimeNow - InterruptHandlers[i].time) > CHECK_INTERVAL) {
			InterruptHandlers[i].time = nTimeNow;
		}
		InterruptHandlers[i].type = TYPE_TIME;
		InterruptHandlers[i].time += FastTime ? FastTime : RealTime;
		nTimeFirst = CycInt_AddInterrupt(nTimeFirst, i);
	} else {
		InterruptHandlers[i].type = TYPE_CYCLES;
		InterruptHandlers[i].time += RealTime * ConfigureParams.System.nCpuFreq;
		nCyclesFirst = CycInt_AddInterrupt(nCyclesFirst, i);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Convert microseconds to cycles and set or update cycle interrupt.
 */
void CycInt_AddCycleTimeInterrupt(int64_t RealTime, int64_t FastTime, interrupt_id i) {
	if (ConfigureParams.System.bRealtime && FastTime) {
		RealTime = FastTime;
	}
	CycInt_AddCyclesInterrupt(RealTime * ConfigureParams.System.nCpuFreq, i);
}
void CycInt_UpdateCycleTimeInterrupt(int64_t RealTime, int64_t FastTime, interrupt_id i) {
	if (ConfigureParams.System.bRealtime && FastTime) {
		RealTime = FastTime;
	}
	CycInt_UpdateCyclesInterrupt(RealTime * ConfigureParams.System.nCpuFreq, i);
}

/*-----------------------------------------------------------------------*/
/**
 * Remove interrupt from the corresponding queue.
 */
void CycInt_RemovePendingInterrupt(interrupt_id i) {
	if (InterruptHandlers[i].type == TYPE_CYCLES) {
		if (i == nCyclesFirst) {
			nCyclesFirst = InterruptHandlers[i].next;
		}
	} else if (InterruptHandlers[i].type == TYPE_TIME) {
		if (i == nTimeFirst) {
			nTimeFirst = InterruptHandlers[i].next;
		}
	} else {
		return;
	}
	if (InterruptHandlers[i].prev) {
		InterruptHandlers[InterruptHandlers[i].prev].next = InterruptHandlers[i].next;
	}
	if (InterruptHandlers[i].next) {
		InterruptHandlers[InterruptHandlers[i].next].prev = InterruptHandlers[i].prev;
	}
	InterruptHandlers[i].type = TYPE_NONE;
}

/*-----------------------------------------------------------------------*/
/**
 * Return true if the interrupt is queued.
 */
bool CycInt_InterruptActive(interrupt_id i) {
	return (InterruptHandlers[i].type != TYPE_NONE);
}
