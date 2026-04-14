/* Darwin/PowerPC specific low level interface, for the remote server for GDB.
   Copyright 2005 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "server.h"
#include "darwin-low.h"

#include <string.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/ppc/thread_status.h>

/*
  Apple GDB (gdb-437) register layout for PowerPC (106 registers, 1084 bytes):

    Regs  0-31:  r0-r31    (64-bit each, zero-extended from 32-bit HW regs)
    Regs 32-63:  f0-f31    (64-bit each)
    Regs 64-95:  v0-v31    (128-bit each, AltiVec)
    Reg  96:     pc/srr0   (64-bit, zero-extended)
    Reg  97:     ps/srr1   (64-bit, zero-extended)
    Reg  98:     cr        (32-bit)
    Reg  99:     lr        (64-bit, zero-extended)
    Reg 100:     ctr       (64-bit, zero-extended)
    Reg 101:     xer       (64-bit, zero-extended)
    Reg 102:     mq        (32-bit, 601 only, zero for G4)
    Reg 103:     fpscr     (32-bit)
    Reg 104:     vscr      (32-bit)
    Reg 105:     vrsave    (32-bit)
*/

#define PPC_NUM_REGS 106

/* Zero-extend a 32-bit value into a big-endian 64-bit buffer. */
static void
supply_reg64_from_32 (const char *name, unsigned int val)
{
  unsigned char buf[8];
  buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
  buf[4] = (val >> 24) & 0xff;
  buf[5] = (val >> 16) & 0xff;
  buf[6] = (val >> 8) & 0xff;
  buf[7] = val & 0xff;
  supply_register_by_name (name, buf);
}

/* Extract lower 32 bits from a big-endian 64-bit register. */
static unsigned int
collect_reg32_from_64 (const char *name)
{
  unsigned char buf[8];
  collect_register_by_name (name, buf);
  return ((unsigned int)buf[4] << 24)
       | ((unsigned int)buf[5] << 16)
       | ((unsigned int)buf[6] << 8)
       | (unsigned int)buf[7];
}

/* Fetch all registers from the Mach thread into the regcache.  */
static void
darwin_ppc_fetch_registers (int thread_port)
{
  thread_act_t thread = (thread_act_t) thread_port;
  kern_return_t kr;
  int i;

  /* --- GPRs and SPRs via PPC_THREAD_STATE --- */
  {
    ppc_thread_state_t state;
    mach_msg_type_number_t count = PPC_THREAD_STATE_COUNT;
    unsigned int *gprs;

    kr = thread_get_state (thread, PPC_THREAD_STATE,
                           (thread_state_t) &state, &count);
    if (kr != KERN_SUCCESS)
      {
        warning ("thread_get_state(PPC_THREAD_STATE) failed: %s",
                 mach_error_string (kr));
        return;
      }

    /* r0-r31: pointer to r0 in the struct, they are contiguous. */
    gprs = &state.r0;
    for (i = 0; i < 32; i++)
      {
        char name[8];
        sprintf (name, "r%d", i);
        supply_reg64_from_32 (name, gprs[i]);
      }

    /* 64-bit SPRs (zero-extended from 32-bit hardware) */
    supply_reg64_from_32 ("pc",  state.srr0);
    supply_reg64_from_32 ("ps",  state.srr1);
    supply_reg64_from_32 ("lr",  state.lr);
    supply_reg64_from_32 ("ctr", state.ctr);
    supply_reg64_from_32 ("xer", state.xer);

    /* 32-bit SPRs */
    supply_register_by_name ("cr",     &state.cr);
    supply_register_by_name ("mq",     &state.mq);
    supply_register_by_name ("vrsave", &state.vrsave);
  }

  /* --- FPRs via PPC_FLOAT_STATE --- */
  {
    ppc_float_state_t fstate;
    mach_msg_type_number_t count = PPC_FLOAT_STATE_COUNT;

    kr = thread_get_state (thread, PPC_FLOAT_STATE,
                           (thread_state_t) &fstate, &count);
    if (kr != KERN_SUCCESS)
      {
        warning ("thread_get_state(PPC_FLOAT_STATE) failed: %s",
                 mach_error_string (kr));
        return;
      }

    for (i = 0; i < 32; i++)
      {
        char name[8];
        sprintf (name, "f%d", i);
        supply_register_by_name (name, &fstate.fpregs[i]);
      }

    supply_register_by_name ("fpscr", &fstate.fpscr);
  }

  /* --- AltiVec via PPC_VECTOR_STATE --- */
  {
    ppc_vector_state_t vstate;
    mach_msg_type_number_t count = PPC_VECTOR_STATE_COUNT;

    kr = thread_get_state (thread, PPC_VECTOR_STATE,
                           (thread_state_t) &vstate, &count);
    if (kr == KERN_SUCCESS)
      {
        for (i = 0; i < 32; i++)
          {
            char name[8];
            sprintf (name, "v%d", i);
            supply_register_by_name (name, &vstate.save_vr[i][0]);
          }

        /* VSCR is in the last word of the 128-bit save_vscr field. */
        supply_register_by_name ("vscr", &vstate.save_vscr[3]);
      }
    else
      {
        /* AltiVec not available — supply zeros. */
        unsigned char zero16[16];
        unsigned int zero4 = 0;
        memset (zero16, 0, sizeof (zero16));
        for (i = 0; i < 32; i++)
          {
            char name[8];
            sprintf (name, "v%d", i);
            supply_register_by_name (name, zero16);
          }
        supply_register_by_name ("vscr", &zero4);
      }
  }
}

