#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

#ifndef MAX_WORDS
#define MAX_WORDS 512
#endif

char *background_pid = "";  // $!
char *foreground_pid = "0";  // $?
int background_flag = 0;
int background_child = 0;
int childExitStatus = 0;
int expanded_param_flag = 0;
int nwords = 0;
char * input_file = NULL;
char * output_file = NULL;
int append_flag = 0;
int sourceFD = 0;
int targetFD = 0;

char *words[MAX_WORDS];
size_t wordsplit(char const *line);
char * expand(char const *word);
void condenseArray(int index, char * words[MAX_WORDS], int length);


void handle_SIGINT(int signo){
  // no action
}


/* MAIN FUNCTION */
int main(int argc, char *argv[])
{
  // open input to std in if no args. If arg given, open that file, if 2+ args error
  FILE *input = stdin;
  char *input_fn = "(stdin)";
  if (argc == 2) {
    input_fn = argv[1];
    input = fopen(input_fn, "re");
    if (!input) err(1, "%s", input_fn);
  } else if (argc > 2) {
    errx(1, "Too many arguments\n");
  }

  // Set up signals
  struct sigaction SIGINT_action ={0}, SIGTSTP_action = {0}, ignore_action = {0};
  // no signal handling unless interactive mode (stdin)
  ignore_action.sa_handler = SIG_IGN;
  // Ignore SIGTSTPif in interactive mode
  if (input == stdin) {
    // Ignore SIGTSTP
    sigaction(SIGTSTP, &ignore_action, NULL);
  }

  // Initialize line and n 
  char *line = NULL;
  size_t n = 0;

  // MAIN LOOP
  for (;;) {
start:
    // default settings
    expanded_param_flag = 0;
    input_file = NULL;
    output_file = NULL;
    int append_flag = 0;
    // clear prev array
    memset(words, 0, sizeof(words));
    
    /* 
      Manage background processes 
    */
    // check if process has completed. WNOHANG doesn't block the parent process
    while ((background_child = waitpid(0, &childExitStatus, WUNTRACED | WNOHANG)) > 0) {
      // if greater than 0 a child process has completed. 
      if (WIFEXITED(childExitStatus)) {
        fprintf(stderr, "Child process %jd done. Exit status %d.\n", (intmax_t) background_child, WEXITSTATUS(childExitStatus));
      }
      if (WIFSTOPPED(childExitStatus)) {
        kill(background_child, SIGCONT);
        fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) background_child);
      }
      if (WIFSIGNALED(childExitStatus)) {
        fprintf(stderr, "Child process %jd done. Signaled %d.\n", (intmax_t) background_child, WTERMSIG(childExitStatus));
      }
    }
    
    /* Interactive mode Prompt and SIGINT settings*/
    if (input == stdin) {
      // set PS!
      char *PS1 = getenv("PS1");
      if (PS1 == NULL){
        PS1 = "";
      }
      fprintf(stderr, "%s", PS1);

      // set sig handler to when reading line of input
      SIGINT_action.sa_handler = handle_SIGINT;
      // Block all catchable signals while handle_SIGINT is running
      sigfillset(&SIGINT_action.sa_mask);
      // No flags set
      SIGINT_action.sa_flags = 0;
      sigaction(SIGINT, &SIGINT_action, NULL);
    }

    // read line
    ssize_t line_len = getline(&line, &n, input);

    // if line blocked by a signal
      //errno=EINTR
      // CLEARERR()
      // \n
      // goto start

    // exit if file is at end
    if ((line_len < 0) && (feof(input))) {
      exit(atoi(foreground_pid));
    }
    if (line_len < 0) err(1, "%s", input_fn);  

    // Ignore SIGINT again
    if (input == stdin) {
      sigaction(SIGINT, &ignore_action, NULL);
    }
    
    // split and expand words
    size_t nwords = wordsplit(line);
    for (size_t i = 0; i < nwords; ++i) {
      //fprintf(stderr, "Word %zu: %s\n", i, words[i]);
      char *exp_word = expand(words[i]);
      free(words[i]);
      words[i] = exp_word;
      if (expanded_param_flag == 1){
        // get env value
        char *new_word = getenv(words[i]);
        if (new_word == NULL){
          // set to empty str if null
          new_word = "";
        }
        // replace word with expanded param
        free(words[i]);
        words[i] = new_word;
      }
      //fprintf(stderr, "Expanded Word %zu: %s\n", i, words[i]);
    }

    // ignore all below code if no words were entered
    if (nwords == 0) {
      goto start;
    }

    /* 
      Check for special characters &, <, >, >>.
    */
    // check if it should be a background porocess by & at end of line
    if (strcmp(words[nwords-1], "&") == 0) {
      background_flag = 1;
      free(words[nwords-1]);
      words[nwords -1] = NULL;
      // decrement for word that was removed
      nwords--;
    } else {
      background_flag = 0;
    }

    // check for redirection <, >, or >> as a word
    // check for output redirection.
    int i = 0;
    while (i < nwords-1) {
      int j = i;
      if (strcmp(words[i], "<") == 0) {
        // read file
        input_file = words[i+1];
        // now move elements forwards so null values are at end
        condenseArray(j, words, nwords);
        nwords = nwords -2;
      } 
      else if (strcmp(words[i], ">") == 0) {
        // output to file
        output_file = words[i+1];
        // now move elements forwards so null values are at end
        condenseArray(j, words, nwords);
        nwords = nwords -2;
      }
      else if (strcmp(words[i], ">>") == 0) {
        // append to file
        output_file = words[i+1];
        // now move elements forwards so null values are at end
        condenseArray(j, words, nwords);
        nwords = nwords -2;
        append_flag = 1;
      }
      else{ i++; }
    }

    // ignore all below code if no words were entered
    if (nwords == 0) {
      goto start;
    }

    /* 
    HANDLE COMMANDS. exit, cd, or all others
    */
    // case 1: exit
    if (strcmp(words[0], "exit") == 0) {
      if (nwords > 2){
        // only one exit arg allowed
        fprintf(stderr, "Too many arguments\n");
        goto start;
      } else if (nwords == 2){
        // exit command is given
          if (isdigit(*words[1])) {
            exit(atoi(words[1]));  // atoi makes int to str
          } else {
            // exit arg is not an int
            fprintf(stderr, "Exit command is not an int\n");
            goto start;
        }} else {
          // no exit arg given. exit default
          exit(atoi(foreground_pid));
        }
    // case 2: cd
    } else if (strcmp(words[0], "cd") == 0) {
        if (nwords == 2) {
          chdir(words[1]);
          goto start;
        } else if (nwords == 1) {
          chdir(getenv("HOME"));
        } else {
          fprintf(stderr, "Too many arguments\n");
          goto start;
        }
    // case 3: other command
    } else {
      fflush(stdout);
      pid_t spawn_pid = -5;
      int childExitStatus = -5;

      spawn_pid = fork();
      switch (spawn_pid) {
        case -1: {
          perror("Fork error\n"); 
          exit(1); 
          break;
        }
        case 0: {
          /*
            CHILD PROCESS
          */

          // Reset signals for child
          ///
          /// oldact in SIGACTION(2).
          //

          // check for input file
          if (input_file != NULL) {
            sourceFD = open(input_file, O_RDONLY);
            if (sourceFD == -1) {
              perror("source open()");
              exit(1);
            }
            // redirect stdin to source file
            int result = dup2(sourceFD, 0);
            if (result == -1) {
              perror("source dup2()");
              exit(2);
            }
          }

          // check for output file
          if (output_file != NULL) {
            if (append_flag == 0) {
              targetFD = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0777);
            } else {
              targetFD = open(output_file, O_WRONLY | O_CREAT | O_APPEND, 0777);
            }
            if (targetFD == -1) {
              perror("target open()");
              exit(1);
            }
            // redirect stdout to target file
            int result = dup2(targetFD, 1);
            if (result == -1) {
              perror("target dup2()");
              exit(2);
            }
          }

          // execute command
          execvp(words[0], words);
          // close files and exit
          if (input_file != NULL) {
            close(sourceFD);
          }
          if (output_file != NULL) {
            close(targetFD);
          }
          exit(0);
          break;
        }

        default: {
          // Parent process
          // wait for child to process command if foreground
          if (background_flag == 0) {
            spawn_pid = waitpid(spawn_pid, &childExitStatus, 0);
            foreground_pid = malloc(sizeof(int) * 8);
            if (WIFEXITED(childExitStatus)) {
              sprintf(foreground_pid, "%d", WEXITSTATUS(childExitStatus));
            } 
            else if (WIFSIGNALED(childExitStatus)) {
              sprintf(foreground_pid, "%d", 128 + WSTOPSIG(childExitStatus));
            }
          } else {
            // background
            background_pid = malloc(sizeof(int) * 8);
            sprintf(background_pid, "%d", WEXITSTATUS(childExitStatus));
          }
        }
      }
    }
  }
} 

