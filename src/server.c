/* Main code for remote server for GDB.
   Copyright 1989, 1993, 1994, 1995, 1997, 1998, 1999, 2000, 2002, 2003, 2004
   Free Software Foundation, Inc.

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
#include "cond-bp.h"

#include <fcntl.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int cont_thread;
int general_thread;
int step_thread;
int thread_from_wait;
int old_thread_from_wait;
int extended_protocol;
int server_waiting;

/* Set to 1 when LOG=1 environment variable is present. */
static int log_enabled = 0;

#define LOG_CMD(fmt, ...)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (log_enabled)                                                                           \
            fprintf(stderr, "[LOG] " fmt "\n", ##__VA_ARGS__);                                     \
    } while (0)

jmp_buf toplevel;

static unsigned char mywait_cond(char *statusp, int connected);
static int step_past_breakpoint(int is_step, char *statusp);

/* The PID of the originally created or attached inferior.  Used to
   send signals to the process when GDB sends us an asynchronous interrupt
   (user hitting Control-C in the client), and to wait for the child to exit
   when no longer debugging it.  */

int signal_pid;

/* Parse a Mach-O binary and return the entry point address from
   LC_UNIXTHREAD (PPC: srr0 field).  Returns 0 on failure.  */
static CORE_ADDR macho_get_entry_point(const char *path)
{
    int fd;
    struct mach_header mh;
    unsigned char *cmds, *p;
    unsigned int i;
    CORE_ADDR entry = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return 0;
    }

    if (read(fd, &mh, sizeof(mh)) != sizeof(mh))
    {
        close(fd);
        return 0;
    }

    if (mh.magic != MH_MAGIC)
    {
        close(fd);
        return 0;
    }

    cmds = malloc(mh.sizeofcmds);
    if (cmds == NULL || read(fd, cmds, mh.sizeofcmds) != (int)mh.sizeofcmds)
    {
        free(cmds);
        close(fd);
        return 0;
    }
    close(fd);

    p = cmds;
    for (i = 0; i < mh.ncmds; i++)
    {
        struct load_command *lc = (struct load_command *)p;

        if (lc->cmd == LC_UNIXTHREAD)
        {
            /* Layout: load_command header (8 bytes),
         flavor (4), count (4), then thread state.
         For PPC_THREAD_STATE, srr0 is at offset 0 in the state
         (first field after flavor+count = offset 16 from lc start).  */
            unsigned int *data = (unsigned int *)(p + 8);
            unsigned int flavor = data[0];
            (void)flavor;
            /* srr0 is the first register in ppc_thread_state_t.
         data[0] = flavor, data[1] = count, data[2] = srr0 */
            entry = (CORE_ADDR)data[2];
            break;
        }

        p += lc->cmdsize;
    }

    free(cmds);
    return entry;
}

static unsigned char start_inferior(char *argv[], char *statusptr)
{
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);

    signal_pid = create_inferior(argv[0], argv);

    fprintf(stderr, "Process %s created; pid = %d\n", argv[0], signal_pid);

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    tcsetpgrp(fileno(stderr), signal_pid);

    /* Wait till we are at 1st instruction in program, return signal number.  */
    return mywait(statusptr, 0);
}

static int attach_inferior(int pid, char *statusptr, unsigned char *sigptr)
{
    /* myattach should return -1 if attaching is unsupported,
     0 if it succeeded, and call error() otherwise.  */

    if (myattach(pid) != 0)
    {
        return -1;
    }

    fprintf(stderr, "Attached; pid = %d\n", pid);

    /* FIXME - It may be that we should get the SIGNAL_PID from the
     attach function, so that it can be the main thread instead of
     whichever we were told to attach to.  */
    signal_pid = pid;

    *sigptr = mywait(statusptr, 0);

    return 0;
}

extern int remote_debug;

