/*
 * shell.c
 *
 * Written by Drs. William Kreahling and Andy Dalton
 * 
 * @author GJ
 * @version 03/17/2017
 *
 * An implementation of a simple UNIX shell.  This program supports:
 *
 *     - Running processes
 *     - Redirecting standard output (>)
 *     - Redirecting standard input (<)
 *     - Appending standard output to a file (>>)
 *     - Redirecting both standard output and standard input (&>)
 *     - Creating process pipelines (p1 | p2 | ...)
 *     - Interrupting a running process (i.e., Ctrl-C)
 *     - A built-in version of the 'ls' command
 *     - A built-in version of the 'rm' command
 *
 * Among the many things it does _NOT_ support are:
 *
 *     - PATH searching -- you must supply the absolute path to all programs
 *       (e.g., /bin/ls instead of just ls)
 *     - Environment variables
 *     - Appending standard error to a file (2>>)
 *     - Appending both standard output and standard input (2&>)
 *     - Backgrounding processes (p1&)
 *     - Unconditionally chaining processes (p1;p2)
 *     - Conditionally chaining processes (p1 && p2 or p1 || p2)
 *     - Piping/IO redirection for built-in commands
 *
 * Keep in mind that this program was written to be easily understood/modified
 * for educational purposes.  The author makes no claim that this is the
 * "best" way to solve this problem.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include "shellParser.h"

/* Macros to test whether a process ID is a parent's or a child's. */
#define PARENT_PID(pid) ((pid) > 0)
#define CHILD_PID(pid)  ((pid) == 0)

/* Function prototypes */
static char** promptAndRead(void);
static pid_t  forkWrapper(void);
static void   pipeWrapper(int fds[]);
//static int    dupWrapper(int fd);
static bool   isSpecial(char* token);
static void   signalHandler(int signo);

static void   parseArgs(char** args, char** line, int* lineIndex);
static void   continueProcessingLine(char** line, int* lineIndex, char** args);
static void   doAppendRedirection(char* filename);
static void   doStdoutRedirection(char* filename);
static void   doStderrRedirection(char* filename);
static void   doStdoutStderrRedirection(char* filename);
static void   doStdinRedirection(char* filename);
static void   doPipe(char** p1Args, char** line, int* lineIndex);
static void   doLs(char** args);
static void   doRm(char** args);
static void lsHelper(struct dirent *dptr, DIR *dp);

/*
 * A global variable representing the process ID of this shell's child.  When the value of this
 * variable is 0, there are no running children.
 */
static pid_t childPid = 0;

/*
 * Entry point of the application
 */
int main(void) {
    char** line;

    signal(SIGINT, signalHandler);

    /* Read a line of input from the keyboard */
    line = promptAndRead();

    /* While the line was blank or the user didn't type exit */
    while (line[0] == NULL || (strcmp(line[0], "exit") != 0)) {
        int lineIndex = 0; /* An index into the line array */

        /* Ignore blank lines */
        if (line[lineIndex] != NULL) {
            int   status;
            char* args[MAX_ARGS]; /* A processes arguments */

            /* Dig out the arguments for a single process */
            parseArgs(args, line, &lineIndex);

            if (strcmp(args[0], "ls") == 0) {
                doLs(args);
            } else if (strcmp(args[0], "rm") == 0) {
                doRm(args);
            } else {
                /* Fork off a child process */
                childPid = forkWrapper();

                if (CHILD_PID(childPid)) {
                    /* The child shell continues to process the command line */
                    continueProcessingLine(line, &lineIndex, args);
                } else {

                    waitpid(childPid, &status, 0);
                    printf("\nChild %d exited with status %d\n", childPid,
                            status);
                }
            }
        }

        /* Read the next line of input from the keyboard */
        line = promptAndRead();
    }

    /* User must have typed "exit", time to gracefully exit. */
    return 0;
}

/**
 * signalHandler
 *
 * This function is used to handle the the 'Ctrl + c' signal
 * It stops the SIG_INT signal from terminating the program
 *
 * @param signo is the signal to be taken care of
 */
