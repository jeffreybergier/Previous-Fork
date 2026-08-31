/*
 * events.c
 *
 * Event stuff - currently just simplified to the bare minimum
 * in Hatari, but we might want to extend this one day...
 */

#include "main.h"

#include "sysconfig.h"
#include "sysdeps.h"

#include "options_cpu.h"
#include "events.h"
#include "cycInt.h"

#ifdef JIT
int countdown;
static bool jit_cycle_armed;
#ifdef WINUAE_FOR_PREVIOUS
extern void previous_jit_run_other_MPUs(int cycles);
#endif
/* Service NeXT device events at roughly interpreter instruction cadence.
 * Large slices let the JIT run thousands of guest cycles past SCSI/DMA
 * deadlines before raising the pending interrupt. */
#define PREVIOUS_JIT_CYCLE_SLICE CYCLE_UNIT

void jit_cycle_reset(void)
{
	countdown = 0;
	jit_cycle_armed = false;
}
#endif

#ifndef WINUAE_FOR_HATARI
void do_cycles_normal(int cycles_to_add)
{
	while ((nextevent - currcycle) <= cycles_to_add) {

		cycles_to_add -= (int)(nextevent - currcycle);
		currcycle = nextevent;

		for (int i = 0; i < ev_max; i++) {
			if (eventtab[i].active && eventtab[i].evtime == currcycle) {
				if (eventtab[i].handler == NULL) {
					gui_message(_T("eventtab[%d].handler is null!\n"), i);
					eventtab[i].active = 0;
				} else {
					(*eventtab[i].handler)();
				}
			}
		}
		events_schedule();

	}
	currcycle += cycles_to_add;
}
#else
/* Simplified version for Hatari, we don't use eventtab[] */
void do_cycles_normal(int cycles_to_add)
{
//fprintf ( stderr , "  do_cycles_normal add=%d curr=%d -> new=%d\n" , cycles_to_add , currcycle , currcycle+cycles_to_add );
	currcycle += cycles_to_add;
}
#endif


void do_cycles_slow (int cycles_to_add)
{
//fprintf ( stderr , "  do_cycles_slow add=%d curr=%d -> new=%d\n" , cycles_to_add , currcycle , currcycle+cycles_to_add );
	#ifdef JIT
	if (currprefs.cachesize) {
		int elapsed;
		if (cycles_to_add > 0) {
			elapsed = cycles_to_add;
		} else if (countdown < 0) {
			elapsed = -countdown;
			if (jit_cycle_armed)
				elapsed += PREVIOUS_JIT_CYCLE_SLICE;
			countdown = PREVIOUS_JIT_CYCLE_SLICE;
			jit_cycle_armed = true;
		} else {
			return;
		}
		currcycle += elapsed;
	#ifdef WINUAE_FOR_PREVIOUS
		previous_jit_run_other_MPUs(elapsed * 2 / CYCLE_UNIT);
	#else
		CycInt_AddCycles(elapsed * 2 / CYCLE_UNIT);
	#endif
		return;
	}
	#endif
	currcycle += cycles_to_add;
}