/* Handle all of the extended 'q' packets.  */
void handle_query(char *own_buf)
{
    static struct inferior_list_entry *thread_ptr;

    /* [NEW] qC — return current thread ID */
    if (strcmp("qC", own_buf) == 0)
    {
        LOG_CMD("[NEW] qC — query current thread ID");
        if (all_threads.head != NULL)
        {
            sprintf(own_buf, "QC%x", all_threads.head->id);
        }
        else
        {
            own_buf[0] = 0;
        }
        return;
    }

    /* [NEW] qSupported — exchange feature support with client */
    if (strncmp("qSupported", own_buf, 10) == 0)
    {
        LOG_CMD("[NEW] qSupported — feature negotiation");
        sprintf(own_buf, "PacketSize=%x;QStartNoAckMode+", PBUFSIZ - 1);
        return;
    }

    /* [NEW] qAttached — tell client we created (not attached to) the process */
    if (strcmp("qAttached", own_buf) == 0)
    {
        LOG_CMD("[NEW] qAttached — report process creation mode");
        strcpy(own_buf, "0");
        return;
    }

    /* qSymbol — client offers to look up symbols for us */
    if (strcmp("qSymbol::", own_buf) == 0)
    {
        LOG_CMD("qSymbol — symbol lookup offer");
        if (the_target->look_up_symbols != NULL)
        {
            (*the_target->look_up_symbols)();
        }

        strcpy(own_buf, "OK");
        return;
    }

    /* qfThreadInfo — first thread in thread list query */
    if (strcmp("qfThreadInfo", own_buf) == 0)
    {
        LOG_CMD("qfThreadInfo — first thread info query");
        thread_ptr = all_threads.head;
        sprintf(own_buf, "m%x", thread_ptr->id);
        thread_ptr = thread_ptr->next;
        return;
    }

    /* qsThreadInfo — subsequent threads in thread list query */
    if (strcmp("qsThreadInfo", own_buf) == 0)
    {
        LOG_CMD("qsThreadInfo — subsequent thread info query");
        if (thread_ptr != NULL)
        {
            sprintf(own_buf, "m%x", thread_ptr->id);
            thread_ptr = thread_ptr->next;
            return;
        }
        else
        {
            sprintf(own_buf, "l");
            return;
        }
    }

    /* qPart:auxv:read — read auxiliary vector data */
    if (the_target->read_auxv != NULL && strncmp("qPart:auxv:read::", own_buf, 17) == 0)
    {
        char data[(PBUFSIZ - 1) / 2];
        CORE_ADDR ofs;
        unsigned int len;
        int n;
        LOG_CMD("qPart:auxv:read — read auxiliary vector");
        decode_m_packet(&own_buf[17], &ofs, &len); /* "OFS,LEN" */
        if (len > sizeof data)
        {
            len = sizeof data;
        }
        n = (*the_target->read_auxv)(ofs, data, len);
        if (n == 0)
        {
            write_ok(own_buf);
        }
        else if (n < 0)
        {
            write_enn(own_buf);
        }
        else
        {
            convert_int_to_ascii(data, own_buf, n);
        }
        return;
    }

    /* [NEW] qRcmd — remote monitor command (conditional breakpoints, etc.) */
    if (strncmp("qRcmd,", own_buf, 6) == 0)
    {
        LOG_CMD("[NEW] qRcmd — remote monitor command");
        handle_rcmd(own_buf + 6, own_buf);
        return;
    }

    /* Otherwise we didn't know what packet it was.  Say we didn't
     understand it.  */
    own_buf[0] = 0;
}

/* Handle all of the extended 'Q' set packets.  */
void handle_set(char *own_buf)
{
    /* QStartNoAckMode — disable +/- ACK handshake for all subsequent packets.
     The server advertises this in qSupported; IDA Pro sends it immediately
     after feature negotiation.  We must reply OK *before* enabling no-ack
     so that the OK reply itself still gets acknowledged by the client.  */
    if (strcmp("QStartNoAckMode", own_buf) == 0)
    {
        LOG_CMD("[NEW] QStartNoAckMode — disabling ACK handshake");
        write_ok(own_buf);
        /* Enable no-ack BEFORE the main loop calls putpkt, so that putpkt
       sends "$OK#..." without waiting for a '+' — IDA has already
       switched to no-ack mode on its side.  */
        no_ack_mode = 1;
        return;
    }

    /* QPassSignals:XX;YY;... — signals the client handles itself; just ack.  */
    if (strncmp("QPassSignals:", own_buf, 13) == 0)
    {
        LOG_CMD("QPassSignals — acknowledging signal pass list (no-op)");
        write_ok(own_buf);
        return;
    }

    /* QNonStop — non-stop mode; not supported, reply empty.  */
    if (strncmp("QNonStop:", own_buf, 9) == 0)
    {
        LOG_CMD("QNonStop — not supported, ignoring");
        own_buf[0] = '\0';
        return;
    }

    /* Unknown Q packet — reply empty to indicate no support.  */
    LOG_CMD("unknown Q command: '%.40s'", own_buf);
    own_buf[0] = '\0';
}

