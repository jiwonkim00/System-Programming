//--------------------------------------------------------------------------------------------------
// Shell Lab                               Spring 2024                           System Programming
//
/// @file
/// @brief csapsh - a tiny shell with job control
/// @author Jiwon Kim
/// @studid 2019-11563
///
/// @section changelog Change Log
/// 2020/11/14 Bernhard Egger adapted from CS:APP lab
/// 2021/11/03 Bernhard Egger improved for 2021 class
/// 2024/05/11 ARC lab improved for 2024 class
///
/// @section license_section License
/// Copyright CS:APP authors
/// Copyright (c) 2020-2023, Computer Systems and Platforms Laboratory, SNU
/// Copyright (c) 2024, Architecture and Code Optimization Laboratory, SNU
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without modification, are permitted
/// provided that the following conditions are met:
///
/// - Redistributions of source code must retain the above copyright notice, this list of condi-
///   tions and the following disclaimer.
/// - Redistributions in binary form must reproduce the above copyright notice, this list of condi-
///   tions and the following disclaimer in the documentation and/or other materials provided with
///   the distribution.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
/// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF MERCHANTABILITY
/// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
/// CONTRIBUTORS  BE LIABLE FOR ANY DIRECT,  INDIRECT, INCIDENTAL, SPECIAL,  EXEMPLARY,  OR CONSE-
/// QUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)  HOWEVER CAUSED AND ON ANY THEORY OF
/// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
/// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
/// DAMAGE.
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE          // to get basename() in string.h
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jobcontrol.h"
#include "parser.h"

//--------------------------------------------------------------------------------------------------
// Global variables
//

char prompt[] = "csapsh> ";  ///< command line prompt (DO NOT CHANGE)
int emit_prompt = 1;         ///< 1: emit prompt; 0: do not emit prompt
int verbose = 0;             ///< 1: verbose mode; 0: normal mode


//--------------------------------------------------------------------------------------------------
// Functions that you need to implement
//
// Refer to the detailed descriptions at each function implementation.

void eval(char *cmdline);
int  builtin_cmd(char *argv[]);
void do_bgfg(char *argv[]);
void waitfg(int jid);

void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);


//--------------------------------------------------------------------------------------------------
// Implemented functions - do not modify
//

// main & helper functions
int main(int argc, char **argv);
void usage(const char *program);
void unix_error(char *msg);
void app_error(char *msg);
void Signal(int signum, void (*handler)(int));
void sigquit_handler(int sig);
char* stripnewline(char *str);

#define VERBOSE(...)  { if (verbose) { fprintf(stderr, ##__VA_ARGS__); fprintf(stderr, "\n"); } }





/// @brief Program entry point.
int main(int argc, char **argv)
{
  char c;
  char cmdline[MAXLINE];

  // redirect stderr to stdout so that the driver will get all output on the pipe connected 
  // to stdout.
  dup2(STDOUT_FILENO, STDERR_FILENO);

  // set Standard I/O's buffering mode for stdout and stderr to line buffering
  // to avoid any discrepancies between running the shell interactively or via the driver
  setlinebuf(stdout);
  setlinebuf(stderr);

  // parse command line
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h': usage(argv[0]);        // print help message
                break;
      case 'v': verbose = 1;           // emit additional diagnostic info
                break;
      case 'p': emit_prompt = 0;       // don't print a prompt
                break;                 // handy for automatic testing
      default:  usage(argv[0]);        // invalid option -> print help message
    }
  }

  // install signal handlers
  VERBOSE("Installing signal handlers...");
  Signal(SIGINT,  sigint_handler);     // Ctrl-c
  Signal(SIGTSTP, sigtstp_handler);    // Ctrl-z
  Signal(SIGCHLD, sigchld_handler);    // Terminated or stopped child
  Signal(SIGQUIT, sigquit_handler);    // Ctrl-Backslash (useful to exit shell)

  // execute read/eval loop
  VERBOSE("Execute read/eval loop...");
  while (1) {
    if (emit_prompt) { printf("%s", prompt); fflush(stdout); }

    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) {
      app_error("fgets error");
    }

    if (feof(stdin)) break;            // end of input (Ctrl-d)

    eval(cmdline);

    fflush(stdout);
  }

  // that's all, folks!
  return EXIT_SUCCESS;
}



/// @brief Evaluate the command line. The function @a parse_cmdline() does the heavy lifting of 
///        parsing the command line and splitting it into separate char **argv[] arrays that 
///        represent individual commands with their arguments. 
///        A command line consists of one job or several jobs connected via ampersand('&'). And
///        a job consists of one process or several processes connected via pipes. Optionally,
///        the output of the entire job can be saved into a file specified by outfile.
///        The shell waits for jobs that are executed in the foreground, while jobs that run
///        in the background are not waited for.
/// @param cmdline command line

