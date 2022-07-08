#include "lisp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
repl ()
{
  lisp_runtime_t *rt = lisp_runtime_new ();
  lisp_context_t *ctx = lisp_new_global_context (rt);
  lisp_reader_t *reader = lisp_reader_new (ctx, stdin);

  lisp_value_t exp;
  lisp_value_t val;

  fprintf (stderr, ">>> ");
  while (!feof (stdin))
    {
      exp = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (exp)) {
        lisp_print_exception (ctx);
        break;
      }
      lisp_print_value (ctx, exp);
      val = lisp_eval (ctx, exp);
      if (LISP_IS_EXCEPTION (val))
        lisp_print_exception (ctx);
      else
        lisp_print_value (ctx, val);
      lisp_free_value (ctx, val);
      lisp_free_value (ctx, exp);

      lisp_gc_rt (rt);

      fprintf (stderr, ">>> ");
    }

  lisp_reader_free (reader);
  lisp_context_unref (ctx);
  lisp_gc_rt (rt);
  lisp_runtime_free (rt);

  return 0;
}

static int
interpreter (FILE *filep, char **args)
{
  lisp_runtime_t *rt = lisp_runtime_new ();
  lisp_context_t *ctx = lisp_new_global_context (rt);
  lisp_reader_t *reader = lisp_reader_new (ctx, filep);

  (void)args;

  while (!feof (filep))
    {
      lisp_value_t val;
      lisp_value_t expr = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (expr))
        goto fail;

      val = lisp_eval (ctx, expr);
      lisp_free_value (ctx, expr);
      if (LISP_IS_EXCEPTION (val))
        goto fail;

      lisp_free_value (ctx, val);
    }

  lisp_reader_free (reader);
  lisp_context_unref (ctx);
  lisp_gc_rt (rt);
  lisp_runtime_free (rt);

  return 0;

fail:
  lisp_print_exception (ctx);

  lisp_reader_free (reader);

  lisp_context_unref (ctx);
  lisp_gc_rt (rt);
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
    int res;
    FILE *filep = fopen (argv[1], "r");
    if (!filep)
      return 1;

    res = interpreter (filep, argv);
    fclose (filep);

    return res;
  }
}