/* Parse vCont packets.  */
void handle_v_cont(char *own_buf, char *status, unsigned char *signal)
{
    char *p, *q;
    int n = 0, i = 0;
    struct thread_resume *resume_info, default_action;

    /* Count the number of semicolons in the packet.  There should be one
     for every action.  */
    p = &own_buf[5];
    while (p)
    {
        n++;
        p++;
        p = strchr(p, ';');
    }
    /* Allocate room for one extra action, for the default remain-stopped
     behavior; if no default action is in the list, we'll need the extra
     slot.  */
    resume_info = malloc((n + 1) * sizeof(resume_info[0]));

    default_action.thread = -1;
    default_action.leave_stopped = 1;
    default_action.step = 0;
    default_action.sig = 0;

    p = &own_buf[5];
    i = 0;
    while (*p)
    {
        p++;

        resume_info[i].leave_stopped = 0;

        if (p[0] == 's' || p[0] == 'S')
        {
            resume_info[i].step = 1;
        }
        else if (p[0] == 'c' || p[0] == 'C')
        {
            resume_info[i].step = 0;
        }
        else
        {
            goto err;
        }

        if (p[0] == 'S' || p[0] == 'C')
        {
            int sig;
            sig = strtol(p + 1, &q, 16);
            if (p == q)
            {
                goto err;
            }
            p = q;

            if (!target_signal_to_host_p(sig))
            {
                goto err;
            }
            resume_info[i].sig = target_signal_to_host(sig);
        }
        else
        {
            resume_info[i].sig = 0;
            p = p + 1;
        }

        if (p[0] == 0)
        {
            resume_info[i].thread = -1;
            default_action = resume_info[i];

            /* Note: we don't increment i here, we'll overwrite this entry
         the next time through.  */
        }
        else if (p[0] == ':')
        {
            resume_info[i].thread = strtol(p + 1, &q, 16);
            if (p == q)
            {
                goto err;
            }
            p = q;
            if (p[0] != ';' && p[0] != 0)
            {
                goto err;
            }

            i++;
        }
    }

    resume_info[i] = default_action;

    /* Still used in occasional places in the backend.  */
    if (n == 1 && resume_info[0].thread != -1)
    {
        cont_thread = resume_info[0].thread;
    }
    else
    {
        cont_thread = -1;
    }
    set_desired_inferior(0);

    /* If the first action is a step and PC is at a breakpoint,
     step past it first — the step-past IS the user's step. */
    if (n >= 1 && resume_info[0].step)
    {
        char stepstat = 'T';
        if (step_past_breakpoint(1, &stepstat))
        {
            free(resume_info);
            *signal = TARGET_SIGNAL_TRAP;
            *status = stepstat;
            prepare_resume_reply(own_buf, *status, *signal);
            return;
        }
    }
    else
    {
        char stepstat = 'T';
        step_past_breakpoint(0, &stepstat);
    }

    (*the_target->resume)(resume_info);

    free(resume_info);

    *signal = mywait_cond(status, 1);
    prepare_resume_reply(own_buf, *status, *signal);
    return;

err:
    /* No other way to report an error... */
    strcpy(own_buf, "");
    free(resume_info);
    return;
}

/* Handle all of the extended 'v' packets.  */
void handle_v_requests(char *own_buf, char *status, unsigned char *signal)
{
    /* vCont;action — resume with per-thread actions (step/continue) */
    if (strncmp(own_buf, "vCont;", 6) == 0)
    {
        LOG_CMD("vCont — resume with actions: %s", own_buf + 5);
        handle_v_cont(own_buf, status, signal);
        return;
    }

    /* vCont? — query supported vCont actions */
    if (strncmp(own_buf, "vCont?", 6) == 0)
    {
        LOG_CMD("vCont? — query supported resume actions");
        strcpy(own_buf, "vCont;c;C;s;S");
        return;
    }

    /* Otherwise we didn't know what packet it was.  Say we didn't
     understand it.  */
    own_buf[0] = 0;
    return;
}