void eval(char *cmdline)
{
  #define P_READ  0                      // pipe read end
  #define P_WRITE 1                      // pipe write end

  char *str = strdup(cmdline);
  VERBOSE("eval(%s)", stripnewline(str));
  free(str);

  char ****argv  = NULL;
  char **infile  = NULL;
  char **outfile = NULL;
  char **commands = NULL;
  int *num_cmds = NULL;
  JobState *mode = NULL;

  // parse command line
  int njob = parse_cmdline(cmdline, &mode, &argv, &infile, &outfile, &num_cmds, &commands);
  VERBOSE("parse_cmdline(...) = %d", njob);
  if (njob == -1) return;              // parse error
  if (njob == 0)  return;              // no input
  assert(njob > 0);

  // dump parsed command line
  for (int job_idx=0; job_idx<njob; job_idx++) {
    if (verbose) dump_cmdstruct(argv[job_idx], infile[job_idx], outfile[job_idx], mode[job_idx]);
  }

  // if the command is a single built-in command (no pipes or redirection), do not fork. Instead,
  // execute the command directly in this process. Note that this is not just to be more efficient -
  // it is necessary for the 'quit' command to work.
  if ((njob == 1) && (num_cmds[0] == 1) && (outfile[0] == NULL)) {
    if (builtin_cmd(argv[0][0])) {
      VERBOSE("builtin_cmd(%s)", argv[0][0][0]);
      free_cmdstruct(argv, infile, outfile, mode);
      return;
    }
  }
  
  //
  // TODO
  //
  VERBOSE("njob: %d", njob);
  for (int job_idx = 0; job_idx < njob; job_idx++) {

    int num_processes = num_cmds[job_idx];
    int pipes[num_processes - 1][2];

    // Create pipes
    for (int i = 0; i < num_processes - 1; i++) {
      if (pipe(pipes[i]) == -1) {
        unix_error("pipe");
        exit(EXIT_FAILURE);
      }
    }

    // Block SIGCHLD signal
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if(sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
      unix_error("sigprocmask error");
    }

    // Fork child processes
    pid_t pid;
    pid_t pgid = 0;
    pid_t* pids = (pid_t*) calloc(num_processes, sizeof(pid_t));

    int input_fd = STDIN_FILENO;
    int output_fd = STDOUT_FILENO;

    for (int cmd_idx = 0; cmd_idx < num_processes; cmd_idx++) {
      
      
      pid = fork();

      if (pid == -1) {
        unix_error("fork");
        exit(EXIT_FAILURE);
      } 

      else if (pid == 0) {
        // Child process

        // 1) Set pgid for the first command in foreground mode
        if ((pgid == -1) && (cmd_idx == 0)) {
          pgid = getpid();
        }
        setpgid(getpid(), pgid);   
        
        // 2) Redirect input if it's the first command and infile is specified
        if (infile[job_idx] && cmd_idx == 0) {
          input_fd = open(infile[job_idx], O_RDONLY);
          if (input_fd == -1) {
            unix_error("open infile");
            exit(EXIT_FAILURE);
          }
          if (input_fd != STDIN_FILENO) {  // If fd is not stdin, redirect it to stdin (0
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
          }
        }
          
        // 3) Redirect output if it's the last command and outfile is specified
        if (outfile[job_idx] && cmd_idx == num_processes - 1) {
          output_fd = open(outfile[job_idx], O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU|S_IRWXG|S_IRWXO);
          if (output_fd == -1) {
            unix_error("open outfile");
            exit(EXIT_FAILURE);
          }
          if (output_fd != STDOUT_FILENO) {  // If fd is not stdout, redirect it to stdout (1)
            if (dup2(output_fd, STDOUT_FILENO) == -1) {
              unix_error("dup2");
              exit(EXIT_FAILURE);
            }
            close(output_fd);
            }
        }

        if (cmd_idx > 0) {
          // Not the first command, get input from the previous pipe
          dup2(pipes[cmd_idx - 1][P_READ], STDIN_FILENO);
        }
        if (cmd_idx < num_processes - 1) {
          // Not the last command, output to the next pipe
          dup2(pipes[cmd_idx][P_WRITE], STDOUT_FILENO);
        }

        // Close all pipes in child process
        for (int i = 0; i < num_processes - 1; i++) {
          close(pipes[i][P_READ]);
          close(pipes[i][P_WRITE]);
        }

        // Unblock SIGCHLD signal
        if(sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0) {
          unix_error("sigprocmask error");
        }
        if (execvp(argv[job_idx][cmd_idx][0], argv[job_idx][cmd_idx]) == -1) {
          app_error("No such file or directory");
          exit(EXIT_FAILURE);
        }

      } 
      else {
        // Parent process

        // if ((cmd_idx == 0) && (pgid == -1)) {
        //   // Set pgid for the first command
        //   pgid = pid;
        // }
        // pids[cmd_idx] = pid;
        if ((cmd_idx == 0) && (pgid == 0)) {
          pgid = pid;
        }
        setpgid(pid, pgid);
        pids[cmd_idx] = pid;

        //close pipes in parent process
        if (num_processes > 1) {
          if (cmd_idx> 0) {
            close(pipes[cmd_idx - 1][P_READ]);
          } 
          if (cmd_idx < num_processes - 1) {
            close(pipes[cmd_idx][P_WRITE]);
          }
        }
        
      }
    }

    // Add job to job list

    // print all pids in 'pids' array
    for (int i = 0; i < num_processes; i++) {
      VERBOSE("pids[%d] : %d", i, pids[i]);  
    }

    int jid = addjob(pgid, pids, num_processes, mode[job_idx], commands[job_idx]);
    VERBOSE("addjob(PGID: %d, JID: %d, %d, %s)", pgid, jid, mode[job_idx], commands[job_idx])
    // Unblock SIGCHLD signal
    if(sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0) {
      unix_error("sigprocmask error");
    }

    if (mode[job_idx] == jsForeground) {
      waitfg(jid);
      // for (int i = 0; i < num_processes; i++) {
      //   int status;
      //   waitpid(-1, &status, 0);  // Wait for each child process
      // }
    } else {
      printjob(jid);
      // for (int i = 0; i < num_processes; i++) {
      //   printjob(jid);  // Print job details for background processes
      // }
    }
    // Close all pipes
    for (int i = 0; i < num_processes - 1; i++) {
      close(pipes[i][P_READ]);
      close(pipes[i][P_WRITE]);
    }
  }
  // free_cmdstruct(argv, infile, outfile, mode);

  // These lines are placeholder for minimal functionality.
  // You can either use these or not.
  // int jid = -1;
  // if (mode[0] == jsForeground) waitfg(jid);
  // else printjob(jid);
}