char *words[MAX_WORDS] = {0}; // is this needed??? from starter code


/* Splits a string into words delimited by whitespace. Recognizes
 * comments as '#' at the beginning of a word, and backslash escapes.
 *
 * Returns number of words parsed, and updates the words[] array
 * with pointers to the words, each as an allocated string.
 */
size_t wordsplit(char const *line) {
  size_t wlen = 0;
  size_t wind = 0;

  char const *c = line;
  for (;*c && isspace(*c); ++c); /* discard leading space */

  for (; *c;) {
    if (wind == MAX_WORDS) break;
    /* read a word */
    if (*c == '#') break;
    for (;*c && !isspace(*c); ++c) {
      if (*c == '\\') ++c;
      void *tmp = realloc(words[wind], sizeof **words * (wlen + 2));
      if (!tmp) err(1, "realloc");
      words[wind] = tmp;
      words[wind][wlen++] = *c; 
      words[wind][wlen] = '\0';
    }
    ++wind;
    wlen = 0;
    for (;*c && isspace(*c); ++c);
  }
  return wind;
}


/* Find next instance of a parameter within a word. Sets
 * start and end pointers to the start and end of the parameter
 * token.
 */
char
param_scan(char const *word, char const **start, char const **end)
{
  static char const *prev;
  if (!word) word = prev;
  
  char ret = 0;
  *start = 0;
  *end = 0;
  for (char const *s = word; *s && !ret; ++s) {
    s = strchr(s, '$');
    if (!s) break;
    switch (s[1]) {
    case '$':
    case '!':
    case '?':
      ret = s[1];
      *start = s;
      *end = s + 2;
      break;
    case '{':;
      char *e = strchr(s + 2, '}');
      if (e) {
        ret = s[1];
        *start = s;
        *end = e + 1;
      }
      break;
    }
  }
  prev = *end;
  return ret;
}

