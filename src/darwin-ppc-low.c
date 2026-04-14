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

#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/ppc/thread_status.h>

#define PPC_NUM_REGS 71

/*
  Register layout in reg-ppc.dat (matches regcache order):
    r0-r31    (32 x 32-bit)  regs 0..31
    f0-f31    (32 x 64-bit)  regs 32..63
    pc        (32-bit)       reg 64
    ps/msr    (32-bit)       reg 65
    cr        (32-bit)       reg 66
    lr        (32-bit)       reg 67
    ctr       (32-bit)       reg 68
    xer       (32-bit)       reg 69
    fpscr     (32-bit)       reg 70
*/

/* Fetch all registers from the Mach thread into the regcache.  */
static void
darwin_ppc_fetch_registers (int thread_port)
{
  thread_act_t thread = (thread_act_t) thread_port;
  kern_return_t kr;

  /* Fetch GPRs via PPC_THREAD_STATE.  */
  {
    ppc_thread_state_t state;
    mach_msg_type_number_t count = PPC_THREAD_STATE_COUNT;

    kr = thread_get_state (thread, PPC_THREAD_STATE,
                           (thread_state_t) &state, &count);
    if (kr != KERN_SUCCESS)
      {
        warning ("thread_get_state(PPC_THREAD_STATE) failed: %s",
                 mach_error_string (kr));
        return;
      }

    /* GPRs r0-r31.  */
    supply_register_by_name ("r0",  &state.r0);
    supply_register_by_name ("r1",  &state.r1);
    supply_register_by_name ("r2",  &state.r2);
    supply_register_by_name ("r3",  &state.r3);
    supply_register_by_name ("r4",  &state.r4);
    supply_register_by_name ("r5",  &state.r5);
    supply_register_by_name ("r6",  &state.r6);
    supply_register_by_name ("r7",  &state.r7);
    supply_register_by_name ("r8",  &state.r8);
    supply_register_by_name ("r9",  &state.r9);
    supply_register_by_name ("r10", &state.r10);
    supply_register_by_name ("r11", &state.r11);
    supply_register_by_name ("r12", &state.r12);
    supply_register_by_name ("r13", &state.r13);
    supply_register_by_name ("r14", &state.r14);
    supply_register_by_name ("r15", &state.r15);
    supply_register_by_name ("r16", &state.r16);
    supply_register_by_name ("r17", &state.r17);
    supply_register_by_name ("r18", &state.r18);
    supply_register_by_name ("r19", &state.r19);
    supply_register_by_name ("r20", &state.r20);
    supply_register_by_name ("r21", &state.r21);
    supply_register_by_name ("r22", &state.r22);
    supply_register_by_name ("r23", &state.r23);
    supply_register_by_name ("r24", &state.r24);
    supply_register_by_name ("r25", &state.r25);
    supply_register_by_name ("r26", &state.r26);
    supply_register_by_name ("r27", &state.r27);
    supply_register_by_name ("r28", &state.r28);
    supply_register_by_name ("r29", &state.r29);
    supply_register_by_name ("r30", &state.r30);
    supply_register_by_name ("r31", &state.r31);

    /* Special registers.  */
    supply_register_by_name ("pc",  &state.srr0);
    supply_register_by_name ("ps",  &state.srr1);
    supply_register_by_name ("cr",  &state.cr);
    supply_register_by_name ("lr",  &state.lr);
    supply_register_by_name ("ctr", &state.ctr);
    supply_register_by_name ("xer", &state.xer);
  }

  /* Fetch FPRs via PPC_FLOAT_STATE.  */
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

    supply_register_by_name ("f0",  &fstate.fpregs[0]);
    supply_register_by_name ("f1",  &fstate.fpregs[1]);
    supply_register_by_name ("f2",  &fstate.fpregs[2]);
    supply_register_by_name ("f3",  &fstate.fpregs[3]);
    supply_register_by_name ("f4",  &fstate.fpregs[4]);
    supply_register_by_name ("f5",  &fstate.fpregs[5]);
    supply_register_by_name ("f6",  &fstate.fpregs[6]);
    supply_register_by_name ("f7",  &fstate.fpregs[7]);
    supply_register_by_name ("f8",  &fstate.fpregs[8]);
    supply_register_by_name ("f9",  &fstate.fpregs[9]);
    supply_register_by_name ("f10", &fstate.fpregs[10]);
    supply_register_by_name ("f11", &fstate.fpregs[11]);
    supply_register_by_name ("f12", &fstate.fpregs[12]);
    supply_register_by_name ("f13", &fstate.fpregs[13]);
    supply_register_by_name ("f14", &fstate.fpregs[14]);
    supply_register_by_name ("f15", &fstate.fpregs[15]);
    supply_register_by_name ("f16", &fstate.fpregs[16]);
    supply_register_by_name ("f17", &fstate.fpregs[17]);
    supply_register_by_name ("f18", &fstate.fpregs[18]);
    supply_register_by_name ("f19", &fstate.fpregs[19]);
    supply_register_by_name ("f20", &fstate.fpregs[20]);
    supply_register_by_name ("f21", &fstate.fpregs[21]);
    supply_register_by_name ("f22", &fstate.fpregs[22]);
    supply_register_by_name ("f23", &fstate.fpregs[23]);
    supply_register_by_name ("f24", &fstate.fpregs[24]);
    supply_register_by_name ("f25", &fstate.fpregs[25]);
    supply_register_by_name ("f26", &fstate.fpregs[26]);
    supply_register_by_name ("f27", &fstate.fpregs[27]);
    supply_register_by_name ("f28", &fstate.fpregs[28]);
    supply_register_by_name ("f29", &fstate.fpregs[29]);
    supply_register_by_name ("f30", &fstate.fpregs[30]);
    supply_register_by_name ("f31", &fstate.fpregs[31]);

    supply_register_by_name ("fpscr", &fstate.fpscr);
  }
}