/// @brief Execute built-in commands
/// @param argv command
/// @retval 1 if the command was a built-in command
/// @retval 0 otherwise
int builtin_cmd(char *argv[])
{
  // VERBOSE("builtin_cmd(%s)", argv[0]);
  
  //
  // TODO
  //

  if      (strcmp(argv[0], "quit") == 0) exit(EXIT_SUCCESS);
  else if (strcmp(argv[0], "jobs") == 0) {
    listjobs();
    return 1;
  }
  else if ((strcmp(argv[0], "bg") == 0) || (strcmp(argv[0], "fg") == 0)){
    do_bgfg(argv);
    return 1;
  }

  return 0;
}

/// @brief Execute the builtin bg and fg commands
/// @param argv char* argv[] array where 
///           argv[0] is either "bg" or "fg"
///           argv[1] is either a job id "%<n>", a process group id "@<n>" or a process id "<n>"
void do_bgfg(char *argv[])
{
  VERBOSE("do_bgfg(%s, %s)", argv[0], argv[1]);

  if (argv[1] == NULL) {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }

  //
  // TODO
  //

  pid_t pgid;
  Job *job;

  if (argv[1][0] == '@') { //pgid
    job = getjob_pgid(atoi(argv[1] + 1));
    
    if (job == NULL) {
      printf("(@%d): No such process group\n", atoi(argv[1] + 1));
      return;
    }
    
  } else if (argv[1][0] == '%') {  //jid
    job = getjob_jid(atoi(argv[1] + 1));
    
    if (job == NULL) {
      printf("[%%%d]: No such job\n", atoi(argv[1] + 1));
      return;
    }

  } else {  //pid
    job = getjob_pid(atoi(argv[1]));
    if (job == NULL) {
      printf("{%d}: No such process\n", atoi(argv[1]));
      return;
    }

  }

  pgid = job->pgid;
  
  if (strcmp(argv[0], "bg") == 0) { //background
    job->state = jsBackground;
    printjob(job->jid);

    if (kill(-pgid, SIGCONT) == -1) {
        unix_error("kill error");
    }
    
    
  } else if (strcmp(argv[0], "fg") == 0) {  //foreground
    job->state = jsForeground;
    if (kill(-pgid, SIGCONT) == -1) {
        unix_error("kill error");
    }
    waitfg(job->jid);
  } 

}

