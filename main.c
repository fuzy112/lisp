#include "lisp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
repl ()
{
  char *line = NULL;
  size_t n = 0;
  size_t len = 0;

  lisp_runtime_t *rt = lisp_runtime_new ();
  lisp_context_t *ctx = lisp_new_global_context (rt);

  lisp_value_t exp;
  lisp_value_t val;

  while ((len = getline (&line, &n, stdin)) != 0)
    {
      if (len < 2)
        break;
      exp = lisp_parse (ctx, (const char **)&line);
      val = lisp_eval (ctx, exp);
      if (LISP_IS_EXCEPTION (val))
        lisp_print_exception (ctx);
      else
        lisp_print_value (ctx, val);
      lisp_free_value (ctx, val);
      lisp_free_value (ctx, exp);
    }

  lisp_context_free (ctx);
  lisp_runtime_free (rt);

  return 0;
}

int
interpreter (const char *text, char **args)
{
  lisp_runtime_t *rt = lisp_runtime_new ();
  lisp_context_t *ctx = lisp_new_global_context (rt);

  (void)args;

  while (text)
    {
      lisp_value_t val;
      lisp_value_t expr = lisp_parse (ctx, &text);
      if (LISP_IS_EXCEPTION (expr))
        goto fail;

      val = lisp_eval (ctx, expr);
      lisp_free_value (ctx, expr);
      if (LISP_IS_EXCEPTION (val))
        goto fail;

      lisp_free_value (ctx, val);
    }

  lisp_context_free (ctx);
  lisp_runtime_free (rt);

  return 0;

fail:
  lisp_print_exception (ctx);

  lisp_context_free (ctx);
  lisp_runtime_free (rt);

  return -1;
}

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      return repl ();
    }

  {
    struct stat statb;
    char *text;
    int res;
    int fd;

    if (stat (argv[1], &statb))
      {
        perror ("stat");
        return 1;
      }

    text = malloc (statb.st_size + 1);
    if (!text)
      {
        perror ("malloc");
        return 1;
      }

    fd = open (argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      {
        perror ("open");
        return 1;
      }
    if (read (fd, text, statb.st_size) < 0)
      {
        perror ("read");
        return 1;
      }
    close (fd);
    text[statb.st_size] = '\0';

    res = interpreter (text, argv + 1);
    free (text);

    return res;
  }
}