/* Store all registers from the regcache into the Mach thread.  */
static void
darwin_ppc_store_registers (int thread_port)
{
  thread_act_t thread = (thread_act_t) thread_port;
  kern_return_t kr;
  int i;

  /* --- GPRs and SPRs --- */
  {
    ppc_thread_state_t state;
    mach_msg_type_number_t count = PPC_THREAD_STATE_COUNT;
    unsigned int *gprs;

    kr = thread_get_state (thread, PPC_THREAD_STATE,
                           (thread_state_t) &state, &count);
    if (kr != KERN_SUCCESS)
      {
        warning ("thread_get_state failed in store: %s",
                 mach_error_string (kr));
        return;
      }

    gprs = &state.r0;
    for (i = 0; i < 32; i++)
      {
        char name[8];
        sprintf (name, "r%d", i);
        gprs[i] = collect_reg32_from_64 (name);
      }

    state.srr0   = collect_reg32_from_64 ("pc");
    state.srr1   = collect_reg32_from_64 ("ps");
    state.lr     = collect_reg32_from_64 ("lr");
    state.ctr    = collect_reg32_from_64 ("ctr");
    state.xer    = collect_reg32_from_64 ("xer");

    collect_register_by_name ("cr", &state.cr);
    collect_register_by_name ("mq", &state.mq);
    collect_register_by_name ("vrsave", &state.vrsave);

    kr = thread_set_state (thread, PPC_THREAD_STATE,
                           (thread_state_t) &state,
                           PPC_THREAD_STATE_COUNT);
    if (kr != KERN_SUCCESS)
      warning ("thread_set_state(PPC_THREAD_STATE) failed: %s",
               mach_error_string (kr));
  }

  /* --- FPRs --- */
  {
    ppc_float_state_t fstate;
    mach_msg_type_number_t count = PPC_FLOAT_STATE_COUNT;

    kr = thread_get_state (thread, PPC_FLOAT_STATE,
                           (thread_state_t) &fstate, &count);
    if (kr != KERN_SUCCESS)
      {
        warning ("thread_get_state(PPC_FLOAT_STATE) failed in store: %s",
                 mach_error_string (kr));
        return;
      }

    for (i = 0; i < 32; i++)
      {
        char name[8];
        sprintf (name, "f%d", i);
        collect_register_by_name (name, &fstate.fpregs[i]);
      }

    collect_register_by_name ("fpscr", &fstate.fpscr);

    kr = thread_set_state (thread, PPC_FLOAT_STATE,
                           (thread_state_t) &fstate,
                           PPC_FLOAT_STATE_COUNT);
    if (kr != KERN_SUCCESS)
      warning ("thread_set_state(PPC_FLOAT_STATE) failed: %s",
               mach_error_string (kr));
  }

  /* --- AltiVec --- */
  {
    ppc_vector_state_t vstate;
    mach_msg_type_number_t count = PPC_VECTOR_STATE_COUNT;

    kr = thread_get_state (thread, PPC_VECTOR_STATE,
                           (thread_state_t) &vstate, &count);
    if (kr != KERN_SUCCESS)
      return;  /* AltiVec not available */

    for (i = 0; i < 32; i++)
      {
        char name[8];
        sprintf (name, "v%d", i);
        collect_register_by_name (name, &vstate.save_vr[i][0]);
      }

    collect_register_by_name ("vscr", &vstate.save_vscr[3]);

    kr = thread_set_state (thread, PPC_VECTOR_STATE,
                           (thread_state_t) &vstate,
                           PPC_VECTOR_STATE_COUNT);
    if (kr != KERN_SUCCESS)
      warning ("thread_set_state(PPC_VECTOR_STATE) failed: %s",
               mach_error_string (kr));
  }
}

static CORE_ADDR
darwin_ppc_get_pc (void)
{
  /* PC is a 64-bit register in the cache; extract lower 32 bits. */
  return (CORE_ADDR) collect_reg32_from_64 ("pc");
}

static void
darwin_ppc_set_pc (CORE_ADDR pc)
{
  supply_reg64_from_32 ("pc", (unsigned int) pc);
}

/* PPC trap instruction for software breakpoints. */
static const unsigned int ppc_breakpoint = 0x7d821008;
#define PPC_BREAKPOINT_LEN 4

static int
darwin_ppc_breakpoint_at (CORE_ADDR where)
{
  unsigned int insn;
  (*the_target->read_memory) (where, (char *) &insn, 4);
  if (insn == ppc_breakpoint)
    return 1;
  return 0;
}

struct darwin_target_ops the_low_target = {
  PPC_NUM_REGS,
  darwin_ppc_fetch_registers,
  darwin_ppc_store_registers,
  darwin_ppc_get_pc,
  darwin_ppc_set_pc,
  (const char *) &ppc_breakpoint,
  PPC_BREAKPOINT_LEN,
  0,  /* decr_pc_after_break */
  darwin_ppc_breakpoint_at,
};