void myresume(int step, int sig)
{
    struct thread_resume resume_info[2];
    int n = 0;

    if (step || sig || cont_thread > 0)
    {
        resume_info[0].thread = ((struct inferior_list_entry *)current_inferior)->id;
        resume_info[0].step = step;
        resume_info[0].sig = sig;
        resume_info[0].leave_stopped = 0;
        n++;
    }
    resume_info[n].thread = -1;
    resume_info[n].step = 0;
    resume_info[n].sig = 0;
    resume_info[n].leave_stopped = (cont_thread > 0);

    (*the_target->resume)(resume_info);
}

static int attached;

/* If the current PC is sitting on an inserted software breakpoint,
   temporarily remove it, single-step the real instruction, and
   reinsert it.  Returns 1 if a breakpoint was stepped past, 0 if not.
   When stepping (is_step=1), the single-step IS the user's step,
   so the caller should skip its own myresume.  */
static int step_past_breakpoint(int is_step, char *statusp)
{
    unsigned int pc32 = 0;
    CORE_ADDR pc;
    unsigned char sig;

    set_desired_inferior(1);
    collect_register_by_name("pc", &pc32);
    pc = (CORE_ADDR)pc32;

    if (!breakpoint_inserted_here(pc))
    {
        return 0;
    }

    /* Remove the trap, step the real instruction, put it back. */
    uninsert_breakpoint(pc);
    myresume(1, 0);
    sig = mywait(statusp, 1);
    reinsert_breakpoint(pc);

    /* Re-fetch registers so the regcache reflects post-step state.
     prepare_resume_reply reads expedited regs (r1, pc) from it. */
    set_desired_inferior(1);

    if (is_step)
    {
        return 1; /* The step-past IS the user's single step. */
    }

    /* For continue: we stepped past, now caller will continue. */
    return 1;
}

static void gdbserver_usage(void)
{
    error("Usage:\tgdbserver COMM PROG [ARGS ...]\n"
          "\tgdbserver COMM --attach PID\n"
          "\n"
          "COMM may either be a tty device (for serial debugging), or \n"
          "HOST:PORT to listen for a TCP connection.\n");
}

/* Wait for the inferior, checking conditional breakpoints.
   When a cond_bp fires but its condition is NOT met, silently
   step over the breakpoint and continue.  Returns only when:
   - the process exits/dies, or
   - a stop occurs that is NOT a cond_bp with unmet condition.  */
static unsigned char mywait_cond(char *statusp, int connected)
{
    unsigned char sig;

    for (;;)
    {
        sig = mywait(statusp, connected);

        /* Only check conditional BPs on SIGTRAP stops. */
        if (*statusp == 'T' && sig == TARGET_SIGNAL_TRAP)
        {
            unsigned int pc32 = 0;
            CORE_ADDR pc;
            int cond;

            /* Fetch registers so we can read PC and condition regs. */
            set_desired_inferior(1);
            collect_register_by_name("pc", &pc32);
            pc = (CORE_ADDR)pc32;

            cond = check_cond_bp(pc);
            if (cond == 2)
            {
                /* Condition not met — step over breakpoint and continue. */
                uninsert_breakpoint(pc);

                /* Single-step past the original instruction. */
                myresume(1, 0);
                sig = mywait(statusp, connected);

                reinsert_breakpoint(pc);

                /* Invalidate register cache before continuing. */
                regcache_invalidate();

                /* Now continue running. */
                myresume(0, 0);
                continue;
            }
            /* cond == 0 (not a cond_bp) or cond == 1 (condition met) → stop. */
        }

        return sig;
    }
}

