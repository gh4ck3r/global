#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

#include "parser.h"
#include "locatestring.h"
#include "langmap.h"

#define CUSTOM_PARSER_CONF_PREFIX  "custom_parser_"

typedef int bool;

static FILE *ip = NULL;

#ifdef __GNUC__
static void terminate_process(void) __attribute__((destructor));
#endif

static char *argv[] = {
  NULL,
  NULL,
  NULL
};
static pid_t pid = 0;

static void start_custom_parser(const char *parser_cmd, const struct parser_param *param)
{
  int ipipe[2];

  if (pipe(ipipe) < 0) param->die("cannot create pipe.");

  pid = fork();

  if (pid == 0) {
    /* child process */
    close(ipipe[0]);
    if (dup2(ipipe[1], STDOUT_FILENO) < 0) {
      param->die("dup2 failed.");
    }
    close(ipipe[1]);

    argv[0] = (char*)parser_cmd;
    argv[1] = (char*)param->file;
    execvp(parser_cmd, argv);
    param->die("Failed to exec %s %s", parser_cmd, param->file);
  }

  /* parent process */
  if (pid < 0) {
    param->die("fork failed.");
  }

  close(ipipe[1]);
  ip = fdopen(ipipe[0], "r");
  if (ip == NULL) {
    param->die("fdopen failed.");
  }
}

static void
terminate_process(void)
{
  if (ip != NULL) fclose(ip);
  if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR);
}

static const char* get_parser(const struct parser_param *param)
{
	const char *suffix = rindex(param->file, '.');
	if (!suffix) {
    param->die("Can't find suffix from %s", param->file);
		return NULL;
  }

  const char *langmap = param->getconf("langmap");
  if (!langmap) {
    param->die("Can't find langmap from configuration");
    return NULL;
  }

  char *lang = strstr(langmap, suffix);
  if (!lang) {
    param->die("Failed to find suffix %s from langmap in configuration", suffix);
    return NULL;
  }
  while (langmap < --lang && *lang != ':');
  if (langmap >= lang) {
    param->die("Failed to get language with suffix %s", suffix);
    return NULL;
  }

  size_t lang_len = 0;
  while (langmap < --lang && *lang != ',') ++lang_len;
  if (langmap == lang) ++lang_len; else ++lang;

  static char custom_parser_conf[BUFSIZ] = CUSTOM_PARSER_CONF_PREFIX;
  char *language = custom_parser_conf + sizeof(CUSTOM_PARSER_CONF_PREFIX) - 1;

  strncpy(language, lang, lang_len);
  if (param->flags & PARSER_DEBUG) {
    param->message("\tLanguage for custom parser: |%s|", language);
  }

  char *parser_cmd = param->getconf(custom_parser_conf);
  if (!parser_cmd) {
    param->die("Failed to find custom parser configuration %s", custom_parser_conf);
    return NULL;
  }

  return parser_cmd;
}

void
parser(const struct parser_param *param)
{
  const char delim[] = ",";
  char *src_line = NULL;
  size_t buf_alloc_siz;

  const bool DEBUG   = param->flags & PARSER_DEBUG;
  const bool VERBOSE = param->flags & PARSER_VERBOSE;
  const bool EXPLAIN = param->flags & PARSER_EXPLAIN;

  const char *parser_cmd = get_parser(param);
  if (!parser_cmd) return;

  if (EXPLAIN) {
    param->message("\tcustom_parser : |%s|", parser_cmd);
  }

  start_custom_parser(parser_cmd, param);

  size_t len;
  while ((len = getline(&src_line, &buf_alloc_siz, ip)) != -1) {
    int line, column;
    char *type, *symbol, *path, *rest, *saveptr;

    if (DEBUG) {
      // Remove newline temporarily
      char c = src_line[len-1];
      src_line[len-1] = '\0';
      param->message("line : %s", src_line);
      src_line[len-1] = c;
    }

    type = strtok_r(src_line, delim, &saveptr);
    if (*type != 'D' && *type != 'R') continue;
    symbol = strtok_r(NULL, delim, &saveptr);
    if (!symbol || (*type == 'D' && param->isnotfunction(symbol))) continue;
    path = strtok_r(NULL, delim, &saveptr);
    if (!path) continue;
    line = atoi(strtok_r(NULL, ":", &saveptr));
    if (!line) continue;
    column = atoi(strtok_r(NULL, delim, &saveptr));
    if (!column) continue;
    rest = strtok_r(NULL, "\n", &saveptr);
    if (!rest) rest = "";

    if (DEBUG) {
      param->message("type : %c, symbol : %s, path : %s, line : %u, col: %u, rest : %s", *type, symbol, path, line, column, rest);
    }

    param->put(*type == 'D' ? PARSER_DEF : PARSER_REF_SYM,
        symbol,
        line,
        path,
        rest,
        param->arg);
  }
  if (src_line != NULL) free(src_line);
}
