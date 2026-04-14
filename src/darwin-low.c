/* Low level interface to ptrace and Mach, for the remote server for GDB.
   Darwin/macOS version.
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

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <mach/task.h>
#include <mach/mach_traps.h>
#include <mach/ppc/thread_status.h>

struct inferior_list all_processes;

static task_t current_task = MACH_PORT_NULL;
static thread_act_t current_thread = MACH_PORT_NULL;

/* These are referenced by remote-utils.c.  */
int using_threads = 0;
int debug_threads = 0;

/* Get the Mach task port for a given pid.  */
static task_t
darwin_task_for_pid (pid_t pid)
{
  task_t task;
  kern_return_t kr;

  kr = task_for_pid (mach_task_self (), pid, &task);
  if (kr != KERN_SUCCESS)
    {
      error ("task_for_pid(%d) failed: %s (kr=%d)\n",
             pid, mach_error_string (kr), kr);
      return MACH_PORT_NULL;
    }
  return task;
}

/* Get the first thread from a task.  */
static thread_act_t
darwin_get_first_thread (task_t task)
{
  thread_act_array_t thread_list;
  mach_msg_type_number_t thread_count;
  kern_return_t kr;

  kr = task_threads (task, &thread_list, &thread_count);
  if (kr != KERN_SUCCESS || thread_count == 0)
    {
      error ("task_threads failed: %s\n", mach_error_string (kr));
      return MACH_PORT_NULL;
    }

  /* Use the first thread.  */
  return thread_list[0];
}

/* Create a new inferior process.  */
static int
darwin_create_inferior (char *program, char **allargs)
{
  pid_t pid;
  struct darwin_process_info *proc;

  pid = fork ();
  if (pid < 0)
    perror_with_name ("fork");

  if (pid == 0)
    {
      /* Child process.  */
      ptrace (PT_TRACE_ME, 0, 0, 0);
      /* Redirect stdin/stdout if needed.  */
      signal (SIGTTOU, SIG_DFL);
      signal (SIGTTIN, SIG_DFL);
      execv (program, allargs);
      fprintf (stderr, "Cannot exec %s: %s.\n", program, strerror (errno));
      _exit (0177);
    }

  /* Parent - wait for child to stop at exec.  */
  {
    int status;
    waitpid (pid, &status, 0);
  }

  current_task = darwin_task_for_pid (pid);
  if (current_task == MACH_PORT_NULL)
    {
      kill (pid, SIGKILL);
      error ("Cannot get task port for child (pid %d).\n"
             "Make sure you are root or the binary is signed.\n", pid);
    }

  current_thread = darwin_get_first_thread (current_task);

  proc = calloc (1, sizeof (*proc));
  proc->head.id = pid;
  proc->pid = pid;
  proc->task = current_task;
  proc->thread = current_thread;
  proc->stopped = 1;
  add_inferior_to_list (&all_processes, &proc->head);

  add_thread (pid, proc);

  return pid;
}

/* Attach to a running process.  */
static int
darwin_attach (int pid)
{
  struct darwin_process_info *proc;
  int ret;

  ret = ptrace (PT_ATTACH, pid, 0, 0);
  if (ret != 0)
    {
      fprintf (stderr, "Cannot attach to pid %d: %s (%d)\n",
               pid, strerror (errno), errno);
      return -1;
    }

  /* Wait for the stop.  */
  {
    int status;
    waitpid (pid, &status, 0);
  }

  current_task = darwin_task_for_pid (pid);
  if (current_task == MACH_PORT_NULL)
    {
      ptrace (PT_DETACH, pid, 0, 0);
      return -1;
    }

  current_thread = darwin_get_first_thread (current_task);

  proc = calloc (1, sizeof (*proc));
  proc->head.id = pid;
  proc->pid = pid;
  proc->task = current_task;
  proc->thread = current_thread;
  proc->stopped = 1;
  add_inferior_to_list (&all_processes, &proc->head);

  add_thread (pid, proc);

  return 0;
}

