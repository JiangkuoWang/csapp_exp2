/* 
 * tsh - A tiny shell program with job control
 * 
 * ID: 220110927 NAME: Jiangkuo Wang
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include "csapp.h"

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS];    /* Argument list execve() */
    char buf[MAXLINE];      /* Holds modified command line */
    int bg;                 /* Should the job run in bg or fg? */
    pid_t pid;              /* Process id */
    sigset_t mask_one, prev_mask; /* Signal masks */

    strcpy(buf, cmdline);
    bg = parseline(buf, argv); 

    if (argv[0] == NULL)
        return;   /* Ignore empty lines */

    if (builtin_cmd(argv)) {
        return; /* Command was a built-in */
    }

    /* Initialize signal set for SIGCHLD */
    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);

    /* Block SIGCHLD */
    if (sigprocmask(SIG_BLOCK, &mask_one, &prev_mask) < 0) {
        unix_error("eval: sigprocmask block error");
        return;
    }

    if ((pid = Fork()) == 0) {   /* Child runs user job */
        /* Unblock SIGCHLD in child */
        if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0) {
            unix_error("eval: child sigprocmask_restore error");
            exit(1); /* Exit if cannot restore mask */
        }

        /* Create a new process group for the child */
        if (setpgid(0, 0) < 0) {
            unix_error("eval: setpgid error");
            exit(1); /* Exit if cannot set new process group */
        }

        if (execve(argv[0], argv, environ) < 0) {
            printf("%s: Command not found\n", argv[0]);
            exit(0); /* Exit with 0 as per lab guide's screenshot */
        }
    }

    /* Parent process */
    int state = bg ? BG : FG;
    if (!addjob(jobs, pid, state, cmdline)) {
        /* If addjob fails (e.g., too many jobs), an error is already printed by addjob. */
        /* The job won't be tracked, which is a limitation. */
    }

    /* Unblock SIGCHLD in parent */
    if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0) {
        unix_error("eval: parent sigprocmask_restore error");
    }

    if (!bg) { 
        waitfg(pid);
    } else {
        struct job_t *job = getjobpid(jobs, pid);
        if (job) { 
            printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
        } else {
            printf("Error: Could not retrieve job info for background process %d\\n", pid);
        }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    if (argv[0] == NULL) { /* Should not happen if called from eval after check */
        return 0;
    }

    if (strcmp(argv[0], "quit") == 0) {
        exit(0);
    }
    else if (strcmp(argv[0], "jobs") == 0) {
        listjobs(jobs); /* listjobs is a provided helper function */
        return 1;
    }
    else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        do_bgfg(argv); /* do_bgfg will handle bg/fg logic */
        return 1;
    }
    /* You can add more built-in commands here if needed */
    /* Example: "&" is handled by parseline, not a builtin_cmd itself */

    return 0;     /* Not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    char *id_str;        /* String for PID or JID */
    struct job_t *job;   /* Pointer to the job struct */
    int jid;
    pid_t pid;

    if (argv[1] == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    id_str = argv[1];

    if (id_str[0] == '%') { /* JID argument */
        jid = atoi(&id_str[1]);
        if (jid == 0) { /* atoi returns 0 for non-numeric or '%0' */
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobjid(jobs, jid);
        if (job == NULL) {
            printf("%s: No such job\n", id_str);
            return;
        }
    } else if (isdigit(id_str[0])) { /* PID argument */
        pid = atoi(id_str);
        if (pid == 0) { /* atoi returns 0 for non-numeric */
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobpid(jobs, pid);
        if (job == NULL) {
            printf("(%d): No such process\n", pid);
            return;
        }
    } else {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }

    /* At this point, 'job' points to a valid job struct */

    if (kill(-(job->pid), SIGCONT) < 0) {
        unix_error("do_bgfg: kill(SIGCONT) error");
        /* SIGCONT error might not be fatal for the shell, but report it */
        /* The job state might not change if kill fails. */
    }

    if (strcmp(argv[0], "bg") == 0) {
        job->state = BG;
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
        /* cmdline includes newline, this is consistent with tshref */
    } else { /* fg command */
        job->state = FG;
        waitfg(job->pid); /* Wait for the foreground job to complete or change state */
    }

    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    if (pid == 0) { /* No process to wait for, or invalid PID */
        return;
    }

    struct job_t *job = getjobpid(jobs, pid);

    /* Check if the job exists and is the foreground job we are interested in */
    /* It might have been reaped by sigchld_handler if it terminated very quickly, */
    /* or its state might have changed (e.g. stopped). */
    if (job == NULL || job->pid != pid) { 
        /* Job doesn't exist or PID mismatch (should not happen if pid comes from a valid fg job) */
        return; 
    }

    /* Loop while the job is still the foreground job and its PID matches */
    /* The state check (job->state == FG) is crucial. */
    /* The pid check (job->pid == pid) ensures we are monitoring the same job, 
       as job structs might be reused if a job is deleted and another added. */
    while (job->state == FG && job->pid == pid) {
        /* Use sigsuspend for a clean wait, as recommended by CSAPP text. */
        /* sleep() is simpler but less robust. The lab guide mentions sleep(1) is okay. */
        /* Let's use sleep(1) as per the guide's allowance for simplicity. */
        /* If using sigsuspend: */
        /* sigset_t mask, prev_mask; */
        /* sigemptyset(&mask); // Empty mask, allows all signals not otherwise blocked */
        /* sigsuspend(&mask); // Atomically unblocks signals and waits for one */
        
        sleep(1); /* Check status roughly every second. */
                 /* In a real shell, sigsuspend would be better here to react immediately to SIGCHLD. */
                 /* When SIGCHLD arrives, its handler will update the job's state or remove it. */
                 /* Then this loop will see the change and terminate. */
    }

    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    int olderrno = errno; /* Save errno, as waitpid can change it */
    pid_t pid;
    int status;
    struct job_t *job;

    /* Reap all available zombie children and handle stopped children */
    /* WNOHANG: return immediately if no child has exited. */
    /* WUNTRACED: also return if a child has stopped (and not just terminated). */
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        job = getjobpid(jobs, pid);
        if (job == NULL) {
            /* This should ideally not happen if all children are launched via addjob */
            /* Could be a child not managed by this shell, or an error */
            if (verbose) {
                printf("sigchld_handler: Job not found for PID %d\n", pid);
            }
            // Sio_error might be too strong here if it exits, use Sio_printf for verbose logging.
            // Let's just continue if job not found, as it might be an external process forked by the job.
            // However, all direct children of the shell should be in the jobs list.
            continue; 
        }

        if (WIFEXITED(status)) { /* Child terminated normally */
            if (verbose) {
                // Using Sio_printf for async-signal-safety if available from csapp.h
                // sprintf(sbuf, "sigchld_handler: Job [%d] (%d) terminated normally with status %d\n", 
                //         job->jid, pid, WEXITSTATUS(status));
                // Sio_puts(sbuf);
            }
            deletejob(jobs, pid);
        } else if (WIFSIGNALED(status)) { /* Child terminated by a signal */
            // Use Sio_ कह safe functions for printing from signal handler
            // We need to compose the message carefully. Standard printf is not safe.
            // The lab guide implies printf might be used, but strictly, it's not safe.
            // Let's compose a message and print it outside if possible, or use Sio if available.
            // For this lab, often simplified signal handling is accepted.
            // The trace files will show what tshref prints.
            // Example: Job [1] (12345) terminated by signal 2 (SIGINT)
            // Let's use a safe way to print, like Sio_psignal if csapp provides it, or construct manually. 
            // The problem description does not strictly enforce async-signal safety for printf, 
            // especially as other parts of shell code use printf.
            // Let's try to build the message as required by traces.
            
            // The csapp.h provides Sio_puts, Sio_error. Let's use Sio_puts with sprintf to sbuf (global).
            // This is a common pattern in CS:APP examples.
            sprintf(sbuf, "Job [%d] (%d) terminated by signal %d\n", 
                    job->jid, pid, WTERMSIG(status));
            Sio_puts(sbuf); // Sio_puts is async-signal-safe
            deletejob(jobs, pid);

        } else if (WIFSTOPPED(status)) { /* Child stopped by a signal */
            job->state = ST;
            sprintf(sbuf, "Job [%d] (%d) stopped by signal %d\n", 
                    job->jid, pid, WSTOPSIG(status));
            Sio_puts(sbuf); // Sio_puts is async-signal-safe
        }
        /* Else: other status changes (WIFCONTINUED) are not explicitly handled by tsh basic requirements */
    }

    /* Check for other errors from waitpid, but not ECHILD (no more children) */
    if (pid < 0 && errno != ECHILD && errno != EINTR) {
        // unix_error("sigchld_handler: waitpid error"); // unix_error exits, too strong for handler
        // Let's use Sio_error which also uses Sio_puts and sets exit_status
        sprintf(sbuf, "sigchld_handler: waitpid error: %s\n", strerror(errno));
        Sio_puts(sbuf); // Report error but don't exit the shell from handler
    }

    errno = olderrno; /* Restore errno */
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    int olderrno = errno; /* Save errno */
    pid_t pid = fgpid(jobs); /* Get PID of the foreground job */

    if (pid != 0) { /* If there is a foreground job */
        if (kill(-pid, SIGINT) < 0) { /* Send SIGINT to the entire foreground process group */
            // unix_error("sigint_handler: kill error"); // unix_error exits, too strong for handler
            // Use Sio_puts for error reporting from handler
            sprintf(sbuf, "sigint_handler: kill(SIGINT) error for PID %d: %s\n", 
                    pid, strerror(errno));
            Sio_puts(sbuf);
        }
    }
    // If there's no foreground job, SIGINT should typically be ignored by the shell itself,
    // or it might terminate the shell if that's the desired behavior (but not for tsh).
    // The default action for SIGINT for the shell process itself is often to terminate.
    // However, the main loop of tsh is robust. This handler ensures only fg job gets it.

    errno = olderrno; /* Restore errno */
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int olderrno = errno; /* Save errno */
    pid_t pid = fgpid(jobs); /* Get PID of the foreground job */

    if (pid != 0) { /* If there is a foreground job */
        if (kill(-pid, SIGTSTP) < 0) { /* Send SIGTSTP to the entire foreground process group */
            // unix_error("sigtstp_handler: kill error"); // unix_error exits, too strong for handler
            // Use Sio_puts for error reporting from handler
            sprintf(sbuf, "sigtstp_handler: kill(SIGTSTP) error for PID %d: %s\n", 
                    pid, strerror(errno));
            Sio_puts(sbuf);
        }
    }
    // If there's no foreground job, SIGTSTP should be ignored by the shell itself.
    // This handler ensures only the fg job gets it.
    // The sigchld_handler will then be responsible for updating the job's state to ST (Stopped).

    errno = olderrno; /* Restore errno */
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
// void unix_error(char *msg)
// {
//     fprintf(stdout, "%s: %s\n", msg, strerror(errno));
//     exit(1);
// }

/*
 * app_error - application-style error routine
 */
// void app_error(char *msg)
// {
//     fprintf(stdout, "%s\n", msg);
//     exit(1);
// }

/*
 * Signal - wrapper for the sigaction function
 */
// handler_t *Signal(int signum, handler_t *handler) 
// {
//     struct sigaction action, old_action;

//     action.sa_handler = handler;  
//     sigemptyset(&action.sa_mask); /* block sigs of type being handled */
//     action.sa_flags = SA_RESTART; /* restart syscalls if possible */

//     if (sigaction(signum, &action, &old_action) < 0)
// 	unix_error("Signal error");
//     return (old_action.sa_handler);
// }

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



