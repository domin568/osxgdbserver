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
            /* PPC_THREAD_STATE layout: flavor, count, srr0, ... */
            unsigned int *data = (unsigned int *)(p + 8);
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

    return mywait(statusptr, 0);
}

static int attach_inferior(int pid, char *statusptr, unsigned char *sigptr)
{
    if (myattach(pid) != 0)
    {
        return -1;
    }

    fprintf(stderr, "Attached; pid = %d\n", pid);

    signal_pid = pid;

    *sigptr = mywait(statusptr, 0);

    return 0;
}

extern int remote_debug;

void handle_query(char *own_buf)
{
    static struct inferior_list_entry *thread_ptr;

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

    if (strncmp("qSupported", own_buf, 10) == 0)
    {
        LOG_CMD("[NEW] qSupported — feature negotiation");
        sprintf(own_buf, "PacketSize=%x;QStartNoAckMode+", PBUFSIZ - 1);
        return;
    }

    if (strcmp("qAttached", own_buf) == 0)
    {
        LOG_CMD("[NEW] qAttached — report process creation mode");
        strcpy(own_buf, "0");
        return;
    }

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

    if (strcmp("qfThreadInfo", own_buf) == 0)
    {
        LOG_CMD("qfThreadInfo — first thread info query");
        thread_ptr = all_threads.head;
        sprintf(own_buf, "m%x", thread_ptr->id);
        thread_ptr = thread_ptr->next;
        return;
    }

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

    if (the_target->read_auxv != NULL && strncmp("qPart:auxv:read::", own_buf, 17) == 0)
    {
        char data[(PBUFSIZ - 1) / 2];
        CORE_ADDR ofs;
        unsigned int len;
        int n;
        LOG_CMD("qPart:auxv:read — read auxiliary vector");
        decode_m_packet(&own_buf[17], &ofs, &len);
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

    if (strncmp("qRcmd,", own_buf, 6) == 0)
    {
        LOG_CMD("[NEW] qRcmd — remote monitor command");
        handle_rcmd(own_buf + 6, own_buf);
        return;
    }

    /* Unknown packet — empty reply means "not supported".  */
    own_buf[0] = 0;
}

void handle_set(char *own_buf)
{
    /* QStartNoAckMode: reply OK *before* enabling no-ack so that the OK
       reply itself is still acknowledged by the client.  */
    if (strcmp("QStartNoAckMode", own_buf) == 0)
    {
        LOG_CMD("[NEW] QStartNoAckMode — disabling ACK handshake");
        write_ok(own_buf);
        no_ack_mode = 1;
        return;
    }

    if (strncmp("QPassSignals:", own_buf, 13) == 0)
    {
        LOG_CMD("QPassSignals — acknowledging signal pass list (no-op)");
        write_ok(own_buf);
        return;
    }

    if (strncmp("QNonStop:", own_buf, 9) == 0)
    {
        LOG_CMD("QNonStop — not supported, ignoring");
        own_buf[0] = '\0';
        return;
    }

    LOG_CMD("unknown Q command: '%.40s'", own_buf);
    own_buf[0] = '\0';
}

/* Parse vCont packets.  */
void handle_v_cont(char *own_buf, char *status, unsigned char *signal)
{
    char *p, *q;
    int n = 0, i = 0;
    struct thread_resume *resume_info, default_action;

    /* One action per ';' in the packet, plus one default-action slot.  */
    p = &own_buf[5];
    while (p)
    {
        n++;
        p++;
        p = strchr(p, ';');
    }
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
            /* Don't increment i; this slot will be reused.  */
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

    if (n == 1 && resume_info[0].thread != -1)
    {
        cont_thread = resume_info[0].thread;
    }
    else
    {
        cont_thread = -1;
    }
    set_desired_inferior(0);

    /* If the first action is a step and PC sits on a breakpoint, the
       step-past IS the user's step.  */
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
    strcpy(own_buf, "");
    free(resume_info);
    return;
}

void handle_v_requests(char *own_buf, char *status, unsigned char *signal)
{
    if (strncmp(own_buf, "vCont;", 6) == 0)
    {
        LOG_CMD("vCont — resume with actions: %s", own_buf + 5);
        handle_v_cont(own_buf, status, signal);
        return;
    }

    if (strncmp(own_buf, "vCont?", 6) == 0)
    {
        LOG_CMD("vCont? — query supported resume actions");
        strcpy(own_buf, "vCont;c;C;s;S");
        return;
    }

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
   temporarily remove it, single-step the real instruction, and reinsert
   it.  Returns 1 if a breakpoint was stepped past, 0 if not.  */
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

    uninsert_breakpoint(pc);
    myresume(1, 0);
    sig = mywait(statusp, 1);
    reinsert_breakpoint(pc);

    /* Re-fetch registers so the regcache reflects post-step state;
       prepare_resume_reply reads expedited regs (r1, pc) from it.  */
    set_desired_inferior(1);

    (void)is_step;
    (void)sig;
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

/* Wait for the inferior, transparently handling conditional breakpoints
   whose conditions are not met (silently step over and continue).  */
static unsigned char mywait_cond(char *statusp, int connected)
{
    unsigned char sig;

    for (;;)
    {
        sig = mywait(statusp, connected);

        if (*statusp == 'T' && sig == TARGET_SIGNAL_TRAP)
        {
            unsigned int pc32 = 0;
            CORE_ADDR pc;
            int cond;

            set_desired_inferior(1);
            collect_register_by_name("pc", &pc32);
            pc = (CORE_ADDR)pc32;

            cond = check_cond_bp(pc);
            if (cond == 2)
            {
                /* Condition not met — step over and continue.  */
                uninsert_breakpoint(pc);
                myresume(1, 0);
                sig = mywait(statusp, connected);
                reinsert_breakpoint(pc);
                regcache_invalidate();
                myresume(0, 0);
                continue;
            }
        }

        return sig;
    }
}

/* ---------------- Per-packet handlers ----------------------------------- */

static void handle_d_toggle_debug(char *buf)
{
    LOG_CMD("d — toggle remote debug");
    remote_debug = !remote_debug;
    (void)buf;
}

static void handle_D_detach(char *buf)
{
    LOG_CMD("D — detach from inferior");
    fprintf(stderr, "Detaching from inferior\n");
    detach_inferior();
    write_ok(buf);
    putpkt(buf);
    remote_close();

    if (!attached)
    {
        int wstatus, ret;
        do
        {
            ret = waitpid(signal_pid, &wstatus, 0);
            if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus))
            {
                break;
            }
        } while (ret != -1 || errno != ECHILD);
    }
    exit(0);
}