/* Kill the inferior.  */
static void
darwin_kill (void)
{
  struct darwin_process_info *proc;

  if (current_inferior == NULL)
    return;

  proc = get_thread_process (current_inferior);
  ptrace (PT_KILL, proc->pid, 0, 0);
  waitpid (proc->pid, NULL, 0);

  current_task = MACH_PORT_NULL;
  current_thread = MACH_PORT_NULL;

  clear_inferiors ();
}

/* Detach from the inferior.  */
static void
darwin_detach (void)
{
  struct darwin_process_info *proc;

  if (current_inferior == NULL)
    return;

  proc = get_thread_process (current_inferior);
  ptrace (PT_DETACH, proc->pid, (caddr_t) 1, 0);

  current_task = MACH_PORT_NULL;
  current_thread = MACH_PORT_NULL;

  clear_inferiors ();
}

/* Return 1 if the given thread is still alive.  */
static int
darwin_thread_alive (int pid)
{
  /* For now, just check if we can signal it.  */
  return (kill (pid, 0) == 0);
}

/* Resume the inferior.  */
static void
darwin_resume (struct thread_resume *resume_info)
{
  struct darwin_process_info *proc;
  int step = 0;
  int sig = 0;
  int pid;

  /* Find the process for the current thread.  */
  proc = get_thread_process (current_inferior);
  pid = proc->pid;

  /* Determine step/signal from resume_info.
     The last entry (thread == -1) is the default.  */
  {
    int i;
    for (i = 0; resume_info[i].thread != -1; i++)
      {
        if (resume_info[i].thread == pid || resume_info[i].thread == -1)
          {
            step = resume_info[i].step;
            sig = resume_info[i].sig;
            break;
          }
      }
    /* If we didn't find a specific entry, use the default.  */
    if (resume_info[i].thread == -1)
      {
        step = resume_info[i].step;
        sig = resume_info[i].sig;
      }
  }

  regcache_invalidate ();

  if (step)
    ptrace (PT_STEP, pid, (caddr_t) 1, sig);
  else
    ptrace (PT_CONTINUE, pid, (caddr_t) 1, sig);

  proc->stopped = 0;
}

/* Wait for the inferior to stop.  */
static unsigned char
darwin_wait (char *status)
{
  int w;
  int ret;
  struct darwin_process_info *proc;

  proc = get_thread_process (current_inferior);

again:
  ret = waitpid (proc->pid, &w, 0);
  if (ret == -1)
    {
      if (errno == EINTR)
        goto again;
      perror_with_name ("waitpid");
    }

  /* Refresh Mach thread port after the wait.  */
  current_thread = darwin_get_first_thread (current_task);
  proc->thread = current_thread;
  proc->stopped = 1;

  if (WIFEXITED (w))
    {
      *status = 'W';
      return WEXITSTATUS (w);
    }
  else if (WIFSIGNALED (w))
    {
      *status = 'X';
      return WTERMSIG (w);
    }
  else if (WIFSTOPPED (w))
    {
      *status = 'T';
      return WSTOPSIG (w);
    }

  /* Unknown status.  */
  *status = '?';
  return 0;
}

/* Fetch registers using Mach thread_get_state.
   Delegates to the arch-specific low target.  */
static void
darwin_fetch_registers (int regno)
{
  struct darwin_process_info *proc;
  proc = get_thread_process (current_inferior);
  the_low_target.fetch_registers ((int) proc->thread);
}

/* Store registers using Mach thread_set_state.
   Delegates to the arch-specific low target.  */
static void
darwin_store_registers (int regno)
{
  struct darwin_process_info *proc;
  proc = get_thread_process (current_inferior);
  the_low_target.store_registers ((int) proc->thread);
}