/* Simple string-builder function. Builds up a base
 * string by appending supplied strings/character ranges
 * to it.
 */
char *
build_str(char const *start, char const *end)
{
  static size_t base_len = 0;
  static char *base = 0;

  if (!start) {
    /* Reset; new base string, return old one */
    char *ret = base;
    base = NULL;
    base_len = 0;
    return ret;
  }
  /* Append [start, end) to base string 
   * If end is NULL, append whole start string to base string.
   * Returns a newly allocated string that the caller must free.
   */
  size_t n = end ? end - start : strlen(start);
  size_t newsize = sizeof *base *(base_len + n + 1);
  void *tmp = realloc(base, newsize);
  if (!tmp) err(1, "realloc");
  base = tmp;
  memcpy(base + base_len, start, n);
  base_len += n;
  base[base_len] = '\0';

  return base;
}

/* Expands all instances of $! $$ $? and ${param} in a string 
 * Returns a newly allocated string that the caller must free
 */
char *
expand(char const *word)
{
  char *pid = malloc(sizeof(int) *8);
  sprintf(pid, "%d", getpid());
  char const *pos = word;
  char const *start, *end;
  char c = param_scan(pos, &start, &end);
  build_str(NULL, NULL);
  build_str(pos, start);
  while (c) {
    if (c == '!') build_str(background_pid, NULL);
    else if (c == '$') build_str(pid, NULL);
    else if (c == '?') build_str(foreground_pid, NULL);
    else if (c == '{') {
      build_str("", NULL);
      build_str(start + 2, end - 1);
      build_str("", NULL);
      expanded_param_flag = 1;
    }
    pos = end;
    c = param_scan(pos, &start, &end);
    build_str(pos, start);
  }
  return build_str(start, NULL);
}

/*
Moves all array elements forward wehn deleting redirections
*/
void condenseArray(int index, char * words[MAX_WORDS], int length) {
  while (index < length-2) {
    words[index] = words[index+2];
    index++;
  }
  // update last 2 to be null for removed values. no need to update length since NULL values
  words[index] = NULL;
  words[index+1] = NULL;
}