static void handle_question(char *buf, char status, unsigned char sig)
{
    LOG_CMD("? — query halt reason");
    prepare_resume_reply(buf, status, sig);
}

static void handle_H_set_thread(char *buf)
{
    LOG_CMD("H — set thread: %s", buf);
    switch (buf[1])
    {
        case 'g':
            general_thread = strtol(&buf[2], NULL, 16);
            write_ok(buf);
            set_desired_inferior(1);
            break;
        case 'c':
            cont_thread = strtol(&buf[2], NULL, 16);
            write_ok(buf);
            break;
        case 's':
            step_thread = strtol(&buf[2], NULL, 16);
            write_ok(buf);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static void handle_g_read_regs(char *buf)
{
    LOG_CMD("g — read all registers");
    set_desired_inferior(1);
    registers_to_string(buf);
}

static void handle_G_write_regs(char *buf)
{
    LOG_CMD("G — write all registers");
    set_desired_inferior(1);
    registers_from_string(&buf[1]);
    write_ok(buf);
}

static void handle_p_read_reg(char *buf)
{
    int regno = strtol(&buf[1], NULL, 16);
    LOG_CMD("p — read register %d", regno);
    set_desired_inferior(1);
    if (regno >= 0 && find_register_by_number(regno) != NULL)
    {
        collect_register_as_string(regno, buf);
    }
    else
    {
        write_enn(buf);
    }
}

static void handle_P_write_reg(char *buf)
{
    char *regbytes;
    int regno = strtol(&buf[1], &regbytes, 16);
    LOG_CMD("P — write register %d", regno);
    if (*regbytes == '=')
    {
        regbytes++;
    }
    set_desired_inferior(1);
    if (regno >= 0 && find_register_by_number(regno) != NULL)
    {
        char regbuf[16];
        int regsize = register_size(regno);
        convert_ascii_to_int(regbytes, regbuf, regsize);
        supply_register(regno, regbuf);
        write_ok(buf);
    }
    else
    {
        write_enn(buf);
    }
}

static void handle_m_read_mem(char *buf, char *mem_buf)
{
    CORE_ADDR addr;
    unsigned int len;
    decode_m_packet(&buf[1], &addr, &len);
    LOG_CMD("m — read memory at 0x%lx len %u", (unsigned long)addr, len);
    read_inferior_memory(addr, mem_buf, len);
    convert_int_to_ascii(mem_buf, buf, len);
}

static void handle_M_write_mem(char *buf, char *mem_buf)
{
    CORE_ADDR addr;
    unsigned int len;
    decode_M_packet(&buf[1], &addr, &len, mem_buf);
    LOG_CMD("M — write memory at 0x%lx len %u", (unsigned long)addr, len);
    if (write_inferior_memory(addr, mem_buf, len) == 0)
    {
        write_ok(buf);
    }
    else
    {
        write_enn(buf);
    }
}

static unsigned char parse_signal_byte(const char *p)
{
    unsigned char sig;
    convert_ascii_to_int((char *)p, &sig, 1);
    if (target_signal_to_host_p(sig))
    {
        return target_signal_to_host(sig);
    }
    return 0;
}

static void handle_C_continue_signal(char *buf, char *status, unsigned char *sig_out)
{
    unsigned char sig = parse_signal_byte(buf + 1);
    LOG_CMD("C — continue with signal %u", sig);
    set_desired_inferior(0);
    step_past_breakpoint(0, status);
    myresume(0, sig);
    *sig_out = mywait_cond(status, 1);
    prepare_resume_reply(buf, *status, *sig_out);
}

static void handle_S_step_signal(char *buf, char *status, unsigned char *sig_out)
{
    unsigned char sig = parse_signal_byte(buf + 1);
    LOG_CMD("S — step with signal %u", sig);
    set_desired_inferior(0);
    if (step_past_breakpoint(1, status))
    {
        *sig_out = TARGET_SIGNAL_TRAP;
        prepare_resume_reply(buf, *status, *sig_out);
        return;
    }
    myresume(1, sig);
    *sig_out = mywait_cond(status, 1);
    prepare_resume_reply(buf, *status, *sig_out);
}

static void handle_c_continue(char *buf, char *status, unsigned char *sig_out)
{
    LOG_CMD("c — continue");
    set_desired_inferior(0);
    step_past_breakpoint(0, status);
    myresume(0, 0);
    *sig_out = mywait_cond(status, 1);
    prepare_resume_reply(buf, *status, *sig_out);
}

static void handle_s_step(char *buf, char *status, unsigned char *sig_out)
{
    LOG_CMD("s — single step");
    set_desired_inferior(0);
    if (step_past_breakpoint(1, status))
    {
        *sig_out = TARGET_SIGNAL_TRAP;
        prepare_resume_reply(buf, *status, *sig_out);
        return;
    }
    myresume(1, 0);
    *sig_out = mywait_cond(status, 1);
    prepare_resume_reply(buf, *status, *sig_out);
}

/* Returns 1 if the inferior was restarted (caller should `goto restart`).  */
static int handle_k_kill(char *buf, char **argv, char *status, unsigned char *sig_out)
{
    LOG_CMD("k — kill inferior");
    fprintf(stderr, "Killing inferior\n");
    kill_inferior();

    if (extended_protocol)
    {
        write_ok(buf);
        fprintf(stderr, "GDBserver restarting\n");
        *sig_out = start_inferior(&argv[2], status);
        return 1;
    }
    exit(0);
}

static void handle_T_thread_alive(char *buf)
{
    LOG_CMD("T — thread alive check: %s", &buf[1]);
    if (mythread_alive(strtol(&buf[1], NULL, 16)))
    {
        write_ok(buf);
    }
    else
    {
        write_enn(buf);
    }
}

/* Returns 1 if the inferior was restarted.  */
static int handle_R_restart(char *buf, char **argv, char *status, unsigned char *sig_out)
{
    LOG_CMD("R — restart inferior");
    if (extended_protocol)
    {
        kill_inferior();
        write_ok(buf);
        fprintf(stderr, "GDBserver restarting\n");
        *sig_out = start_inferior(&argv[2], status);
        return 1;
    }
    buf[0] = '\0';
    return 0;
}

static void handle_Z_insert_bp(char *buf)
{
    char type = buf[1];
    LOG_CMD("Z — insert breakpoint type=%c: %s", type, buf);
    if (type == '0')
    {
        char *p = &buf[3];
        CORE_ADDR addr = (CORE_ADDR)strtoul(p, &p, 16);
        if (*p == ',')
        {
            p++;
        }
        (void)strtoul(p, NULL, 16); /* kind, unused */
        set_breakpoint_at(addr, NULL);
        write_ok(buf);
    }
    else
    {
        buf[0] = '\0';
    }
}

static void handle_z_remove_bp(char *buf)
{
    char type = buf[1];
    LOG_CMD("z — remove breakpoint type=%c: %s", type, buf);
    if (type == '0')
    {
        write_ok(buf);
    }
    else
    {
        buf[0] = '\0';
    }
}

/* Dispatch a single packet.  Returns 1 if the inferior was restarted
   (caller should `goto restart` and skip the reply send).  */
static int handle_packet(char *buf, char **argv, char *status, unsigned char *sig_out,
                         char *mem_buf)
{
    switch (buf[0])
    {
        case 'q':
            LOG_CMD("q — query: %.40s", buf);
            handle_query(buf);
            return 0;
        case 'Q':
            LOG_CMD("Q — set: %.40s", buf);
            handle_set(buf);
            return 0;
        case 'd':
            handle_d_toggle_debug(buf);
            return 0;
        case 'D':
            handle_D_detach(buf); /* never returns */
            return 0;
        case '!':
            LOG_CMD("! — extended protocol");
            write_ok(buf);
            return 0;
        case '?':
            handle_question(buf, *status, *sig_out);
            return 0;
        case 'H':
            handle_H_set_thread(buf);
            return 0;
        case 'g':
            handle_g_read_regs(buf);
            return 0;
        case 'G':
            handle_G_write_regs(buf);
            return 0;
        case 'p':
            handle_p_read_reg(buf);
            return 0;
        case 'P':
            handle_P_write_reg(buf);
            return 0;
        case 'm':
            handle_m_read_mem(buf, mem_buf);
            return 0;
        case 'M':
            handle_M_write_mem(buf, mem_buf);
            return 0;
        case 'C':
            handle_C_continue_signal(buf, status, sig_out);
            return 0;
        case 'S':
            handle_S_step_signal(buf, status, sig_out);
            return 0;
        case 'c':
            handle_c_continue(buf, status, sig_out);
            return 0;
        case 's':
            handle_s_step(buf, status, sig_out);
            return 0;
        case 'k':
            return handle_k_kill(buf, argv, status, sig_out);
        case 'T':
            handle_T_thread_alive(buf);
            return 0;
        case 'R':
            return handle_R_restart(buf, argv, status, sig_out);
        case 'v':
            LOG_CMD("v — extended: %.40s", buf);
            handle_v_requests(buf, status, sig_out);
            return 0;
        case 'Z':
            handle_Z_insert_bp(buf);
            return 0;
        case 'z':
            handle_z_remove_bp(buf);
            return 0;
        default:
            LOG_CMD("unknown command: '%c' (0x%02x)", buf[0], buf[0]);
            buf[0] = '\0';
            return 0;
    }
}

/* React to inferior exit/termination.  Returns 1 if a restart was triggered. */
static int handle_terminated_inferior(char *buf, char **argv, char *status, unsigned char *sig_out)
{
    if (*status == 'W')
    {
        fprintf(stderr, "\nChild exited with status %d\n", *sig_out);
    }
    else if (*status == 'X')
    {
        fprintf(stderr, "\nChild terminated with signal = 0x%x\n", *sig_out);
    }
    else
    {
        return 0;
    }

    if (extended_protocol)
    {
        fprintf(stderr, "Killing inferior\n");
        kill_inferior();
        write_ok(buf);
        fprintf(stderr, "GDBserver restarting\n");
        *sig_out = start_inferior(&argv[2], status);
        return 1;
    }

    fprintf(stderr, "GDBserver exiting\n");
    exit(0);
}

/* Parse argv. Returns 0 = launch new process, 1 = attach mode,
   -1 = bad usage.  On success, writes the pid to *out_pid (0 for launch).  */
static int parse_args(int argc, char *argv[], int *out_pid)
{
    *out_pid = 0;
    if (argc < 3)
    {
        return -1;
    }
    if (strcmp(argv[2], "--attach") != 0)
    {
        return 0;
    }
    if (argc != 4 || argv[3][0] == '\0')
    {
        return -1;
    }
    {
        char *end;
        unsigned long pid = strtoul(argv[3], &end, 10);
        if (pid == 0 || *end != '\0')
        {
            return -1;
        }
        *out_pid = (int)pid;
    }
    return 1;
}

/* After the inferior stops in dyld, run to the program's real entry
   point so the user lands in their own code.  */
static void run_to_entry_point(const char *path, char *status, unsigned char *sig_out)
{
    CORE_ADDR ep = macho_get_entry_point(path);
    if (ep == 0)
    {
        fprintf(stderr, "Warning: could not determine entry point from '%s'\n", path);
        return;
    }
    fprintf(stderr, "Entry point at 0x%lx — running to it\n", (unsigned long)ep);
    set_breakpoint_at(ep, NULL);
    myresume(0, 0);
    *sig_out = mywait(status, 0);
    delete_breakpoint_at(ep);
    fprintf(stderr, "Stopped at entry point 0x%lx\n", (unsigned long)ep);
}

/* ---------------- main and its helpers --------------------------------- */

static void init_logging(void)
{
    const char *log_env = getenv("LOG");
    log_enabled = (log_env && log_env[0] == '1');
}

/* Bring up the debug target: either launch a new inferior and run it to
   its real entry point, or attach to an existing pid.  */
static void prepare_inferior(int argc, char *argv[], int pid, char *status, unsigned char *sig)
{
    (void)argc;
    if (!attached)
    {
        *sig = start_inferior(&argv[2], status);
        run_to_entry_point(argv[2], status, sig);
        return;
    }
    if (attach_inferior(pid, status, sig) < 0)
    {
        error("Attaching not supported on this target");
    }
}

/* Run a single GDB session: pump packets until the connection drops or
   the inferior is restarted (in which case we re-enter the loop).  */
static void packet_loop(char *own_buf, char *mem_buf, char **argv, char *status,
                        unsigned char *sig)
{
restart:
    setjmp(toplevel);
    while (getpkt(own_buf) > 0)
    {
        if (handle_packet(own_buf, argv, status, sig, mem_buf))
        {
            goto restart;
        }
        putpkt(own_buf);
        if (handle_terminated_inferior(own_buf, argv, status, sig))
        {
            goto restart;
        }
    }
}

int main(int argc, char *argv[])
{
    char status;
    unsigned char sig = 0;
    char *own_buf;
    char mem_buf[2000];
    int pid;

    if (setjmp(toplevel))
    {
        fprintf(stderr, "Exiting\n");
        exit(1);
    }

    init_logging();

    switch (parse_args(argc, argv, &pid))
    {
        case -1:
            gdbserver_usage();
            break;
        case 1:
            attached = 1;
            break;
        default:
            attached = 0;
            break;
    }

    initialize_low();
    own_buf = malloc(PBUFSIZ);

    prepare_inferior(argc, argv, pid, &status, &sig);

    for (;;)
    {
        remote_open(argv[1]);
        packet_loop(own_buf, mem_buf, argv, &status, &sig);

        if (extended_protocol)
        {
            remote_close();
            exit(0);
        }
        fprintf(stderr, "Remote side has terminated connection.  "
                        "GDBserver will reopen the connection.\n");
        remote_close();
    }
}