int main(int argc, char *argv[])
{
    char ch, status, *own_buf, mem_buf[2000];
    int i = 0;
    unsigned char signal;
    unsigned int len;
    CORE_ADDR mem_addr;
    int bad_attach;
    int pid;
    char *arg_end;

    if (setjmp(toplevel))
    {
        fprintf(stderr, "Exiting\n");
        exit(1);
    }

    /* Check LOG=1 environment variable for command logging. */
    {
        const char *log_env = getenv("LOG");
        if (log_env && log_env[0] == '1')
        {
            log_enabled = 1;
        }
    }

    bad_attach = 0;
    pid = 0;
    attached = 0;
    if (argc >= 3 && strcmp(argv[2], "--attach") == 0)
    {
        if (argc == 4 && argv[3] != '\0' && (pid = strtoul(argv[3], &arg_end, 10)) != 0 &&
            *arg_end == '\0')
        {
            ;
        }
        else
        {
            bad_attach = 1;
        }
    }

    if (argc < 3 || bad_attach)
    {
        gdbserver_usage();
    }

    initialize_low();

    own_buf = malloc(PBUFSIZ);

    if (pid == 0)
    {
        /* Wait till we are at first instruction in program.  */
        signal = start_inferior(&argv[2], &status);

        /* We are now stopped at the first instruction of the target process.
       This is typically inside dyld.  Try to run up to the real entry
       point so the debugger lands in the user's code.  */
        {
            CORE_ADDR ep = macho_get_entry_point(argv[2]);
            if (ep != 0)
            {
                fprintf(stderr, "Entry point at 0x%lx — running to it\n", (unsigned long)ep);
                set_breakpoint_at(ep, NULL);
                myresume(0, 0);
                signal = mywait(&status, 0);
                delete_breakpoint_at(ep);
                fprintf(stderr, "Stopped at entry point 0x%lx\n", (unsigned long)ep);
            }
            else
            {
                fprintf(stderr, "Warning: could not determine entry point from '%s'\n", argv[2]);
            }
        }
    }
    else
    {
        switch (attach_inferior(pid, &status, &signal))
        {
            case -1:
                error("Attaching not supported on this target");
                break;
            default:
                attached = 1;
                break;
        }
    }

    while (1)
    {
        remote_open(argv[1]);

    restart:
        setjmp(toplevel);
        while (getpkt(own_buf) > 0)
        {
            unsigned char sig;
            i = 0;
            ch = own_buf[i++];
            switch (ch)
            {
                /* q — query packets (qSupported, qC, qRcmd, etc.) */
                case 'q':
                    LOG_CMD("q — query packet: %.40s", own_buf);
                    handle_query(own_buf);
                    break;

                /* Q — set packets (QStartNoAckMode, QPassSignals, etc.) */
                case 'Q':
                    LOG_CMD("Q — set packet: %.40s", own_buf);
                    handle_set(own_buf);
                    break;

                /* d — toggle remote debug output */
                case 'd':
                    LOG_CMD("d — toggle remote debug");
                    remote_debug = !remote_debug;
                    break;

                /* D — detach from inferior */
                case 'D':
                    LOG_CMD("D — detach from inferior");
                    fprintf(stderr, "Detaching from inferior\n");
                    detach_inferior();
                    write_ok(own_buf);
                    putpkt(own_buf);
                    remote_close();

                    /* If we are attached, then we can exit.  Otherwise, we need to
           hang around doing nothing, until the child is gone.  */
                    if (!attached)
                    {
                        int status, ret;

                        do
                        {
                            ret = waitpid(signal_pid, &status, 0);
                            if (WIFEXITED(status) || WIFSIGNALED(status))
                            {
                                break;
                            }
                        } while (ret != -1 || errno != ECHILD);
                    }

                    exit(0);

                /* ! — extended protocol request; ack it so IDA proceeds
         with the already-launched process.  */
                case '!':
                    LOG_CMD("! — extended protocol request");
                    write_ok(own_buf);
                    break;

                /* ? — report why the target halted (stop reply) */
                case '?':
                    LOG_CMD("? — query halt reason");
                    prepare_resume_reply(own_buf, status, signal);
                    break;

                /* H — set thread for subsequent operations */
                case 'H':
                    LOG_CMD("H — set thread: %s", own_buf);
                    switch (own_buf[1])
                    {
                        case 'g':
                            /* Hg — set thread for register read/write ops */
                            general_thread = strtol(&own_buf[2], NULL, 16);
                            write_ok(own_buf);
                            set_desired_inferior(1);
                            break;
                        case 'c':
                            /* Hc — set thread for continue/step ops */
                            cont_thread = strtol(&own_buf[2], NULL, 16);
                            write_ok(own_buf);
                            break;
                        case 's':
                            /* Hs — set thread for step ops */
                            step_thread = strtol(&own_buf[2], NULL, 16);
                            write_ok(own_buf);
                            break;
                        default:
                            /* Silently ignore it so that gdb can extend the protocol
             without compatibility headaches.  */
                            own_buf[0] = '\0';
                            break;
                    }
                    break;

                /* g — read all registers */
                case 'g':
                    LOG_CMD("g — read all registers");
                    set_desired_inferior(1);
                    registers_to_string(own_buf);
                    break;

                /* G — write all registers */
                case 'G':
                    LOG_CMD("G — write all registers");
                    set_desired_inferior(1);
                    registers_from_string(&own_buf[1]);
                    write_ok(own_buf);
                    break;

                /* [NEW] p — read single register by number */
                case 'p':
                {
                    int regno = strtol(&own_buf[1], NULL, 16);
                    LOG_CMD("[NEW] p — read register %d", regno);
                    set_desired_inferior(1);
                    if (regno >= 0 && find_register_by_number(regno) != NULL)
                    {
                        collect_register_as_string(regno, own_buf);
                    }
                    else
                    {
                        write_enn(own_buf);
                    }
                }
                break;

                /* [NEW] P — write single register by number */
                case 'P':
                {
                    int regno;
                    char *regbytes;
                    char regbuf[16];

                    regno = strtol(&own_buf[1], &regbytes, 16);
                    LOG_CMD("[NEW] P — write register %d", regno);
                    if (*regbytes == '=')
                    {
                        regbytes++;
                    }

                    set_desired_inferior(1);
                    if (regno >= 0 && find_register_by_number(regno) != NULL)
                    {
                        int regsize = register_size(regno);
                        convert_ascii_to_int(regbytes, regbuf, regsize);
                        supply_register(regno, regbuf);
                        write_ok(own_buf);
                    }
                    else
                    {
                        write_enn(own_buf);
                    }
                }
                break;

                /* m — read memory (m addr,length) */
                case 'm':
                    decode_m_packet(&own_buf[1], &mem_addr, &len);
                    LOG_CMD("m — read memory at 0x%lx len %u", (unsigned long)mem_addr, len);
                    read_inferior_memory(mem_addr, mem_buf, len);
                    convert_int_to_ascii(mem_buf, own_buf, len);
                    break;

                /* M — write memory (M addr,length:data) */
                case 'M':
                    decode_M_packet(&own_buf[1], &mem_addr, &len, mem_buf);
                    LOG_CMD("M — write memory at 0x%lx len %u", (unsigned long)mem_addr, len);
                    if (write_inferior_memory(mem_addr, mem_buf, len) == 0)
                    {
                        write_ok(own_buf);
                    }
                    else
                    {
                        write_enn(own_buf);
                    }
                    break;

                /* C — continue with signal */
                case 'C':
                    LOG_CMD("C — continue with signal");
                    convert_ascii_to_int(own_buf + 1, &sig, 1);
                    if (target_signal_to_host_p(sig))
                    {
                        signal = target_signal_to_host(sig);
                    }
                    else
                    {
                        signal = 0;
                    }
                    set_desired_inferior(0);
                    step_past_breakpoint(0, &status);
                    myresume(0, signal);
                    signal = mywait_cond(&status, 1);
                    prepare_resume_reply(own_buf, status, signal);
                    break;

                /* S — single step with signal */
                case 'S':
                    LOG_CMD("S — step with signal");
                    convert_ascii_to_int(own_buf + 1, &sig, 1);
                    if (target_signal_to_host_p(sig))
                    {
                        signal = target_signal_to_host(sig);
                    }
                    else
                    {
                        signal = 0;
                    }
                    set_desired_inferior(0);
                    if (step_past_breakpoint(1, &status))
                    {
                        signal = TARGET_SIGNAL_TRAP;
                        prepare_resume_reply(own_buf, status, signal);
                        break;
                    }
                    myresume(1, signal);
                    signal = mywait_cond(&status, 1);
                    prepare_resume_reply(own_buf, status, signal);
                    break;

                /* c — continue execution */
                case 'c':
                    LOG_CMD("c — continue");
                    set_desired_inferior(0);
                    step_past_breakpoint(0, &status);
                    myresume(0, 0);
                    signal = mywait_cond(&status, 1);
                    prepare_resume_reply(own_buf, status, signal);
                    break;

                /* s — single step one instruction */
                case 's':
                    LOG_CMD("s — single step");
                    set_desired_inferior(0);
                    if (step_past_breakpoint(1, &status))
                    {
                        signal = TARGET_SIGNAL_TRAP;
                        prepare_resume_reply(own_buf, status, signal);
                        break;
                    }
                    myresume(1, 0);
                    signal = mywait_cond(&status, 1);
                    prepare_resume_reply(own_buf, status, signal);
                    break;

                /* k — kill the inferior */
                case 'k':
                    LOG_CMD("k — kill inferior");
                    fprintf(stderr, "Killing inferior\n");
                    kill_inferior();
                    /* When using the extended protocol, we start up a new
           debugging session.   The traditional protocol will
           exit instead.  */
                    if (extended_protocol)
                    {
                        write_ok(own_buf);
                        fprintf(stderr, "GDBserver restarting\n");

                        /* Wait till we are at 1st instruction in prog.  */
                        signal = start_inferior(&argv[2], &status);
                        goto restart;
                        break;
                    }
                    else
                    {
                        exit(0);
                        break;
                    }

                /* T — check if thread is alive */
                case 'T':
                    LOG_CMD("T — thread alive check: %s", &own_buf[1]);
                    if (mythread_alive(strtol(&own_buf[1], NULL, 16)))
                    {
                        write_ok(own_buf);
                    }
                    else
                    {
                        write_enn(own_buf);
                    }
                    break;

                /* R — restart inferior (extended protocol only) */
                case 'R':
                    LOG_CMD("R — restart inferior");
                    if (extended_protocol)
                    {
                        kill_inferior();
                        write_ok(own_buf);
                        fprintf(stderr, "GDBserver restarting\n");

                        /* Wait till we are at 1st instruction in prog.  */
                        signal = start_inferior(&argv[2], &status);
                        goto restart;
                        break;
                    }
                    else
                    {
                        /* It is a request we don't understand.  Respond with an
             empty packet so that gdb knows that we don't support this
             request.  */
                        own_buf[0] = '\0';
                        break;
                    }

                /* v — extended (long) request packets (vCont, etc.) */
                case 'v':
                    LOG_CMD("v — extended request: %.40s", own_buf);
                    handle_v_requests(own_buf, &status, &signal);
                    break;

                /* [NEW] Z — insert breakpoint (Z0 = software breakpoint) */
                case 'Z':
                {
                    char type = own_buf[1];
                    LOG_CMD("[NEW] Z — insert breakpoint type=%c: %s", type, own_buf);
                    if (type == '0')
                    {
                        CORE_ADDR addr;
                        unsigned int kind;
                        char *p = &own_buf[3]; /* skip "Z0," */
                        addr = (CORE_ADDR)strtoul(p, &p, 16);
                        if (*p == ',')
                        {
                            p++;
                        }
                        kind = strtoul(p, NULL, 16);
                        set_breakpoint_at(addr, NULL);
                        write_ok(own_buf);
                    }
                    else
                    {
                        own_buf[0] = '\0'; /* unsupported Z type */
                    }
                }
                break;

                /* [NEW] z — remove breakpoint (z0 = software breakpoint) */
                case 'z':
                {
                    char type = own_buf[1];
                    LOG_CMD("[NEW] z — remove breakpoint type=%c: %s", type, own_buf);
                    if (type == '0')
                    {
                        write_ok(own_buf);
                    }
                    else
                    {
                        own_buf[0] = '\0';
                    }
                }
                break;

                /* Unknown command — respond with empty packet */
                default:
                    LOG_CMD("unknown command: '%c' (0x%02x)", ch, ch);
                    own_buf[0] = '\0';
                    break;
            }

            putpkt(own_buf);

            if (status == 'W')
            {
                fprintf(stderr, "\nChild exited with status %d\n", sig);
            }
            if (status == 'X')
            {
                fprintf(stderr, "\nChild terminated with signal = 0x%x\n", sig);
            }
            if (status == 'W' || status == 'X')
            {
                if (extended_protocol)
                {
                    fprintf(stderr, "Killing inferior\n");
                    kill_inferior();
                    write_ok(own_buf);
                    fprintf(stderr, "GDBserver restarting\n");

                    /* Wait till we are at 1st instruction in prog.  */
                    signal = start_inferior(&argv[2], &status);
                    goto restart;
                    break;
                }
                else
                {
                    fprintf(stderr, "GDBserver exiting\n");
                    exit(0);
                }
            }
        }

        /* We come here when getpkt fails.

       For the extended remote protocol we exit (and this is the only
       way we gracefully exit!).

       For the traditional remote protocol close the connection,
       and re-open it at the top of the loop.  */
        if (extended_protocol)
        {
            remote_close();
            exit(0);
        }
        else
        {
            fprintf(stderr, "Remote side has terminated connection.  "
                            "GDBserver will reopen the connection.\n");
            remote_close();
        }
    }
}
