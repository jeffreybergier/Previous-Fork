/*
  Previous - cycInt.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#pragma once

#ifndef PREV_CYCINT_H
#define PREV_CYCINT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Interrupt handlers in system */
typedef enum
{
  INTERRUPT_NULL,
  INTERRUPT_VIDEO_VBL,
  INTERRUPT_HARDCLOCK,
  INTERRUPT_MOUSE,
  INTERRUPT_ESP,
  INTERRUPT_ESP_IO,
  INTERRUPT_M2M_IO,
  INTERRUPT_MO,
  INTERRUPT_MO_IO,
  INTERRUPT_ECC_IO,
  INTERRUPT_ENET_IO,
  INTERRUPT_FLP_IO,
  INTERRUPT_SND_OUT,
  INTERRUPT_SND_IN,
  INTERRUPT_LP_IO,
  INTERRUPT_SCC_IO,
  INTERRUPT_EVENT_LOOP,
  INTERRUPT_ND_VBL,
  INTERRUPT_ND_VIDEO_VBL,
  NUM_INTERRUPTS
} interrupt_id;

typedef enum {
  TYPE_NONE,
  TYPE_CYCLES,
  TYPE_TIME
} interrupt_type;

typedef struct
{
	void (*pFunction)(void);
	interrupt_type type;
	uint64_t time;
	interrupt_id prev;
	interrupt_id next;
} INTERRUPTHANDLER;

extern INTERRUPTHANDLER InterruptHandlers[NUM_INTERRUPTS];

extern interrupt_id nCyclesFirst;
extern interrupt_id nTimeFirst;

extern uint64_t nCyclesMainCounter;

extern void CycInt_Reset(void);
extern void CycInt_AcknowledgeInterrupt(void);
extern void CycInt_AddRelativeInterruptCycles(int64_t CycleTime, interrupt_id Handler);
extern void CycInt_AddRelativeInterruptUs(int64_t us, int64_t usreal, interrupt_id Handler);
extern void CycInt_AddRelativeInterruptUsCycles(int64_t us, int64_t usreal, interrupt_id Handler);
extern void CycInt_RemovePendingInterrupt(interrupt_id Handler);
extern bool CycInt_InterruptActive(interrupt_id Handler);
extern void CycInt_AddCycles(int Cycles);

#ifdef __cplusplus
}
#endif

#endif /* ifndef PREV_CYCINT_H */
