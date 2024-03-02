/*
  Previous - ioMem.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef PREV_IOMEM_H
#define PREV_IOMEM_H

#include "config.h"
#include "memory.h"

extern uint32_t IoAccessMask;
extern uint32_t IoAccessCurrentAddress;

/**
 * Read 32-bit word from IO memory space without interception.
 * NOTE - value will be converted to PC endian.
 */
static inline uint32_t IoMem_ReadLong(uint32_t Address)
{
	Address &= IoAccessMask;
	Address &= ~3;
	return do_get_mem_long(&NEXTIo[Address]);
}


/**
 * Read 16-bit word from IO memory space without interception.
 * NOTE - value will be converted to PC endian.
 */
static inline uint16_t IoMem_ReadWord(uint32_t Address)
{
	Address &= IoAccessMask;
	Address &= ~1;
	return do_get_mem_word(&NEXTIo[Address]);
}


/**
 * Read 8-bit byte from IO memory space without interception.
 */
static inline uint8_t IoMem_ReadByte(uint32_t Address)
{
	Address &= IoAccessMask;
 	return NEXTIo[Address];
}


/**
 * Write 32-bit word into IO memory space without interception.
 * NOTE - value will be convert to 68000 endian
 */
static inline void IoMem_WriteLong(uint32_t Address, uint32_t Var)
{
	Address &= IoAccessMask;
	Address &= ~3;
	do_put_mem_long(&NEXTIo[Address], Var);
}


/**
 * Write 16-bit word into IO memory space without interception.
 * NOTE - value will be convert to 68000 endian.
 */
static inline void IoMem_WriteWord(uint32_t Address, uint16_t Var)
{
	Address &= IoAccessMask;
	Address &= ~1;
	do_put_mem_word(&NEXTIo[Address], Var);
}


/**
 * Write 8-bit byte into IO memory space without interception.
 */
static inline void IoMem_WriteByte(uint32_t Address, uint8_t Var)
{
	Address &= IoAccessMask;
	NEXTIo[Address] = Var;
}


extern void IoMem_Init(void);
extern void IoMem_UnInit(void);

extern uint8_t IoMem_ReadBytePort(void);

extern uint32_t IoMem_bget(uint32_t addr);
extern uint32_t IoMem_wget(uint32_t addr);
extern uint32_t IoMem_lget(uint32_t addr);

extern void IoMem_bput(uint32_t addr, uint32_t val);
extern void IoMem_wput(uint32_t addr, uint32_t val);
extern void IoMem_lput(uint32_t addr, uint32_t val);

extern void IoMem_BusErrorEvenReadAccess(void);
extern void IoMem_BusErrorOddReadAccess(void);
extern void IoMem_BusErrorEvenWriteAccess(void);
extern void IoMem_BusErrorOddWriteAccess(void);
extern void IoMem_VoidRead(void);
extern void IoMem_VoidRead_00(void);
extern void IoMem_VoidWrite(void);
extern void IoMem_WriteWithoutInterception(void);
extern void IoMem_ReadWithoutInterception(void);
extern void IoMem_WriteWithoutInterceptionButTrace(void);
extern void IoMem_ReadWithoutInterceptionButTrace(void);

#endif /* PREV_IOMEM_H */