void signalHandler(int signo) {

    if(childPid > 0) {
        kill(childPid, signo);
    }
    else if(childPid < 0) {
        perror("fork");
    }
}


/*
 * continueProcessingLine
 *
 * This function continues to process a line read in from the user.  This processing can include
 * append redirection, stderr redirection, etc.  Note that this function operates recursively; it
 * breaks off a piece associated with a process until it gets to something "special", decides what
 * to do with that "special" thing, and then calls itself to handle the rest.  The base case of
 * the recursion is when the end of the 'line' array is reached (i.e., when line[*lineIndex] ==
 * NULL).
 *
 * line      - An array of pointers to string corresponding to ALL of the tokens entered on the
 * command line.
 * lineIndex - A pointer to the index of the next token to be processed
 * args      - A NULL terminated array of string corresponding to the arguments for a process
 * (i.e., stuff that was already parsed off of line).
 */
static void continueProcessingLine(char** line, int* lineIndex, char** args) {
    if (line[*lineIndex] == NULL) { /* Base case -- nothing left in line */
        
        if(execvp(line[0], args) < 0){
            perror("EXEC failed");
            _exit(1);
        }		 
    } else if (strcmp(line[*lineIndex], ">>") == 0) {
        (*lineIndex)++;
        doAppendRedirection(line[*lineIndex]);

        (*lineIndex)++;
        continueProcessingLine(line, lineIndex, args);

    } else if (strcmp(line[*lineIndex], "2>") == 0) {
        (*lineIndex)++;
        doStderrRedirection(line[*lineIndex]);

        (*lineIndex)++;
        continueProcessingLine(line, lineIndex, args);

    } else if (strcmp(line[*lineIndex], "&>") == 0) {
        (*lineIndex)++;
        doStdoutStderrRedirection(line[*lineIndex]);

        (*lineIndex)++;
        continueProcessingLine(line, lineIndex, args);

    } else if (strcmp(line[*lineIndex], ">") == 0) {
        (*lineIndex)++;
        doStdoutRedirection(line[*lineIndex]);

        (*lineIndex)++;
        continueProcessingLine(line, lineIndex, args);

    } else if (strcmp(line[*lineIndex], "<") == 0) {
        (*lineIndex)++;
        doStdinRedirection(line[*lineIndex]);

        (*lineIndex)++;
        continueProcessingLine(line, lineIndex, args);

    } else if (strcmp(line[*lineIndex], "|") == 0) {
        (*lineIndex)++;
        doPipe(args, line, lineIndex);
        /* doPipe() calls continueProcessingLine() only in some cases */
    }
}

/*
 * doPipe
 *
 * Implements a pipe between two processes.
 *
 * p1Args    - The arguments for the left-hand-side command.
 * line      - An array of pointers to string corresponding to ALL of the
 *             tokens entered on the command line.
 * lineIndex - A pointer to the index of the next token to be processed.
 *             This index should point to one element beyond the pipe
 *             symbol.
 */
static void doPipe(char** p1Args, char** line, int* lineIndex) {
    int   pipefd[2]; /* Array of integers to hold 2 file descriptors. */
    pid_t pid;       /* PID of a child process */

    pipeWrapper(pipefd);

    /* Fork the current process */
    pid = forkWrapper();

    if (CHILD_PID(pid)) { /* Child -- will execute left-hand-side process */
       
        dup2(pipefd[1], 1);
        close(pipefd[0]);
      
        execvp(line[0], p1Args);

    } else {  /* Parent will keep going */
        char* args[MAX_ARGS];
 
        dup2(pipefd[0], 0);
        close(pipefd[1]);
        execvp(line[2], p1Args);

        /* Read the args for the next process in the pipeline */
        parseArgs(args, line, lineIndex);

        /* And keep going... */
        continueProcessingLine(line, lineIndex, args);

    }
}

/*
 * doAppendRedirection
 *
 * Redirects the standard output of this process to append to the
 * file with the specified name.
 *
 * filename - the name of the file to which to append our output
 */
