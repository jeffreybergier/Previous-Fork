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
#include "main.h"
#include "dimension.hpp"


interrupt_id nCyclesFirst;
interrupt_id nTimeFirst;

uint64_t nCyclesMainCounter; /* Main cycles counter, counts emulated CPU cycles since reset */

static int nCheckCycles;

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

INTERRUPTHANDLER InterruptHandlers[NUM_INTERRUPTS];


/*-----------------------------------------------------------------------*/
/**
 * Reset interrupts and handlers.
 */
void CycInt_Reset(void) {
	interrupt_id i;

	/* Reset counts */
	nCyclesMainCounter = 0;
	nCheckCycles       = 0;
	
	/* Reset entry points */
	nCyclesFirst = INTERRUPT_NULL;
	nTimeFirst   = INTERRUPT_NULL;

	/* Reset interrupt table */
	for (i = INTERRUPT_NULL; i < NUM_INTERRUPTS; i++) {
		InterruptHandlers[i].pFunction = pIntHandlerFunctions[i];
		InterruptHandlers[i].cycles    = UINT64_MAX;
		InterruptHandlers[i].time      = UINT64_MAX;
		InterruptHandlers[i].prev      = INTERRUPT_NULL;
		InterruptHandlers[i].next      = INTERRUPT_NULL;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Add cycles, check for pending microsecond interrupt and place it at 
 * the beginning of the cycle interrupt queue.
 */
void CycInt_AddCycles(int cycles) {
	nCyclesMainCounter += cycles;
	if (nCheckCycles > 0) {
		nCheckCycles -= cycles;
	} else {
		uint64_t now = Timing_GetTime();
		if (ConfigureParams.System.bRealtime) {
			int64_t diff = InterruptHandlers[nTimeFirst].time - now;
			if (diff > 0) {
				if (diff < 100) {
					nCheckCycles = diff * ConfigureParams.System.nCpuFreq;
					return;
				}
			} else {
				interrupt_id i = nTimeFirst;
				nTimeFirst = InterruptHandlers[i].next;
				if (nTimeFirst) {
					InterruptHandlers[nTimeFirst].prev = InterruptHandlers[i].prev;
				}
				InterruptHandlers[i].cycles = nCyclesMainCounter;
				InterruptHandlers[i].time   = UINT64_MAX;
				InterruptHandlers[i].prev   = INTERRUPT_NULL;
				InterruptHandlers[i].next   = nCyclesFirst;
				nCyclesFirst = i;
				diff = InterruptHandlers[nTimeFirst].time - now;
				if (diff < 100) {
					nCheckCycles = diff * ConfigureParams.System.nCpuFreq;
					return;
				}
			}
		}
		nCheckCycles = 100 * ConfigureParams.System.nCpuFreq;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Remove the active interrupt from the cycle interrupt queue.
 */
void CycInt_AcknowledgeInterrupt(void) {
	interrupt_id i = nCyclesFirst;
	nCyclesFirst = InterruptHandlers[i].next;
	InterruptHandlers[i].cycles = UINT64_MAX;
	InterruptHandlers[i].time   = UINT64_MAX;
	InterruptHandlers[i].prev   = INTERRUPT_NULL;
	InterruptHandlers[i].next   = INTERRUPT_NULL;
}

/*-----------------------------------------------------------------------*/
/**
 * Add cycle interrupt to the queue.
 */
void CycInt_AddRelativeInterruptCycles(int64_t CycleTime, interrupt_id i) {
	interrupt_id next, prev;
	uint64_t cycles;
		
	CycInt_RemovePendingInterrupt(i);
	
	cycles = nCyclesMainCounter + CycleTime;
	next   = nCyclesFirst;
	prev   = INTERRUPT_NULL;
	
	while (InterruptHandlers[next].cycles < cycles) {
		prev = next;
		next = InterruptHandlers[next].next;
	}	
	if (next == nCyclesFirst) {
		nCyclesFirst = i;
	}
	InterruptHandlers[i].cycles = cycles;
	InterruptHandlers[i].prev   = prev;
	InterruptHandlers[i].next   = next;
	if (prev) {
		InterruptHandlers[prev].next = i;
	}
	if (next) {
		InterruptHandlers[next].prev = i;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Add microsecond interrupt to the queue.
 */
void CycInt_AddRelativeInterruptUs(int64_t us, int64_t usreal, interrupt_id i) {
	assert(us >= 0);
	
	if (ConfigureParams.System.bRealtime) {
		interrupt_id next, prev;
		uint64_t time;
		
		CycInt_RemovePendingInterrupt(i);
		
		time = Timing_GetTime() + (usreal ? usreal : us);
		next = nTimeFirst;
		prev = INTERRUPT_NULL;
		
		while (InterruptHandlers[next].time < time) {
			prev = next;
			next = InterruptHandlers[next].next;
		}	
		if (next == nTimeFirst) {
			nTimeFirst = i;
		}
		InterruptHandlers[i].time = time;
		InterruptHandlers[i].prev = prev;
		InterruptHandlers[i].next = next;
		if (prev) {
			InterruptHandlers[prev].next = i;
		}
		if (next) {
			InterruptHandlers[next].prev = i;
		}
	} else {
		CycInt_AddRelativeInterruptCycles(us * ConfigureParams.System.nCpuFreq, i);
	}
}

/*-----------------------------------------------------------------------*/
/**
 * Convert microseconds to cycles and add cycle interrupt to the queue.
 */
void CycInt_AddRelativeInterruptUsCycles(int64_t us, int64_t usreal, interrupt_id i) {

	if (ConfigureParams.System.bRealtime && usreal) {
		us = usreal;
	}

	CycInt_AddRelativeInterruptCycles(us * ConfigureParams.System.nCpuFreq, i);
}

/*-----------------------------------------------------------------------*/
/**
 * Remove interrupt from the corresponding queue.
 */
void CycInt_RemovePendingInterrupt(interrupt_id i) {
	if (InterruptHandlers[i].cycles != UINT64_MAX) {
		if (i == nCyclesFirst) {
			nCyclesFirst = InterruptHandlers[i].next;
		}
	} else if (InterruptHandlers[i].time != UINT64_MAX) {
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
	InterruptHandlers[i].cycles = UINT64_MAX;
	InterruptHandlers[i].time   = UINT64_MAX;
	InterruptHandlers[i].prev   = INTERRUPT_NULL;
	InterruptHandlers[i].next   = INTERRUPT_NULL;
}

/*-----------------------------------------------------------------------*/
/**
 * Return true if the interrupt queued.
 */
bool CycInt_InterruptActive(interrupt_id i)
{
	return (InterruptHandlers[i].cycles != UINT64_MAX) || (InterruptHandlers[i].time != UINT64_MAX);
}