/* Store all registers from the regcache into the Mach thread.  */
static void
darwin_ppc_store_registers (int thread_port)
{
  thread_act_t thread = (thread_act_t) thread_port;
  kern_return_t kr;

  /* Store GPRs.  */
  {
    ppc_thread_state_t state;
    mach_msg_type_number_t count = PPC_THREAD_STATE_COUNT;

    /* Read current state first so we don't clobber fields we don't track.  */
    kr = thread_get_state (thread, PPC_THREAD_STATE,
                           (thread_state_t) &state, &count);
    if (kr != KERN_SUCCESS)
      {
        warning ("thread_get_state failed in store: %s",
                 mach_error_string (kr));
        return;
      }

    collect_register_by_name ("r0",  &state.r0);
    collect_register_by_name ("r1",  &state.r1);
    collect_register_by_name ("r2",  &state.r2);
    collect_register_by_name ("r3",  &state.r3);
    collect_register_by_name ("r4",  &state.r4);
    collect_register_by_name ("r5",  &state.r5);
    collect_register_by_name ("r6",  &state.r6);
    collect_register_by_name ("r7",  &state.r7);
    collect_register_by_name ("r8",  &state.r8);
    collect_register_by_name ("r9",  &state.r9);
    collect_register_by_name ("r10", &state.r10);
    collect_register_by_name ("r11", &state.r11);
    collect_register_by_name ("r12", &state.r12);
    collect_register_by_name ("r13", &state.r13);
    collect_register_by_name ("r14", &state.r14);
    collect_register_by_name ("r15", &state.r15);
    collect_register_by_name ("r16", &state.r16);
    collect_register_by_name ("r17", &state.r17);
    collect_register_by_name ("r18", &state.r18);
    collect_register_by_name ("r19", &state.r19);
    collect_register_by_name ("r20", &state.r20);
    collect_register_by_name ("r21", &state.r21);
    collect_register_by_name ("r22", &state.r22);
    collect_register_by_name ("r23", &state.r23);
    collect_register_by_name ("r24", &state.r24);
    collect_register_by_name ("r25", &state.r25);
    collect_register_by_name ("r26", &state.r26);
    collect_register_by_name ("r27", &state.r27);
    collect_register_by_name ("r28", &state.r28);
    collect_register_by_name ("r29", &state.r29);
    collect_register_by_name ("r30", &state.r30);
    collect_register_by_name ("r31", &state.r31);

    collect_register_by_name ("pc",  &state.srr0);
    collect_register_by_name ("ps",  &state.srr1);
    collect_register_by_name ("cr",  &state.cr);
    collect_register_by_name ("lr",  &state.lr);
    collect_register_by_name ("ctr", &state.ctr);
    collect_register_by_name ("xer", &state.xer);

    kr = thread_set_state (thread, PPC_THREAD_STATE,
                           (thread_state_t) &state,
                           PPC_THREAD_STATE_COUNT);
    if (kr != KERN_SUCCESS)
      warning ("thread_set_state(PPC_THREAD_STATE) failed: %s",
               mach_error_string (kr));
  }

  /* Store FPRs.  */
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

    collect_register_by_name ("f0",  &fstate.fpregs[0]);
    collect_register_by_name ("f1",  &fstate.fpregs[1]);
    collect_register_by_name ("f2",  &fstate.fpregs[2]);
    collect_register_by_name ("f3",  &fstate.fpregs[3]);
    collect_register_by_name ("f4",  &fstate.fpregs[4]);
    collect_register_by_name ("f5",  &fstate.fpregs[5]);
    collect_register_by_name ("f6",  &fstate.fpregs[6]);
    collect_register_by_name ("f7",  &fstate.fpregs[7]);
    collect_register_by_name ("f8",  &fstate.fpregs[8]);
    collect_register_by_name ("f9",  &fstate.fpregs[9]);
    collect_register_by_name ("f10", &fstate.fpregs[10]);
    collect_register_by_name ("f11", &fstate.fpregs[11]);
    collect_register_by_name ("f12", &fstate.fpregs[12]);
    collect_register_by_name ("f13", &fstate.fpregs[13]);
    collect_register_by_name ("f14", &fstate.fpregs[14]);
    collect_register_by_name ("f15", &fstate.fpregs[15]);
    collect_register_by_name ("f16", &fstate.fpregs[16]);
    collect_register_by_name ("f17", &fstate.fpregs[17]);
    collect_register_by_name ("f18", &fstate.fpregs[18]);
    collect_register_by_name ("f19", &fstate.fpregs[19]);
    collect_register_by_name ("f20", &fstate.fpregs[20]);
    collect_register_by_name ("f21", &fstate.fpregs[21]);
    collect_register_by_name ("f22", &fstate.fpregs[22]);
    collect_register_by_name ("f23", &fstate.fpregs[23]);
    collect_register_by_name ("f24", &fstate.fpregs[24]);
    collect_register_by_name ("f25", &fstate.fpregs[25]);
    collect_register_by_name ("f26", &fstate.fpregs[26]);
    collect_register_by_name ("f27", &fstate.fpregs[27]);
    collect_register_by_name ("f28", &fstate.fpregs[28]);
    collect_register_by_name ("f29", &fstate.fpregs[29]);
    collect_register_by_name ("f30", &fstate.fpregs[30]);
    collect_register_by_name ("f31", &fstate.fpregs[31]);

    collect_register_by_name ("fpscr", &fstate.fpscr);

    kr = thread_set_state (thread, PPC_FLOAT_STATE,
                           (thread_state_t) &fstate,
                           PPC_FLOAT_STATE_COUNT);
    if (kr != KERN_SUCCESS)
      warning ("thread_set_state(PPC_FLOAT_STATE) failed: %s",
               mach_error_string (kr));
  }
}

static CORE_ADDR
darwin_ppc_get_pc (void)
{
  unsigned int pc;
  collect_register_by_name ("pc", &pc);
  return (CORE_ADDR) pc;
}

static void
darwin_ppc_set_pc (CORE_ADDR pc)
{
  unsigned int newpc = pc;
  supply_register_by_name ("pc", &newpc);
}

/* "trap" instruction — same as used by GDB for PPC software breakpoints.
   This is "twge r2, r2" (0x7d821008), which is what GDB uses.
   Correct in both big-endian and little-endian (PPC is big-endian here).  */
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
