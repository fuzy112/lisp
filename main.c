#define _GNU_SOURCE 1
#include "lisp.h"
#include <editline/readline.h>
#include <errno.h>
#include <fcntl.h>
#include <gc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
repl ()
{
  lisp_runtime_t *rt = lisp_runtime_new ();
  lisp_env_t *env = lisp_new_top_level_env (rt);
  lisp_reader_t *reader = lisp_reader_new (env, stdin);

  lisp_value_t exp;
  lisp_value_t val;

  while (!feof (stdin))
    {
      fprintf (stderr, ">>> ");

      exp = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (exp))
        {
          lisp_print_exception (env);
          break;
        }
      if (LISP_IS_EOF (exp))
        break;
      // lisp_print_value (env, exp);
      val = lisp_eval (env, exp);
      if (LISP_IS_EXCEPTION (val))
        lisp_print_exception (env);
      else
        lisp_print_value (env, val);
      fflush (stdout);
    }

  return 0;
}

static int
interpreter (FILE *filep, char **args)
{
  GC_INIT ();

  lisp_runtime_t *rt = lisp_runtime_new ();
  lisp_env_t *env = lisp_new_top_level_env (rt);
  lisp_reader_t *reader = lisp_reader_new (env, filep);

  (void)args;

  while (!feof (filep))
    {
      lisp_value_t val;
      lisp_value_t expr = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (expr))
        goto fail;
      if (LISP_IS_EOF (expr))
        break;

      val = lisp_eval (env, expr);
      if (LISP_IS_EXCEPTION (val))
        goto fail;
    }

  return 0;

fail:
  lisp_print_exception (env);

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
    int res;
    FILE *filep = fopen (argv[1], "r");
    if (!filep)
      {
        perror (argv[1]);
        return 1;
      }

    res = interpreter (filep, argv);
    fclose (filep);

    return res;
  }
}