/// @brief Block until job jid is no longer in the foreground
/// @param jid job ID of foreground job
void waitfg(int jid)
{
  if (verbose) {
    fprintf(stderr, "waitfg(%%%d): ", jid);
    printjob(jid);
  }

  //
  // TODO
  //

  Job* job = getjob_jid(jid);
  while (job->state == jsForeground ) {
    sleep(1);
    VERBOSE("waitfg: sleep(1)");
  }
  usleep(1000);
  return;

}


//--------------------------------------------------------------------------------------------------
// Signal handlers
//

/// @brief SIGCHLD handler. Sent to the shell whenever a child process terminates or stops because
///        it received a SIGSTOP or SIGTSTP signal. This handler reaps all zombies.
/// @param sig signal (SIGCHLD)
void sigchld_handler(int sig)
{
  VERBOSE("[SCH] SIGCHLD handler (signal: %d)", sig);

  //
  // TODO
  //
  int old_errno = errno;  
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
    
    Job *job = getjob_pid(pid);
    if (job == NULL) {
      app_error("getjob_pid error");
    } 
    int jid = job->jid;
    
    if (WIFEXITED(status) || WIFSIGNALED(status)) {

      if ((--(job->nproc_cur)) == 0) {
        if (job->state == jsForeground) {
          job->state = jsUndefined;
        }
        
        if (deletejob(jid)) {
          VERBOSE("deletejob(%d)", jid); 
        } else {
          VERBOSE("deletejob error(%d)", jid);
        }
        
      }
      
      VERBOSE("Child %d terminated with exit status %d\n", pid, WEXITSTATUS(status));
    } 
    else if (WIFSTOPPED(status)) {
      job->state = jsStopped;
      VERBOSE("Child %d stopped by signal %d\n", pid, WSTOPSIG(status));
    }
  }
  errno = old_errno;  
}

/// @brief SIGINT handler. Sent to the shell whenever the user types Ctrl-c at the keyboard.
///        Forward the signal to the foreground job.
/// @param sig signal (SIGINT)
void sigint_handler(int sig)
{
  VERBOSE("[SIH] SIGINT handler (signal: %d)", sig);

  //
  // TODO
  //
  Job *job = getjob_foreground();
  if (job != NULL) {
    if (kill(-job->pgid, SIGINT) == -1) {
      unix_error("kill error");
    }
  }

}

/// @brief SIGTSTP handler. Sent to the shell whenever the user types Ctrl-z at the keyboard.
///        Forward the signal to the foreground job.
/// @param sig signal (SIGTSTP)
void sigtstp_handler(int sig)
{
  VERBOSE("[SSH] SIGTSTP handler (signal: %d)", sig);

  //
  // TODO
  //
  Job *job = getjob_foreground();
  if (job != NULL) {
    if (kill(-job->pgid, SIGTSTP) == -1) {
      unix_error("kill error");
    }
  }
}


//--------------------------------------------------------------------------------------------------
// Other helper functions
//

/// @brief Print help message. Does not return.
__attribute__((noreturn))
void usage(const char *program)
{
  printf("Usage: %s [-hvp]\n", basename(program));
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(EXIT_FAILURE);
}

/// @brief Print a Unix-level error message based on errno. Does not return.
/// param msg additional descriptive string (optional)
__attribute__((noreturn))
void unix_error(char *msg)
{
  if (msg != NULL) fprintf(stdout, "%s: ", msg);
  fprintf(stdout, "%s\n", strerror(errno));
  exit(EXIT_FAILURE);
}

/// @brief Print an application-level error message. Does not return.
/// @param msg error message
__attribute__((noreturn))
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(EXIT_FAILURE);
}

/// @brief Wrapper for sigaction(). Installs the function @a handler as the signal handler
///        for signal @a signum. Does not return on error.
/// @param signum signal number to catch
/// @param handler signal handler to invoke
void Signal(int signum, void (*handler)(int))
{
  struct sigaction action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask); // block sigs of type being handled
  action.sa_flags = SA_RESTART; // restart syscalls if possible

  if (sigaction(signum, &action, NULL) < 0) unix_error("Sigaction");
}

/// @brief SIGQUIT handler. Terminates the shell.
__attribute__((noreturn))
void sigquit_handler(int sig)
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(EXIT_SUCCESS);
}

/// @brief strip newlines (\n) from a string. Warning: modifies the string itself!
///        Inside the string, newlines are replaced with a space, at the end 
///        of the string, the newline is deleted.
///
/// @param str string
/// @reval char* stripped string
char* stripnewline(char *str)
{
  char *p = str;
  while (*p != '\0') {
    if (*p == '\n') *p = *(p+1) == '\0' ? '\0' : ' ';
    p++;
  }

  return str;
}