/* Read memory from the inferior using Mach vm_read.  */
static void
darwin_read_memory (CORE_ADDR memaddr, char *myaddr, int len)
{
  kern_return_t kr;
  vm_offset_t data;
  mach_msg_type_number_t data_count;

  if (current_task == MACH_PORT_NULL)
    {
      memset (myaddr, 0, len);
      return;
    }

  kr = vm_read (current_task, (vm_address_t) memaddr, len,
                &data, &data_count);
  if (kr != KERN_SUCCESS)
    {
      /* Fall back: try reading word-by-word via ptrace.  */
      int i;
      struct darwin_process_info *proc;
      proc = get_thread_process (current_inferior);

      for (i = 0; i < len; i += sizeof (int))
        {
          int word;
          int count = (len - i < (int) sizeof (int)) ? len - i : sizeof (int);
          errno = 0;
          word = ptrace (PT_READ_D, proc->pid,
                         (caddr_t)(unsigned long)(memaddr + i), 0);
          if (errno != 0)
            {
              memset (myaddr + i, 0, count);
              continue;
            }
          memcpy (myaddr + i, &word, count);
        }
      return;
    }

  memcpy (myaddr, (void *) data, len);
  vm_deallocate (mach_task_self (), data, data_count);
}

/* Write memory to the inferior using Mach vm_write.  */
static int
darwin_write_memory (CORE_ADDR memaddr, const char *myaddr, int len)
{
  kern_return_t kr;

  if (current_task == MACH_PORT_NULL)
    return EIO;

  kr = vm_write (current_task, (vm_address_t) memaddr,
                 (vm_offset_t) myaddr, len);
  if (kr != KERN_SUCCESS)
    {
      /* The memory might not be writable. Try to change protection.  */
      kr = vm_protect (current_task, (vm_address_t) memaddr, len,
                       FALSE, VM_PROT_READ | VM_PROT_WRITE);
      if (kr == KERN_SUCCESS)
        {
          kr = vm_write (current_task, (vm_address_t) memaddr,
                         (vm_offset_t) myaddr, len);
        }
    }

  if (kr != KERN_SUCCESS)
    {
      /* Last resort: try ptrace PT_WRITE_D word by word.  */
      int i;
      struct darwin_process_info *proc;
      proc = get_thread_process (current_inferior);

      for (i = 0; i < len; i += sizeof (int))
        {
          int word = 0;
          int count = (len - i < (int) sizeof (int)) ? len - i : sizeof (int);

          if (count < (int) sizeof (int))
            {
              /* Partial word: read existing, modify, write back.  */
              errno = 0;
              word = ptrace (PT_READ_D, proc->pid,
                             (caddr_t)(unsigned long)(memaddr + i), 0);
              if (errno != 0)
                return errno;
            }
          memcpy (&word, myaddr + i, count);

          errno = 0;
          ptrace (PT_WRITE_D, proc->pid,
                  (caddr_t)(unsigned long)(memaddr + i), word);
          if (errno != 0)
            return errno;
        }
      return 0;
    }

  return 0;
}

/* Send a signal to the inferior.  */
static void
darwin_send_signal (int signum)
{
  struct darwin_process_info *proc;
  proc = get_thread_process (current_inferior);
  kill (proc->pid, signum);
}

static struct target_ops darwin_target_ops = {
  darwin_create_inferior,
  darwin_attach,
  darwin_kill,
  darwin_detach,
  darwin_thread_alive,
  darwin_resume,
  darwin_wait,
  darwin_fetch_registers,
  darwin_store_registers,
  darwin_read_memory,
  darwin_write_memory,
  NULL,  /* look_up_symbols */
  darwin_send_signal,
  NULL,  /* read_auxv - not available on Darwin */
};

void
initialize_low (void)
{
  set_target_ops (&darwin_target_ops);
  set_breakpoint_data (the_low_target.breakpoint,
                       the_low_target.breakpoint_len);
  init_registers ();
}
