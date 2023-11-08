#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>

#ifndef MAX_WORDS
#define MAX_WORDS 512
#endif

char *backgroundPid = "";
char *foregroundPid = "0";
int nwords = 0;

char *words[MAX_WORDS];
size_t wordsplit(char const *line);
char * expand(char const *word);

int main(int argc, char *argv[])
{
  // open input to std in if no args. If arg given, open that file, if 2+ args error
  FILE *input = stdin;
  char *input_fn = "(stdin)";
  if (argc == 2) {
    input_fn = "foo.txt";
    input = fopen(input_fn, "re");
    if (!input) err(1, "%s", input_fn);
  } else if (argc > 2) {
    errx(1, "Too many arguments\n");
  }

  char *line = NULL;
  size_t n = 0;
  for (;;) {
prompt:
    // clear prev array
    memset(words, 0, sizeof(words));
    
    /* TODO: Manage background processes LATER....*/

    
    /* TODO: prompt */
    if (input == stdin) {
      char *PS1 = getenv("PS1");
      if (PS1 == NULL){
        PS1 = "";
      }
      fprintf(stderr, "%s", PS1);
    }

    // read line
    ssize_t line_len = getline(&line, &n, input);
    if (line_len < 0) err(1, "%s", input_fn);  // end of file casues an error***

    size_t nwords = wordsplit(line);
    for (size_t i = 0; i < nwords; ++i) {
      //fprintf(stderr, "Word %zu: %s\n", i, words[i]);
      char *exp_word = expand(words[i]);
      free(words[i]);
      words[i] = exp_word;
      //fprintf(stderr, "Expanded Word %zu: %s\n", i, words[i]);
    }

    // which command? cd, exit, or other
    // case 1: exit
    if (strcmp(words[0], "exit") == 0) {
      if (nwords > 2){
        // only one exit arg allowed
        fprintf(stderr, "Too many arguments\n");
        goto prompt;
      } else if (nwords == 2){
        // exit command is given
          if (isdigit(*words[1])){
            int code = words[1];
            exit((int) code);  // int to str
          } else {
            // exit arg is not an int
            fprintf(stderr, "Exit command is not an int\n");
            goto prompt;
        }} else {
          // not exit arg give. exit default
          exit((int) *foregroundPid);
        }
    // case 2: cd
    } else if (strcmp(words[0], "cd") == 0) {
        if (nwords == 2) {
          chdir(words[1]); // NOT WORKING????
          goto prompt;
        } else if (nwords == 1) {
          chdir(getenv("HOME"));
        } else {
          fprintf(stderr, "Too many arguments\n");
          goto prompt;
        }
    // case 3: other command
    } else {
      // Execute the command
      fflush(stdout);
      pid_t spawnPid = -5;
      int childExitStatus = -5;

      spawnPid = fork();
      switch (spawnPid) {
        case -1: {
          perror("Fork error\n"); 
          exit(1); 
          break;
        }
        case 0: {
          // Child process
          execvp(words[0], words);
          exit(0);
          break;
        }
        default: {
          // Parent process
          // wait for child to process command
          waitpid(spawnPid, &childExitStatus, 0);
        }
      }
    }
  }
} 

char *words[MAX_WORDS] = {0};


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
    if (c == '!') build_str(backgroundPid, NULL);
    else if (c == '$') build_str(pid, NULL);
    else if (c == '?') build_str(foregroundPid, NULL);
    else if (c == '{') {
      build_str("<Parameter: ", NULL);
      build_str(start + 2, end - 1);
      build_str(">", NULL);
    }
    pos = end;
    c = param_scan(pos, &start, &end);
    build_str(pos, start);
  }
  return build_str(start, NULL);
}