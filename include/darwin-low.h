/* Internal interfaces for the Darwin/macOS specific target code for gdbserver.
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

#ifndef DARWIN_LOW_H
#define DARWIN_LOW_H

#include <mach/mach.h>
#include <mach/mach_vm.h>

struct darwin_target_ops
{
  int num_regs;

  /* Fetch registers from the Mach thread state into the regcache.  */
  void (*fetch_registers) (int tid);

  /* Store registers from the regcache into the Mach thread state.  */
  void (*store_registers) (int tid);

  CORE_ADDR (*get_pc) (void);
  void (*set_pc) (CORE_ADDR newpc);

  const char *breakpoint;
  int breakpoint_len;
  int decr_pc_after_break;
  int (*breakpoint_at) (CORE_ADDR pc);
};

extern struct darwin_target_ops the_low_target;

/* Per-process information.  */
struct darwin_process_info
{
  struct inferior_list_entry head;
  pid_t pid;
  task_t task;
  thread_act_t thread;
  int stopped;
  int status;
};

extern struct inferior_list all_processes;

#define get_process(inf) ((struct darwin_process_info *)(inf))
#define get_thread_process(thr) \
  (get_process (inferior_target_data (thr)))

#endif /* DARWIN_LOW_H */