static void doAppendRedirection(char* filename) {
   
    int stdOutFile = open(filename, O_RDWR | O_APPEND, S_IRWXU);
    if(stdOutFile == -1){
        perror("Error opening file");
        exit(1);
    }

    if(dup2(stdOutFile, 1) == -1){ // Redirecting stdout to file
        perror("can't redirect standard out for append");
        exit(1);
    }

    fflush(stdout);
    close(stdOutFile);

    return;

}

/*
 * doStdoutRedirection
 *
 * Redirects the standard output of this process to overwrite the
 * file with the specified name.
 *
 * filename - the name of the file which to overwrite
 */
static void doStdoutRedirection(char* filename) {
  
    int stdOutFile = open(filename, O_TRUNC | O_WRONLY | O_CREAT, S_IRWXU);
    if(stdOutFile == -1){
        perror("Error opening file");
        exit(1);
    }

    if(dup2(stdOutFile, 1) == -1){ // Redirecting stdout to file
        perror("can't redirect standard out");
        exit(1);
    }

    fflush(stdout);
    close(stdOutFile);

    return;
}

/*
 * doStderrRedirection
 *
 * Redirects the standard error of this process to overwrite the
 * file with the specified name.
 *
 * filename - the name of the file which to overwrite
 */
static void doStderrRedirection(char* filename) {
 
    int stdErrFile = open(filename, O_TRUNC | O_WRONLY | O_CREAT, S_IRWXU);
    if(stdErrFile == -1){
        perror("Error opening file");
        exit(1);
    }
    if(dup2(stdErrFile, 2) == -1){ // Redirecting stderr to file
        perror("can't redirect standard error");
        exit(1);
    }

    fflush(stderr);
    close(stdErrFile);

    return;
}

/*
 * doStdoutStderrRedirection
 *
 * Redirects the standard output AND standard error of this process to
 * overwrite the file with the specified name.
 *
 * filename - the name of the file which to overwrite
 */
static void doStdoutStderrRedirection(char* filename) {
   
    int stdOutErrFile = open(filename, O_TRUNC | O_WRONLY | O_CREAT, S_IRWXU);
    if(stdOutErrFile == -1){
        perror("Error opening file");
        exit(1);
    }

    if(dup2(stdOutErrFile, 1) == -1){ // Redirecting stdout to file
        perror("can't redirect standard out");
        exit(1);
    }
    if(dup2(stdOutErrFile, 2) == -1){ // Redirecting stderr to file
        perror("can't redirect standard error");
        exit(1);
    }

    fflush(stdout);
    fflush(stderr);
    close(stdOutErrFile);

    return;
}

/*
 * doStdinRedirection
 *
 * Redirects the standard input to this process from the file with the
 * specified name.
 *
 * filename - the name of the file from which to read as standard input.
 */
static void doStdinRedirection(char* filename) {
  
    int stdInFile = open(filename, O_RDONLY);
    if(stdInFile == -1){
        perror("Error opening file");
        exit(1);
    }

    if(dup2(stdInFile, 0) == -1){ // Redirecting stdin to file
        perror("can't redirect standard in");
        exit(1);
    }

    fflush(stdin);
    close(stdInFile);

    return;
}

/*
 * parseArgs
 *
 * Parse the command line, stopping at a special symbol of the end of the line.
 *
 * args      - The array to populate with arguments from line
 * line      - An array of pointers to string corresponding to ALL of the
 *             tokens entered on the command line.
 * lineIndex - A pointer to the index of the next token to be processed.
 *             This index should point to one element beyond the pipe
 *             symbol.
 */
static void parseArgs(char** args, char** line, int* lineIndex) {
    int i;

    for (i = 0;    line[*lineIndex] != NULL
            && !isSpecial(line[*lineIndex]); ++(*lineIndex), ++i) {
        args[i] = line[*lineIndex];
    }
    args[i] = NULL;
}

/*
 * promptAndRead
 *
 * A simple wrapper that displays a prompt and reads a line of input
 * from the user.
 *
 * Returns a pointer to an array of strings where each element in
 * the array corresponds to a token from the input line.
 */
static char** promptAndRead(void) {
    printf("(%d) $ ", getpid());
    return getArgList();
}

/*
 * forkWrapper
 *
 * A simple wrapper around the 'fork' system call that attempts to
 * invoke fork and on failure, prints an appropriate message and
 * terminates the process.
 */
static pid_t forkWrapper(void) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        _exit(2);
    }

    return pid;
}

/*
 * pipeWrapper
 *
 * A simple wrapper around the 'pipe' system call that attempts to invoke
 * pipe and on failure, prints an appropriate message and terminates the
 * process.
 */
static void pipeWrapper(int pipefds[]) {
   
    int returnValue = pipe(pipefds);
    if(returnValue < 0) {
        perror("Pipe_Error!");
        _exit(0);
    }
}

/*
 * dupWrapper
 *
 * A simple wrapper around the 'dup' system call that attempts to invoke
 * pipe and on failure, prints an appropriate message and terminates the
 * process.
 */

// We have commented out this function as we dont use it

/**static int dupWrapper(int oldfd) {
    int newfd = -1;

    if ((newfd = dup(oldfd)) < 0) {
        perror("dup");
        _exit(3);
    }

    return newfd;
}*/

/*
 * isSpecial
 *
 * Returns true if the specified token is "special" (i.e., is an
 * operator like >, >>, |, <); false otherwise.
 */
static bool isSpecial(char* token) {
    return    (strlen(token) == 1 && strchr("<>|", token[0]) != NULL)
        || (strlen(token) == 2 && strchr(">",   token[1]) != NULL);
}

/**
 * doLs
 *
 * Implements a built-in version of the 'ls' command.
 *
 * args - An array of strings corresponding to the command and its arguments.
 *        If args[1] is NULL, the current directory (./) is assumed; otherwise
 *        it specifies the directory to list.
 */
static void doLs(char** args) {

    DIR *dp = NULL;
    struct dirent *dptr = NULL;

    if(args[1] == NULL) {

        dp = opendir("./");
        lsHelper(dptr, dp);
        closedir(dp);
    }

    else {

        int i = 1;
        while (args[i] != NULL) {
            dp = opendir(args[i]);

            if(dp) {

                // If there are more than 1 directory to display
                //  print the directory name 
                if(args[2] != NULL) {
                    printf("%s --->", args[i]);
                }
                lsHelper(dptr, dp);
                closedir(dp);
            }
            else {
                printf("\nError ! Cannot open '%s' : Directory not found\n\n",
                        args[i]);
            }
            i++;
        }
    }
}


/**
 * lsHelper function
 * This is a helper function for doLs to print
 * directory names on the screen
 *
 * @param dptr is the directory pointer
 * @param dp is the directory
 */
static void lsHelper(struct dirent *dptr, DIR *dp) {

    printf("\n");

    int count;

    for(count = 0; (dptr = readdir(dp)) != NULL; count++) {

        // Not to display hidden files
        if(dptr->d_name[0] != '.') {
            printf("%s\n", dptr->d_name);
        }
    }
    printf("\n");
}


/**
 * doRm
 *
 * Implements a built-in version of the 'rm' command.
 *
 * args - An array of strings corresponding to the command and its arguments.
 *        args[0] is "rm", additional arguments are in args[1] ... n.
 *        args[x] = NULL indicates the end of the argument list.
 */
static void doRm(char** args) {
    
    if(args[1] == NULL) {

        printf("\nError! Need file name\n\n");

    }
    else {

        int i = 1;

        while (args[i] != NULL) {

            int fd = open(args[i], O_RDONLY);
            if (fd < 0) {
                printf("\nError! Cannot remove '%s' : File not found\n\n",
                        args[i]);
            }
            else { 
                unlink(args[i]);
                close(fd);
            }
            i++;
        }
    }
}
