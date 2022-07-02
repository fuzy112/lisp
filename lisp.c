#include "lisp.h"
#include "hashtable.h"
#include "list.h"
#include "string_buf.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LISP_VAR_TABLE_BITS 8

struct lisp_object;

struct lisp_object_operations
{
  void (*free) (lisp_context_t *ctx, struct lisp_object *);
  char *(*to_string) (lisp_context_t *ctx, struct lisp_object *);
};

struct lisp_object
{
  long ref_count;
  const struct lisp_object_operations *ops;
};

struct lisp_runtime
{
  long nr_blocks;
  lisp_value_t exception_list;
};

struct lisp_context
{
  char *name;
  struct lisp_context *parent;
  struct lisp_runtime *runtime;

  DECLARE_HASHTABLE (var_table, LISP_VAR_TABLE_BITS);
};

struct lisp_function;

typedef lisp_value_t (*lisp_native_invoke_t) (lisp_context_t *ctx,
                                              lisp_value_t args,
                                              struct lisp_function *func);

struct lisp_function
{
  struct lisp_object obj;

  lisp_value_t params;
  lisp_value_t body;
  lisp_native_invoke_t invoker;

  lisp_value_t name;

  lisp_context_t *ctx;
};

static void
lisp_function_free (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_function *func = (struct lisp_function *)obj;

  lisp_context_free (func->ctx);

  lisp_free_value (ctx, func->params);
  lisp_free_value (ctx, func->body);
  lisp_free_value (ctx, func->name);
  func->invoker = NULL;

  lisp_free (ctx, func);
}

static const struct lisp_object_operations lisp_function_operations = {
  .free = &lisp_function_free,
};

struct lisp_variable_store
{
  int ref_count;
  lisp_value_t value;
};

struct lisp_variable
{
  struct hlist_node node;

  lisp_value_t name;
  struct lisp_variable_store *store;
};

static lisp_value_t
lisp_nil (void)
{
  lisp_value_t val = LISP_NIL;
  return val;
}

lisp_runtime_t *
lisp_runtime_new (void)
{
  lisp_runtime_t *rt = malloc (sizeof (*rt));
  rt->nr_blocks = 0;
  rt->exception_list = lisp_nil ();
  return rt;
}

void
lisp_runtime_free (lisp_runtime_t *rt)
{
  assert (rt->nr_blocks == 0);
  free (rt);
}

struct lisp_variable_store *
lisp_variable_store_new (lisp_context_t *ctx)
{
  struct lisp_variable_store *store;

  store = lisp_malloc (ctx, sizeof (*store));
  store->value = lisp_nil ();
  store->ref_count = 1;

  return store;
}

struct lisp_variable_store *
lisp_variable_store_ref (lisp_context_t *ctx,
                         struct lisp_variable_store *store)
{
  (void)ctx;
  store->ref_count++;
  return store;
}

void
lisp_variable_store_unref (lisp_context_t *ctx,
                           struct lisp_variable_store *store)
{
  if (--store->ref_count > 0)
    return;

  lisp_free_value (ctx, store->value);
  lisp_free (ctx, store);
}

lisp_context_t *
lisp_context_new (lisp_runtime_t *rt, const char *name)
{
  lisp_context_t *ctx = lisp_malloc_rt (rt, sizeof (*ctx));
  if (!ctx)
    return NULL;
  hash_init (ctx->var_table);
  ctx->parent = NULL;
  ctx->runtime = rt;
  ctx->name = lisp_strdup_rt (rt, name);
  return ctx;
}

void
lisp_context_free (lisp_context_t *ctx)
{
  struct lisp_variable *var;
  struct hlist_node *tmp;
  size_t bkt;

  hash_for_each_safe (ctx->var_table, bkt, tmp, var, node)
  {
    lisp_free_value (ctx, var->name);
    lisp_variable_store_unref (ctx, var->store);
    hash_del(&var->node);
    lisp_free (ctx, var);
  }

  lisp_free_rt (ctx->runtime, ctx->name);

  lisp_free_rt (ctx->runtime, ctx);
}

lisp_value_t
lisp_exception (void)
{
  lisp_value_t val = LISP_EXCEPTION;
  return val;
}

lisp_runtime_t *
lisp_get_runtime (lisp_context_t *ctx)
{
  return ctx->runtime;
}

void *
lisp_malloc_rt (lisp_runtime_t *rt, size_t size)
{
  void *ptr = calloc (1, size);
  if (ptr)
    rt->nr_blocks += 1;
  return ptr;
}

void
lisp_free_rt (lisp_runtime_t *rt, void *p)
{
  if (p)
    {
      assert (rt->nr_blocks > 0);
      rt->nr_blocks -= 1;
    }

  free (p);
}

void *
lisp_malloc (lisp_context_t *ctx, size_t size)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  return lisp_malloc_rt (rt, size);
}

void
lisp_free (lisp_context_t *ctx, void *p)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  lisp_free_rt (rt, p);
}

char *
lisp_strdup_rt (lisp_runtime_t *rt, char const *s)
{
  char *r = strdup (s);
  rt->nr_blocks++;
  return r;
}

char *
lisp_strdup (lisp_context_t *ctx, const char *s)
{
  return lisp_strdup_rt (lisp_get_runtime (ctx), s);
}

lisp_value_t
lisp_dup_value (lisp_context_t *ctx, lisp_value_t val)
{
  (void)ctx;

  if (LISP_IS_OBJECT (val))
    {
      struct lisp_object *obj = val.ptr;

      if (!obj)
        return val;

      assert (obj != NULL);
      assert (obj->ref_count > 0);

      obj->ref_count++;
    }

  return val;
}

void
lisp_free_value (lisp_context_t *ctx, lisp_value_t val)
{
  if (LISP_IS_OBJECT (val))
    {
      struct lisp_object *obj = val.ptr;
      if (!obj)
        return;

      obj->ref_count--;
      if (obj->ref_count == 0)
        {
          if (obj->ops->free)
            {
              obj->ops->free (ctx, obj);
            }
        }
    }
}

static void *
lisp_get_object (lisp_context_t *ctx, lisp_value_ref_t val, unsigned tag)
{
  if (LISP_IS_EXCEPTION (val))
    return NULL;

  if (val.tag != tag)
    {
      lisp_throw_internal_error (ctx, "Type mismatch");
      return NULL;
    }

  if (val.ptr == NULL)
    {
      lisp_throw_internal_error (ctx, "Object pointer not set");
      return NULL;
    }

  return val.ptr;
}

struct lisp_cons
{
  struct lisp_object obj;

  lisp_value_t car;
  lisp_value_t cdr;
};

static void
lisp_cons_free (lisp_context_t *ctx, struct lisp_object *obj)
{
  lisp_value_t tmp;
  struct lisp_cons *cons = (struct lisp_cons *)obj;
  tmp = cons->cdr;
  lisp_free_value (ctx, cons->car);
  lisp_free (ctx, cons);
  lisp_free_value (ctx, tmp);
}

static lisp_value_t
lisp_car (lisp_context_t *ctx, lisp_value_ref_t val)
{
  struct lisp_cons *cons;
  cons = lisp_get_object (ctx, val, LISP_TAG_LIST);
  if (!cons)
    return lisp_exception ();
  return lisp_dup_value (ctx, cons->car);
}

static lisp_value_t
lisp_cdr (lisp_context_t *ctx, lisp_value_ref_t val)
{
  struct lisp_cons *cons;
  cons = lisp_get_object (ctx, val, LISP_TAG_LIST);
  if (!cons)
    return lisp_exception ();
  return lisp_dup_value (ctx, cons->cdr);
}

static lisp_value_t
lisp_cdr_take (lisp_context_t *ctx, lisp_value_t val)
{
  lisp_value_t r = lisp_cdr (ctx, val);
  lisp_free_value (ctx, val);
  return r;
}

static int
lisp_list_string_iter (lisp_context_t *ctx, struct string_buf *buf,
                       lisp_value_ref_t cdr, int start)
{
  int r;
  switch (cdr.tag)
    {
    case LISP_TAG_LIST:
      if (LISP_IS_NIL (cdr))
        return string_buf_append (buf, ")", 1);
      {
        lisp_value_t cdar = lisp_car (ctx, cdr);
        lisp_value_t cddr = lisp_cdr (ctx, cdr);
        char *s = lisp_value_to_string (ctx, cdar);
        lisp_free_value (ctx, cdar);
        if (!start)
          r = string_buf_append (buf, " ", 1);
        r = string_buf_append (buf, s, strlen (s));
        lisp_free (ctx, s);
        r = lisp_list_string_iter (ctx, buf, cddr, 0);
        lisp_free_value (ctx, cddr);
        return r;
      }

    default:
      {
        char *s;
        r = string_buf_append (buf, " . ", 3);
        if (r)
          return r;
        s = lisp_value_to_string (ctx, cdr);
        r = string_buf_append (buf, s, strlen (s));
        lisp_free (ctx, s);
        if (r)
          return r;
        return string_buf_append (buf, ")", 1);
      }
    }
}

static char *
lisp_cons_to_string (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_cons *cons = (struct lisp_cons *)obj;
  lisp_value_ref_t val = LISP_OBJECT (LISP_TAG_LIST, cons);
  struct string_buf buf;
  buf.capacity = 1;
  buf.length = 1;
  buf.s = lisp_strdup (ctx, "(");

  if (lisp_list_string_iter (ctx, &buf, val, 1))
    {
      lisp_free (ctx, buf.s);
      return NULL;
    }

  return buf.s;
}

static const struct lisp_object_operations lisp_cons_operations = {
  .free = &lisp_cons_free,
  .to_string = &lisp_cons_to_string,
};

lisp_value_t
lisp_new_cons (lisp_context_t *ctx, lisp_value_t car, lisp_value_t cdr)
{
  struct lisp_cons *cons;

  if (LISP_IS_EXCEPTION (car) || LISP_IS_EXCEPTION (cdr))
    {
      lisp_free_value (ctx, car);
      lisp_free_value (ctx, cdr);
      return lisp_exception ();
    }

  cons = lisp_malloc (ctx, sizeof (*cons));
  if (!cons)
    return lisp_throw_out_of_memory (ctx);

  cons->obj.ops = &lisp_cons_operations;
  cons->obj.ref_count = 1;

  cons->car = car;
  cons->cdr = cdr;

  {
    lisp_value_t val = LISP_OBJECT (LISP_TAG_LIST, cons);
    return val;
  }
}

struct lisp_symbol
{
  struct lisp_object obj;
  int is_static : 1;
  char *name;
};

static void
lisp_symbol_free (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_symbol *sym = (struct lisp_symbol *)obj;
  if (!sym->is_static)
    lisp_free (ctx, sym->name);
  lisp_free (ctx, sym);
}

static char *
lisp_symbol_to_string (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_symbol *sym = (struct lisp_symbol *)obj;
  return lisp_strdup (ctx, sym->name);
}

static const struct lisp_object_operations lisp_symbol_operations = {
  .free = &lisp_symbol_free,
  .to_string = &lisp_symbol_to_string,
};

static const struct lisp_object_operations lisp_static_symbol_operations = {
  .free = NULL,
  .to_string = &lisp_symbol_to_string,
};

static char *
lisp_toupper (lisp_context_t *ctx, const char *str)
{
  size_t i;
  size_t len;
  char *r;

  if (!str)
    return NULL;

  len = strlen (str);
  r = lisp_malloc (ctx, len + 1);
  if (!r)
    return NULL;
  for (i = 0; i < len + 1; ++i)
    r[i] = toupper (str[i]);
  return r;
}

lisp_value_t
lisp_new_symbol_full (lisp_context_t *ctx, const char *name, int is_static)
{
  struct lisp_symbol *sym = lisp_malloc (ctx, sizeof (*sym));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_SYMBOL, sym);

  assert (name != NULL);
  sym->is_static = is_static;
  sym->name = lisp_toupper (ctx, name);

  sym->obj.ref_count = 1;
  sym->obj.ops = &lisp_symbol_operations;

  return val;
}

lisp_value_t
lisp_new_symbol (lisp_context_t *ctx, const char *name)
{
  return lisp_new_symbol_full (ctx, name, 0);
}

#define LISP_SYMBOL_OBJECT_INIT(name, value)                                  \
  {                                                                           \
    { 1, &lisp_static_symbol_operations }, 1, (char *)(value)                 \
  }

#define LISP_DEFINE_SYMBOL_OBJECT(name, value)                                \
  struct lisp_symbol name = LISP_SYMBOL_OBJECT_INIT (name, value)

#define LISP_DEFINE_SYMBOL(name, value)                                       \
  static LISP_DEFINE_SYMBOL_OBJECT (static_symbol_##name, value);             \
  static lisp_value_t name                                                    \
      = LISP_OBJECT (LISP_TAG_SYMBOL, &static_symbol_##name);

static unsigned
lisp_sym_eq (lisp_value_ref_t a, lisp_value_ref_t b)
{
  return a.tag == LISP_TAG_SYMBOL && b.tag == LISP_TAG_SYMBOL
         && strcmp (((struct lisp_symbol *)a.ptr)->name,
                    ((struct lisp_symbol *)b.ptr)->name)
                == 0;
}

struct lisp_string
{
  struct lisp_object obj;
  int is_static : 1;
  char *str;
};

static void
lisp_string_free (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_string *str = (struct lisp_string *)obj;
  if (!str->is_static)
    lisp_free (ctx, str->str);
  lisp_free (ctx, str);
}

static char *
lisp_string_to_string (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_string *str = (struct lisp_string *)obj;
  size_t length = strlen (str->str);
  char *buf = lisp_malloc (ctx, length + 3);
  snprintf (buf, length + 3, "\"%s\"", str->str);
  return buf;
}

static const struct lisp_object_operations lisp_string_operations = {
  .free = &lisp_string_free,
  .to_string = &lisp_string_to_string,
};

static const struct lisp_object_operations lisp_static_string_operations = {
  .free = NULL,
  .to_string = &lisp_string_to_string,
};

lisp_value_t
lisp_new_string_full (lisp_context_t *ctx, const char *s, int is_static)
{
  struct lisp_string *str = lisp_malloc (ctx, sizeof (*str));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_STRING, str);
  if (!str)
    return lisp_throw_out_of_memory (ctx);

  str->is_static = is_static;
  str->str = lisp_strdup (ctx, s);
  str->obj.ops = &lisp_string_operations;
  str->obj.ref_count = 1;
  return val;
}

lisp_value_t
lisp_new_string (lisp_context_t *ctx, const char *s)
{
  return lisp_new_string_full (ctx, s, 0);
}

lisp_value_t
lisp_new_string_len (lisp_context_t *ctx, const char *n, size_t len)
{
  lisp_value_t val;
  char *s = strndup (n, len);
  if (!s)
    return lisp_throw_out_of_memory (ctx);

  val = lisp_new_string (ctx, s);
  free (s);
  return val;
}

#define LISP_STRING_OBJECT_INIT(name, value)                                  \
  {                                                                           \
    { 1, &lisp_static_string_operations }, 1, (char *)(value)                 \
  }

#define LISP_DEFINE_STRING_OBJECT(name, value)                                \
  struct lisp_string name = LISP_STRING_OBJECT_INIT (name, value)

#define LISP_DEFINE_STRING(name, value)                                       \
  static LISP_DEFINE_STRING_OBJECT (static_string_##name, value);             \
  static lisp_value_t name                                                    \
      = LISP_OBJECT (LISP_TAG_STRING, &static_string_##name);

lisp_value_t
lisp_throw (lisp_context_t *ctx, lisp_value_t error)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  rt->exception_list = lisp_new_cons (ctx, error, rt->exception_list);
  return lisp_exception ();
}

lisp_value_t
lisp_throw_out_of_memory (lisp_context_t *ctx)
{
  LISP_DEFINE_STRING (error_string, "Out of memory");
  return lisp_throw (ctx, error_string);
}

lisp_value_t
lisp_throw_internal_error_v (lisp_context_t *ctx, const char *fmt, va_list ap)
{
  char buffer[500];
  vsnprintf (buffer, sizeof (buffer), fmt, ap);
  return lisp_throw (ctx, lisp_new_string (ctx, buffer));
}

lisp_value_t
lisp_throw_internal_error (lisp_context_t *ctx, const char *fmt, ...)
{
  va_list ap;

  assert (fmt != NULL);

  va_start (ap, fmt);
  lisp_throw_internal_error_v (ctx, fmt, ap);
  va_end (ap);

  return lisp_exception ();
}

enum lisp_parse_error
{
  LISP_PE_EOF = 1,
  LISP_PE_EARLY_EOF = 2,
  LISP_PE_EXPECT_RIGHT_PAREN = 3,
  LISP_PE_INVALID_NUMBER_LITERAL = 4,
  LISP_PE_INVALID_BOOLEAN_LITERAL = 5,
  LISP_PE_INVALID_TOKEN = 6,
};

lisp_value_t
lisp_throw_parse_error (lisp_context_t *ctx, enum lisp_parse_error code,
                        const char *fmt, ...)
{
  va_list ap;
  (void)code;

  va_start (ap, fmt);
  lisp_throw_internal_error_v (ctx, fmt, ap);
  va_end (ap);

  return lisp_exception ();
}

lisp_value_t
lisp_get_exception (lisp_context_t *ctx)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  lisp_value_t val = lisp_car (ctx, rt->exception_list);
  rt->exception_list = lisp_cdr_take (ctx, rt->exception_list);
  return val;
}

void
lisp_print_exception (lisp_context_t *ctx)
{
  lisp_value_t error = lisp_get_exception (ctx);
  if (LISP_IS_EXCEPTION (error))
    abort ();

  printf ("%s: ", ctx->name);
  lisp_print_value (ctx, error);
  lisp_free_value (ctx, error);
}

#define LISP_SYMBOL_STR(sym) (((struct lisp_symbol *)(sym).ptr)->name)

enum lisp_reader_state
{
  LISP_RS_PEEK,
  LISP_RS_NEXT,
};

inline static unsigned
streq (const char *a, const char *b)
{
  return strcmp (a, b) == 0;
}

struct lisp_reader
{
  lisp_context_t *ctx;
  FILE *filep;

  int state;
  char *token;

  struct string_buf buf;
};

static const char *lisp_next_token (struct lisp_reader *reader);

static const char *lisp_peek_token (struct lisp_reader *reader);

static lisp_value_t lisp_read_atom (struct lisp_reader *reader);

static lisp_value_t lisp_read_list (struct lisp_reader *reader);

void
lisp_reader_init (struct lisp_reader *reader, lisp_context_t *ctx)
{
  reader->filep = stdin;
  reader->ctx = ctx;
  reader->state = LISP_RS_NEXT;
  reader->token = NULL;
  reader->buf.s = NULL;
  reader->buf.capacity = 0;
  reader->buf.length = 0;
}

void
lisp_reader_destroy (struct lisp_reader *reader)
{
  free (reader->buf.s);
}

lisp_reader_t *
lisp_reader_new (lisp_context_t *ctx, FILE *filep)
{
  lisp_reader_t *reader = lisp_malloc (ctx, sizeof (*reader));
  if (!reader)
    return reader;

  lisp_reader_init (reader, ctx);
  reader->filep = filep;
  return reader;
}

void
lisp_reader_free (lisp_reader_t *reader)
{
  lisp_context_t *ctx = reader->ctx;
  lisp_reader_destroy (reader);
  lisp_free (ctx, reader);
}

lisp_value_t
lisp_read_form (struct lisp_reader *reader)
{
  const char *token;

  token = lisp_peek_token (reader);
  if (!token)
    return lisp_throw_parse_error (reader->ctx, LISP_PE_EOF, "EOF");

  if (streq (token, "("))
    return lisp_read_list (reader);

  if (streq (token, ")"))
    return lisp_throw_parse_error (reader->ctx, LISP_PE_EXPECT_RIGHT_PAREN,
                                   "Unexpected ')'");

  if (streq (token, "'"))
    {
      LISP_DEFINE_SYMBOL (symbol_quote, "QUOTE");
      lisp_next_token (reader);
      lisp_value_t quoted = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (quoted))
        return lisp_exception ();
      return lisp_new_cons (reader->ctx,
                            lisp_dup_value (reader->ctx, symbol_quote),
                            lisp_new_cons (reader->ctx, quoted, lisp_nil ()));
    }

  return lisp_read_atom (reader);
}

lisp_value_t
lisp_read_list (struct lisp_reader *reader)
{
  const char *token;
  lisp_value_t val = LISP_NIL;
  lisp_value_t *tail = &val;

  token = lisp_next_token (reader);
  assert (streq (token, "("));

  for (;;)
    {
      lisp_value_t form;
      token = lisp_peek_token (reader);
      if (!token)
        {
          lisp_throw_parse_error (reader->ctx, LISP_PE_EARLY_EOF,
                                  "Unexpected eof when parsing list");
          goto fail;
        }

      if (streq (token, ")"))
        break;

      if (streq (token, "."))
        {
          lisp_next_token (reader);
          *tail = lisp_read_form (reader);
          if (LISP_IS_EXCEPTION (*tail))
            goto fail;
          token = lisp_next_token (reader);
          if (!streq (token, ")"))
            {
              lisp_throw_parse_error (reader->ctx, LISP_PE_EXPECT_RIGHT_PAREN,
                                      "expected ')' but got '%s'", token);
              goto fail;
            }
          return val;
        }

      form = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (form))
        goto fail;

      *tail = lisp_new_cons (reader->ctx, form, lisp_nil ());
      tail = &((struct lisp_cons *)tail->ptr)->cdr;
    }

  lisp_next_token (reader); // eat ')'
  return val;

fail:
  lisp_free_value (reader->ctx, val);
  return lisp_exception ();
}

static const char *
strend (const char *s)
{
  size_t len = strlen (s);
  return s + len;
}

lisp_value_t
lisp_read_atom (struct lisp_reader *reader)
{
  const char *token = lisp_next_token (reader);

  assert (!!token);
  assert (!streq (token, "("));
  assert (!streq (token, ")"));

  if (isdigit (token[0]))
    {
      if (strchr (token, '.'))
        {
          char *endptr = NULL;
          double n = strtod (token, &endptr);
          if (endptr != strend (token))
            return lisp_throw_parse_error (
                reader->ctx, LISP_PE_INVALID_NUMBER_LITERAL,
                "invalid number literal: %s", token);
          return lisp_new_real (reader->ctx, n);
        }
      else
        {
          char *endptr = NULL;
          long n = strtol (token, &endptr, 0);
          if (endptr != strend (token))
            return lisp_throw_parse_error (
                reader->ctx, LISP_PE_INVALID_NUMBER_LITERAL,
                "invalid number literal: %s", token);
          return lisp_new_int32 (reader->ctx, n);
        }
    }

  if (token[0] == '"')
    return lisp_new_string_len (reader->ctx, token + 1, strlen (token) - 2);

  if (token[0] == '#')
    {
      if (strlen (token) != 2
          || (toupper (token[1]) != 'T' && toupper (token[1]) != 'F'))
        return lisp_throw_parse_error (reader->ctx,
                                       LISP_PE_INVALID_BOOLEAN_LITERAL,
                                       "Invalid boolean: %s", token);
      if (toupper (token[1]) == 'T')
        return lisp_true ();
      else
        return lisp_false ();
    }

  return lisp_new_symbol (reader->ctx, token);
}

/*
  space and comma
  symbol
*/
#define LISP_TOKEN_PATTERN                                                    \
  "\\s|([a-zA-Z*+/-%?<>=!&][a-zA-Z0-9*+/"                                     \
  "-%?<>=!&]*|\\(|\\)|(+|-)([1-9][0-9]*|0)|\\.|')"

static int
lisp_do_next_token (struct lisp_reader *reader)
{
  int ch;

  reader->buf.length = 0;

  while ((ch = getc (reader->filep)) != EOF)
    {
      if (!isspace (ch))
        break;
    }

  switch (ch)
    {
    case EOF:
      reader->token = NULL;
      return 0;

    case '(':
      reader->token = "(";
      return 0;

    case ')':
      reader->token = ")";
      return 0;

    case '\'':
      reader->token = "'";
      return 0;

    case '.':
      reader->token = ".";
      return 0;

    case '0' ... '9':
      string_buf_append_char (&reader->buf, ch);
      while ((ch = getc (reader->filep)) != EOF)
        if (isdigit (ch))
          string_buf_append_char (&reader->buf, ch);
        else if (isspace (ch))
          break;
        else if (ch == ')')
          {
            ungetc (ch, reader->filep);
            break;
          }
        else
          {
            reader->token = NULL;
            return LISP_PE_INVALID_TOKEN;
          }

      reader->token = reader->buf.s;
      return 0;

    case 'a' ... 'z':
    case 'A' ... 'Z':
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '^':
    case '>':
    case '<':
    case '=':
    case '!':
    case '?':
    case '&':
    case '#':
      string_buf_append_char (&reader->buf, ch);
      while ((ch = getc (reader->filep)) != EOF)
        {
          if (isalnum (ch) || !!strchr ("+-*/%^><=!&?", ch))
            string_buf_append_char (&reader->buf, ch);
          else if (isspace (ch))
            break;
          else if (ch == ')')
            {
              ungetc (ch, reader->filep);
              break;
            }
          else
            {
              reader->token = NULL;
              return LISP_PE_INVALID_TOKEN;
            }
        }
      reader->token = reader->buf.s;
      return 0;

    default:
      fprintf (stderr, "invalid char: %o\n", ch);
      reader->token = NULL;
      return LISP_PE_INVALID_TOKEN;
    }
}

static const char *
lisp_next_token (struct lisp_reader *reader)
{
  if (reader->state == LISP_RS_PEEK)
    {
      reader->state = LISP_RS_NEXT;
      return reader->token;
    }
  else
    {
      lisp_do_next_token (reader);
      return reader->token;
    }
}

static const char *
lisp_peek_token (struct lisp_reader *reader)
{
  if (reader->state == LISP_RS_NEXT)
    {
      lisp_do_next_token (reader);

      reader->state = LISP_RS_PEEK;
      return reader->token;
    }
  else
    {
      return reader->token;
    }
}

char *
lisp_value_to_string (lisp_context_t *ctx, lisp_value_ref_t val)
{
  char *r;
  struct lisp_object *obj;

  switch (val.tag)
    {
    case LISP_TAG_INT32:
      r = lisp_malloc (ctx, 21);
      snprintf (r, 20, "%" PRIi32, val.i32);
      return r;

    case LISP_TAG_INT64:
      r = lisp_malloc (ctx, 41);
      snprintf (r, 40, "%" PRIi64, val.i64);
      return r;

    case LISP_TAG_BOOLEAN:
      if (val.i32)
        return lisp_strdup (ctx, "#T");
      else
        return lisp_strdup (ctx, "#F");

    case LISP_TAG_EXCEPTION:
      assert (0 && "exception");
      return NULL;

    case LISP_TAG_LIST:
      if (LISP_IS_NIL (val))
        return lisp_strdup (ctx, "()");
      /* fall through */

    default:
      obj = val.ptr;
      if (obj->ops->to_string)
        {
          return obj->ops->to_string (ctx, obj);
        }

      return lisp_strdup (ctx, "#OBJECT");
    }
}

void
lisp_print_value (lisp_context_t *ctx, lisp_value_ref_t val)
{
  char *s = lisp_value_to_string (ctx, val);
  puts (s);
  lisp_free (ctx, s);
}

lisp_context_t *
lisp_context_new_with_parent (lisp_context_t *parent, const char *name)
{
  lisp_context_t *new_ctx = lisp_context_new (lisp_get_runtime (parent), name);
  new_ctx->parent = parent;
  return new_ctx;
}

int
lisp_to_int32 (lisp_context_t *ctx, int32_t *result, lisp_value_ref_t val)
{
  if (LISP_IS_EXCEPTION (val))
    return -1;
  if (val.tag == LISP_TAG_INT32)
    *result = val.i32;
  else if (val.tag == LISP_TAG_INT64)
    *result = (int32_t)val.i64;
  else
    {
      lisp_throw_internal_error (ctx, "Value error: %d", val.tag);
      return -1;
    }
  return 0;
}

static uint32_t
lisp_hash_str (const char *s)
{
  const uint32_t p = 31;
  const uint32_t m = 1e9 + 9;
  uint32_t p_pow = 1;
  uint32_t val = 0;
  size_t i;
  size_t len = strlen (s);

  for (i = 0; i < len; ++i)
    {
      val = (val + s[i] * p_pow) % m;
      p_pow = p_pow * p % m;
    }

  return val;
}

static struct lisp_variable_store *
lisp_context_find_variable (lisp_context_t *ctx, lisp_value_ref_t name)
{
  struct lisp_variable *var;
  uint32_t key;

  key = lisp_hash_str (LISP_SYMBOL_STR (name));
  hash_for_each_possible (ctx->var_table, var, node, key)
  {
    if (lisp_sym_eq (var->name, name))
      return lisp_variable_store_ref (ctx, var->store);
  }

  if (ctx->parent)
    {
      return lisp_context_find_variable (ctx->parent, name);
    }

  return NULL;
}

static int lisp_set_value_in_context (lisp_context_t *ctx, lisp_value_t name,
                                      lisp_value_t value);

static int
lisp_function_set_args (lisp_context_t *ctx, lisp_context_t *new_ctx,
                        lisp_value_ref_t params, lisp_value_ref_t args)
{
  lisp_dup_value (ctx, params);
  lisp_dup_value (ctx, args);
  while (!LISP_IS_NIL (params))
    {
      lisp_value_t name = lisp_car (ctx, params);
      lisp_value_t expr = lisp_car (ctx, args);
      lisp_value_t value = lisp_eval (ctx, expr);
      lisp_free_value (ctx, expr);
      if (lisp_set_value_in_context (new_ctx, name, value))
        {
          lisp_free_value (ctx, args);
          lisp_free_value (ctx, params);
          return -1;
        }
      params = lisp_cdr_take (ctx, params);
      args = lisp_cdr_take (ctx, args);
    }

  return 0;
}

static lisp_value_t
lisp_eval_list (lisp_context_t *ctx, lisp_value_ref_t list)
{
  lisp_value_t val = LISP_NIL;
  lisp_dup_value (ctx, list);
  while (!LISP_IS_NIL (list))
    {
      lisp_value_t exp = lisp_car (ctx, list);
      list = lisp_cdr_take (ctx, list);
      lisp_free_value (ctx, val);
      val = lisp_eval (ctx, exp);
      lisp_free_value (ctx, exp);
    }
  lisp_free_value (ctx, list);
  return val;
}

static lisp_value_t
lisp_function_invoker (lisp_context_t *ctx, lisp_value_ref_t args,
                       struct lisp_function *func)
{
  lisp_value_t val;
  lisp_context_t *new_ctx
      = lisp_context_new_with_parent (func->ctx, LISP_SYMBOL_STR (func->name));
  lisp_function_set_args (ctx, new_ctx, func->params, args);
  val = lisp_eval_list (new_ctx, func->body);
  lisp_context_free (new_ctx);
  return val;
}

static unsigned
lisp_list_contains_symbol (lisp_context_t *ctx, lisp_value_ref_t list,
                           lisp_value_t elem)
{
  unsigned result = 0;
  lisp_dup_value (ctx, list);

  while (!LISP_IS_NIL (list))
    {
      lisp_value_t car = lisp_car (ctx, list);
      result = lisp_sym_eq (car, elem);
      lisp_free_value (ctx, car);

      if (result)
        break;
      list = lisp_cdr_take (ctx, list);
    }

  lisp_free_value (ctx, list);
  return result;
}

static unsigned
lisp_var_list_contains (lisp_context_t *ctx, lisp_value_ref_t name)
{
  size_t bkt;
  struct lisp_variable *var;

  hash_for_each (ctx->var_table, bkt, var, node)
  {
    if (lisp_sym_eq (var->name, name))
      return 1;
  }

  return 0;
}

static int
lisp_resolve_variables_in_expr (lisp_context_t *ctx, lisp_context_t *new_ctx,
                                lisp_value_ref_t params, lisp_value_ref_t expr)
{
  int res = 0;
  if (LISP_IS_LIST (expr))
    {
      lisp_dup_value (ctx, expr);

      while (!LISP_IS_NIL (expr))
        {
          lisp_value_t car = lisp_car (ctx, expr);
          res += lisp_resolve_variables_in_expr (ctx, new_ctx, params, car);
          lisp_free_value (ctx, car);
          expr = lisp_cdr_take (ctx, expr);
        }
      return res;
    }

  if (LISP_IS_SYMBOL (expr))
    {
      if (lisp_list_contains_symbol (ctx, params, expr))
        return 0;

      if (lisp_var_list_contains (new_ctx, expr))
        return 0;

      {
        struct lisp_variable *var;
        struct lisp_variable_store *store
            = lisp_context_find_variable (ctx, expr);
        if (!store)
          return 1;

        var = lisp_malloc (ctx, sizeof (*var));
        var->name = lisp_dup_value (ctx, expr);
        var->store = store;

        hash_add (new_ctx->var_table, &var->node, lisp_hash_str (LISP_SYMBOL_STR(var->name)));
        return 0;
      }
    }

  return 0;
}

static int
lisp_resolve_variables_in_body (lisp_context_t *ctx, lisp_context_t *new_ctx,
                                lisp_value_ref_t params, lisp_value_ref_t body)
{
  int res = 0;
  lisp_dup_value (ctx, body);
  while (!LISP_IS_NIL (body))
    {
      lisp_value_t exp = lisp_car (ctx, body);
      res += lisp_resolve_variables_in_expr (ctx, new_ctx, params, exp);
      lisp_free_value (ctx, exp);
      body = lisp_cdr_take (ctx, body);
    }
  lisp_free_value (ctx, body);
  return res;
}

static lisp_value_t
lisp_new_function (lisp_context_t *ctx, lisp_value_t params, lisp_value_t body,
                   lisp_native_invoke_t invoker)
{
  struct lisp_function *fn = lisp_malloc (ctx, sizeof (*fn));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_LAMBDA, fn);

  fn->obj.ref_count = 1;
  fn->obj.ops = &lisp_function_operations;

  fn->params = params;
  fn->body = body;
  fn->invoker = invoker;

  fn->name = lisp_new_symbol (ctx, "#LAMBDA");

  fn->ctx
      = lisp_context_new (lisp_get_runtime (ctx), LISP_SYMBOL_STR (fn->name));
  lisp_resolve_variables_in_body (ctx, fn->ctx, fn->params, fn->body);

  return val;
}

struct lisp_function_proxy
{
  struct lisp_object obj;
  struct lisp_function *func;
};

static void
lisp_function_proxy_free (lisp_context_t *ctx, struct lisp_object *obj)
{
  lisp_free (ctx, obj);
}

static const struct lisp_object_operations lisp_function_proxy_operations = {
  .free = lisp_function_proxy_free,
};

static lisp_value_t
lisp_function_proxy_new (lisp_context_t *ctx, struct lisp_function *func)
{
  struct lisp_function_proxy *proxy = lisp_malloc (ctx, sizeof (*proxy));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_FUNCTION_PROXY, proxy);
  if (!proxy)
    return lisp_exception ();

  proxy->func = func;
  proxy->obj.ref_count = 1;
  proxy->obj.ops = &lisp_function_proxy_operations;

  return val;
}

static int
lisp_set_function_name (lisp_context_t *ctx, lisp_value_ref_t function,
                        lisp_value_t name)
{
  struct lisp_function *fn = lisp_get_object (ctx, function, LISP_TAG_LAMBDA);

  if (!fn)
    {
      lisp_free_value (ctx, name);
      return -1;
    }
  lisp_free_value (ctx, fn->name);
  fn->name = name;

  lisp_set_value_in_context (fn->ctx, lisp_dup_value (fn->ctx, fn->name),
                             lisp_function_proxy_new (ctx, fn));

  return 0;
}

static struct lisp_variable_store *
lisp_find_or_create_variable (lisp_context_t *ctx, lisp_value_ref_t name)
{
  struct lisp_variable_store *store;
  struct lisp_variable *var;

  store = lisp_context_find_variable (ctx, name);
  if (store)
    return store;

  store = lisp_variable_store_new (ctx);
  if (!store)
    return NULL;

  var = lisp_malloc (ctx, sizeof (*var));
  if (!var)
    goto fail;

  var->name = lisp_dup_value (ctx, name);
  var->store = lisp_variable_store_ref (ctx, store);

  hash_add (ctx->var_table, &var->node, lisp_hash_str (LISP_SYMBOL_STR (name)));

  return store;

fail:
  lisp_variable_store_unref (ctx, store);
  return NULL;
}

static int
lisp_set_value_in_context (lisp_context_t *ctx, lisp_value_t name,
                           lisp_value_t value)
{
  struct lisp_variable_store *store;

  if (LISP_IS_EXCEPTION (name) || LISP_IS_EXCEPTION (value))
    goto fail;

  store = lisp_find_or_create_variable (ctx, name);
  if (!store)
    goto fail;

  lisp_free_value (ctx, store->value);
  store->value = value;

  lisp_free_value (ctx, name);
  lisp_variable_store_unref (ctx, store);

  return 0;

fail:
  lisp_free_value (ctx, name);
  lisp_free_value (ctx, value);
  return -1;
}

/**
 * (define var value)
 * (define (func params) body)
 */
static lisp_value_t
lisp_define (lisp_context_t *ctx, lisp_value_ref_t args,
             struct lisp_function *unused)
{
  lisp_value_t sig = lisp_car (ctx, args);
  (void)unused;

  if (sig.tag == LISP_TAG_LIST)
    { // defining function
      lisp_value_t name = lisp_car (ctx, sig);
      lisp_value_t params = lisp_cdr (ctx, sig);
      lisp_value_t body = lisp_cdr (ctx, args);
      lisp_value_t func
          = lisp_new_function (ctx, params, body, &lisp_function_invoker);
      lisp_free_value (ctx, sig);

      if (lisp_set_value_in_context (ctx, name, func))
        return lisp_exception ();

      lisp_set_function_name (ctx, func, lisp_dup_value (ctx, name));

      return lisp_nil ();
    }
  else if (sig.tag == LISP_TAG_SYMBOL)
    { // define variable
      lisp_value_t tmp = lisp_cdr (ctx, args);
      lisp_value_t expr = lisp_car (ctx, tmp);
      lisp_value_t value = lisp_eval (ctx, expr);
      lisp_free_value (ctx, tmp);
      lisp_free_value (ctx, expr);
      if (lisp_set_value_in_context (ctx, sig, value))
        return lisp_exception ();
      return lisp_nil ();
    }

  return lisp_throw_internal_error (ctx, "Invalid syntax");
}

/**
 * (quote exp)
 */
static lisp_value_t
lisp_quote (lisp_context_t *ctx, lisp_value_ref_t list,
            struct lisp_function *unused)
{
  (void)unused;
  return lisp_car (ctx, list);
}

lisp_value_t
lisp_new_int32 (lisp_context_t *ctx, int32_t v)
{
  lisp_value_t val = LISP_INT32 (v);
  (void)ctx;
  return val;
}

lisp_value_t
lisp_new_real (lisp_context_t *ctx, double v)
{
  lisp_value_t val = LISP_REAL (v);
  (void)ctx;
  return val;
}

/**
 * (+ a [b ...])
 * (* a [b ...])
 */
static lisp_value_t
lisp_plus_or_multiply (lisp_context_t *ctx, lisp_value_ref_t args,
                       struct lisp_function *fn)
{
  int32_t sum = 0;
  int32_t product = 1;
  int32_t num = 0;
  lisp_value_t result = LISP_EXCEPTION;
  lisp_value_t list = lisp_dup_value (ctx, args);
  LISP_DEFINE_SYMBOL (name_plus, "+");
  // LISP_DEFINE_SYMBOL (name_multiply, "*");

  while (!LISP_IS_NIL (list))
    {
      lisp_value_t tmp = lisp_car (ctx, list);
      lisp_value_t val = lisp_eval (ctx, tmp);
      lisp_free_value (ctx, tmp);
      if (lisp_to_int32 (ctx, &num, val))
        {
          lisp_free_value (ctx, val);
          goto fail;
        }
      lisp_free_value (ctx, val);

      if (lisp_sym_eq (name_plus, fn->params))
        sum += num;
      else
        product *= num;
      list = lisp_cdr_take (ctx, list);
    }
  if (lisp_sym_eq (name_plus, fn->params))
    result = lisp_new_int32 (ctx, sum);
  else
    result = lisp_new_int32 (ctx, product);

fail:
  lisp_free_value (ctx, list);

  return result;
}

/**
 * (- n) => -n
 * (- n m) => n - m
 */
static lisp_value_t
lisp_minus (lisp_context_t *ctx, lisp_value_ref_t args,
            struct lisp_function *unused)
{
  int32_t r = 0, n = 0, m = 0;
  lisp_value_t result = LISP_EXCEPTION;
  lisp_value_t tmp = LISP_NIL;
  lisp_value_t arg2 = LISP_NIL;
  lisp_value_t arg2_val = LISP_NIL;
  lisp_value_t arg1_val = LISP_NIL;
  lisp_value_t arg1 = lisp_car (ctx, args);
  (void)unused;

  if (LISP_IS_EXCEPTION (arg1))
    return lisp_exception ();
  arg1_val = lisp_eval (ctx, arg1);
  if (LISP_IS_EXCEPTION (arg1_val))
    goto fail;
  if (lisp_to_int32 (ctx, &n, arg1_val))
    goto fail;
  tmp = lisp_cdr (ctx, args);
  if (LISP_IS_EXCEPTION (tmp))
    goto fail;
  if (LISP_IS_NIL (tmp))
    {
      r = -n;
      result = lisp_new_int32 (ctx, r);
    }
  else
    {
      arg2 = lisp_car (ctx, tmp);
      if (LISP_IS_EXCEPTION (arg2))
        goto fail;
      arg2_val = lisp_eval (ctx, arg2);
      if (LISP_IS_EXCEPTION (arg2_val))
        goto fail;
      if (lisp_to_int32 (ctx, &m, arg2_val))
        goto fail;

      r = n - m;
      result = lisp_new_int32 (ctx, r);
    }

fail:
  lisp_free_value (ctx, arg2_val);
  lisp_free_value (ctx, arg2);
  lisp_free_value (ctx, tmp);
  lisp_free_value (ctx, arg1_val);
  lisp_free_value (ctx, arg1);

  return result;
}

static lisp_value_t
lisp_binary_op_number (lisp_context_t *ctx, lisp_value_ref_t args,
                       struct lisp_function *fn)
{
  LISP_DEFINE_SYMBOL (greater, ">");
  LISP_DEFINE_SYMBOL (less, "<");
  LISP_DEFINE_SYMBOL (greater_equal, ">=");
  LISP_DEFINE_SYMBOL (less_equal, "<=");
  LISP_DEFINE_SYMBOL (equal, "=");
  LISP_DEFINE_SYMBOL (not_equal, "!=");
  LISP_DEFINE_SYMBOL (divide, "/");
  LISP_DEFINE_SYMBOL (modulus, "%");
  LISP_DEFINE_SYMBOL (bit_and, "&");
  LISP_DEFINE_SYMBOL (bit_or, "|");
  LISP_DEFINE_SYMBOL (bit_xor, "^");

  const lisp_value_t true_ = LISP_TRUE;

  lisp_value_t arg1 = LISP_NIL;
  lisp_value_t arg2 = LISP_NIL;

  lisp_value_t arg1_val = LISP_NIL;
  lisp_value_t arg2_val = LISP_NIL;

  lisp_value_t tmp = LISP_NIL;
  lisp_value_t result = LISP_EXCEPTION;

  int32_t n1, n2;

  arg1 = lisp_car (ctx, args);
  if (LISP_IS_EXCEPTION (arg1))
    return lisp_exception ();
  arg1_val = lisp_eval (ctx, arg1);
  if (LISP_IS_EXCEPTION (arg1_val))
    goto fail;
  if (lisp_to_int32 (ctx, &n1, arg1_val))
    goto fail;

  tmp = lisp_cdr (ctx, args);
  if (LISP_IS_EXCEPTION (tmp))
    goto fail;

  arg2 = lisp_car (ctx, tmp);
  if (LISP_IS_EXCEPTION (arg2))
    goto fail;
  arg2_val = lisp_eval (ctx, arg2);
  if (LISP_IS_EXCEPTION (arg2_val))
    goto fail;
  if (lisp_to_int32 (ctx, &n2, arg2_val))
    goto fail;

  if (lisp_sym_eq (fn->params, greater))
    {
      if (n1 > n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_false ();
    }
  else if (lisp_sym_eq (fn->params, less))
    {
      if (n1 < n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_false ();
    }
  else if (lisp_sym_eq (fn->params, greater_equal))
    {
      if (n1 >= n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_false ();
    }
  else if (lisp_sym_eq (fn->params, less_equal))
    {
      if (n1 <= n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_false ();
    }
  else if (lisp_sym_eq (fn->params, equal))
    {
      if (n1 == n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_false ();
    }
  else if (lisp_sym_eq (fn->params, not_equal))
    {
      if (n1 != n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_false ();
    }

  else if (lisp_sym_eq (fn->params, divide))
    {
      result = lisp_new_int32 (ctx, n1 / n2);
    }
  else if (lisp_sym_eq (fn->params, modulus))
    {
      result = lisp_new_int32 (ctx, n1 % n2);
    }
  else if (lisp_sym_eq (fn->params, bit_and))
    {
      result = lisp_new_int32 (ctx, n1 & n2);
    }
  else if (lisp_sym_eq (fn->params, bit_or))
    {
      result = lisp_new_int32 (ctx, n1 | n2);
    }
  else if (lisp_sym_eq (fn->params, bit_xor))
    {
      result = lisp_new_int32 (ctx, n1 ^ n2);
    }
  else
    abort ();

fail:
  lisp_free_value (ctx, arg2_val);
  lisp_free_value (ctx, arg2);
  lisp_free_value (ctx, tmp);
  lisp_free_value (ctx, arg1_val);
  lisp_free_value (ctx, arg1);

  return result;
}

static long long
list_length (struct list_head *head)
{
  struct list_head *h;
  long long n = 0;

  list_for_each (h, head) { ++n; }

  return n;
}

static int
lisp_do_dump_context (lisp_context_t *ctx)
{
  long long nr_variables = -1; //list_length (&ctx->var_list);

  printf ("context [ name = %s, "
          "nr_variables: %lld ]\n",
          ctx->name, nr_variables);
  return 0;
}

/**
 * (dump-context)
 */
static lisp_value_t
lisp_dump_context (lisp_context_t *ctx, lisp_value_ref_t args,
                   struct lisp_function *unused)
{
  (void)unused;
  if (!LISP_IS_NIL (args))
    return lisp_throw_internal_error (ctx, "Unexpected arguments");

  while (ctx != NULL)
    {
      lisp_do_dump_context (ctx);
      ctx = ctx->parent;
    }

  return lisp_nil ();
}

static int
lisp_do_print_car (lisp_context_t *ctx, lisp_value_ref_t list)
{
  lisp_value_t expr = lisp_car (ctx, list);
  lisp_value_t val = lisp_eval (ctx, expr);

  if (LISP_IS_EXCEPTION (val))
    {
      lisp_free_value (ctx, expr);
      return -1;
    }

  char *str = lisp_value_to_string (ctx, val);
  printf ("%s", str);
  lisp_free (ctx, str);
  lisp_free_value (ctx, val);
  lisp_free_value (ctx, expr);
  return 0;
}

/**
 * (dump-context)
 */
static lisp_value_t
lisp_print (lisp_context_t *ctx, lisp_value_ref_t args,
            struct lisp_function *unused)
{
  (void)unused;

  lisp_dup_value (ctx, args);
  while (!LISP_IS_NIL (args))
    {
      if (lisp_do_print_car (ctx, args))
        {
          lisp_free_value (ctx, args);
          return lisp_exception ();
        }
      args = lisp_cdr_take (ctx, args);
    }

  printf ("\n");
  return lisp_nil ();
}

int
lisp_to_bool (lisp_context_t *ctx, lisp_value_ref_t val, int *res)
{
  if (LISP_IS_EXCEPTION (val))
    return -1;

  if (val.tag != LISP_TAG_BOOLEAN)
    {
      lisp_throw_internal_error (ctx, "Expected a boolean");
      return -1;
    }

  *res = val.i32;
  return 0;
}

/**
 * (if cond texpr fexpr)
 */
static lisp_value_t
lisp_if (lisp_context_t *ctx, lisp_value_ref_t args,
         struct lisp_function *unused)
{
  lisp_value_t cond = LISP_NIL;
  lisp_value_t texpr = LISP_NIL;
  lisp_value_t fexpr = LISP_NIL;
  lisp_value_t res = LISP_EXCEPTION;

  (void)unused;

  lisp_dup_value (ctx, args);
  cond = lisp_car (ctx, args);
  args = lisp_cdr_take (ctx, args);
  texpr = lisp_car (ctx, args);
  args = lisp_cdr_take (ctx, args);
  if (!LISP_IS_NIL (args))
    fexpr = lisp_car (ctx, args);
  lisp_free_value (ctx, args);

  {
    int v = 0;
    lisp_value_t cond_var = lisp_eval (ctx, cond);
    if (LISP_IS_EXCEPTION (cond_var))
      goto fail;

    if (lisp_to_bool (ctx, cond_var, &v))
      {
        lisp_free_value (ctx, cond_var);
        goto fail;
      }

    if (!v)
      {
        if (LISP_IS_NIL (fexpr))
          res = lisp_nil ();
        else
          res = lisp_eval (ctx, fexpr);
      }
    else
      res = lisp_eval (ctx, texpr);
  }

fail:
  lisp_free_value (ctx, fexpr);
  lisp_free_value (ctx, texpr);
  lisp_free_value (ctx, cond);
  return res;
}

int
lisp_list_extract (lisp_context_t *ctx, lisp_value_ref_t list,
                   lisp_value_ref_t *heads, int n, lisp_value_ref_t *tail)
{

  int i;
  int res = -1;

  if (LISP_IS_EXCEPTION (list))
    return -1;

  *tail = lisp_dup_value (ctx, list);

  for (i = 0; i < n; ++i)
    {
      heads[i] = lisp_car (ctx, *tail);
      if (LISP_IS_EXCEPTION (heads[i]))
        goto fail;

      *tail = lisp_cdr_take (ctx, *tail);
    }

  res = 0;

fail:
  while (i > 0)
    {
      lisp_free_value (ctx, heads[i - 1]);
      i--;
    }

  lisp_free_value (ctx, *tail);
  return res;
}

/**
 * (cond (CONDTION BODY...) ...)
 */
static lisp_value_t
lisp_cond (lisp_context_t *ctx, lisp_value_ref_t args,
           struct lisp_function *unused)
{
  LISP_DEFINE_SYMBOL (sym_else, "ELSE");
  lisp_value_ref_t clause;
  (void)unused;

  while (!LISP_IS_NIL (args))
    {
      int value;
      lisp_value_t condition_value;
      lisp_value_ref_t condition, body;
      if (lisp_list_extract (ctx, args, &clause, 1, &args))
        return lisp_exception ();

      if (lisp_list_extract (ctx, clause, &condition, 1, &body))
        return lisp_exception ();

      if (LISP_IS_SYMBOL (condition) && lisp_sym_eq (condition, sym_else))
        {
          if (!LISP_IS_NIL (args))
            return lisp_throw_internal_error (
                ctx, "ELSE must be the last clause in COND");
          condition_value = lisp_true ();
        }
      else
        condition_value = lisp_eval (ctx, condition);
      if (LISP_IS_EXCEPTION (condition_value))
        return lisp_exception ();
      if (lisp_to_bool (ctx, condition_value, &value))
        {
          lisp_free_value (ctx, condition_value);
          return lisp_exception ();
        }
      lisp_free_value (ctx, condition_value);
      if (value)
        {
          return lisp_eval_list (ctx, body);
        }
    }

  return lisp_nil ();
}

/**
 * (lambda (args...) body)
 */
static lisp_value_t
lisp_lambda (lisp_context_t *ctx, lisp_value_ref_t args,
             struct lisp_function *unused)
{
  lisp_value_t params;
  lisp_value_t body;
  lisp_value_t val;
  (void)unused;

  params = lisp_car (ctx, args);
  body = lisp_cdr (ctx, args);

  val = lisp_new_function (ctx, params, body, lisp_function_invoker);

  return val;
}

lisp_value_t
lisp_true ()
{
  lisp_value_t val = LISP_TRUE;
  return val;
}

lisp_value_t
lisp_false ()
{
  lisp_value_t val = LISP_FALSE;
  return val;
}

static lisp_value_t
lisp_null_p (lisp_context_t *ctx, lisp_value_ref_t args,
             struct lisp_function *unused)
{
  lisp_value_t a = lisp_car (ctx, args);
  lisp_value_t av;
  lisp_value_t r = LISP_FALSE;

  (void)unused;

  if (LISP_IS_EXCEPTION (a))
    return lisp_exception ();

  av = lisp_eval (ctx, a);
  if (LISP_IS_EXCEPTION (av))
    {
      lisp_free_value (ctx, a);
      return av;
    }

  if (LISP_IS_NIL (av))
    r = lisp_true ();

  lisp_free_value (ctx, av);
  lisp_free_value (ctx, a);
  return r;
}

static lisp_value_t
lisp_get_car (lisp_context_t *ctx, lisp_value_ref_t args,
              struct lisp_function *unused)
{
  lisp_value_t a = lisp_car (ctx, args);
  lisp_value_t av;
  lisp_value_t r = LISP_NIL;

  (void)unused;

  if (LISP_IS_EXCEPTION (a))
    return lisp_exception ();

  av = lisp_eval (ctx, a);
  lisp_free_value (ctx, a);
  if (LISP_IS_EXCEPTION (av))
    return lisp_exception ();

  r = lisp_car (ctx, av);

  lisp_free_value (ctx, av);
  return r;
}

static lisp_value_t
lisp_get_cdr (lisp_context_t *ctx, lisp_value_ref_t args,
              struct lisp_function *unused)
{
  lisp_value_t a = lisp_car (ctx, args);
  lisp_value_t av;
  lisp_value_t r = LISP_NIL;

  (void)unused;

  if (LISP_IS_EXCEPTION (a))
    return lisp_exception ();
  av = lisp_eval (ctx, a);
  lisp_free_value (ctx, a);
  if (LISP_IS_EXCEPTION (av))
    return lisp_exception ();

  r = lisp_cdr (ctx, av);

  lisp_free_value (ctx, av);
  return r;
}

static lisp_value_t
lisp_cons (lisp_context_t *ctx, lisp_value_ref_t args,
           struct lisp_function *unused)
{
  lisp_value_t a = lisp_car (ctx, args);
  lisp_value_t b;
  lisp_value_t r;

  (void)unused;

  args = lisp_cdr (ctx, args);
  b = lisp_car (ctx, args);

  lisp_free_value (ctx, args);

  r = lisp_new_cons (ctx, lisp_eval (ctx, a), lisp_eval (ctx, b));

  lisp_free_value (ctx, b);
  lisp_free_value (ctx, a);

  return r;
}

static lisp_value_t
lisp_list (lisp_context_t *ctx, lisp_value_ref_t args,
           struct lisp_function *unused)
{
  (void)unused;

  return lisp_dup_value (ctx, args);
}

static lisp_value_t
lisp_zero_p (lisp_context_t *ctx, lisp_value_ref_t args,
             struct lisp_function *unused)
{
  lisp_value_t a = lisp_car (ctx, args);
  lisp_value_t av = lisp_eval (ctx, a);
  lisp_value_t r = LISP_EXCEPTION;
  (void)unused;

  if (LISP_IS_EXCEPTION (av))
    {
    }

  else if ((av.tag == LISP_TAG_INT32 && av.i32 == 0)
           || (av.tag == LISP_TAG_INT64 && av.i64 == 0))
    {
      r = lisp_dup_value (ctx, lisp_true ());
    }
  else
    {
      r = lisp_false ();
    }
  lisp_free_value (ctx, av);
  lisp_free_value (ctx, a);
  return r;
}

static lisp_value_t
lisp_atom_p (lisp_context_t *ctx, lisp_value_ref_t args,
             struct lisp_function *unused)
{
  lisp_value_t a = lisp_car (ctx, args);
  lisp_value_t av = lisp_eval (ctx, a);
  lisp_value_t r = LISP_EXCEPTION;
  (void)unused;

  if (LISP_IS_EXCEPTION (av))
    {
    }

  else if (av.tag == LISP_TAG_SYMBOL || av.tag == LISP_TAG_INT32
           || av.tag == LISP_TAG_INT64 || av.tag == LISP_TAG_STRING
           || LISP_IS_NIL (av))
    {
      r = lisp_dup_value (ctx, lisp_true ());
    }
  else
    {
      r = lisp_false ();
    }
  lisp_free_value (ctx, av);
  lisp_free_value (ctx, a);
  return r;
}

static int
lisp_define_function (lisp_context_t *ctx, lisp_value_t name,
                      lisp_value_t data, lisp_native_invoke_t invoker)
{
  return lisp_set_value_in_context (
      ctx, name,
      lisp_new_function (ctx, lisp_dup_value (ctx, name), data, invoker));
}

#define LISP_DEFINE_FUNCTION(context, name, data, invoker)                    \
  do                                                                          \
    {                                                                         \
      LISP_DEFINE_SYMBOL (name_symbol, name);                                 \
      lisp_define_function (context, lisp_dup_value (context, name_symbol),   \
                            data, invoker);                                   \
    }                                                                         \
  while (0)

lisp_context_t *
lisp_new_global_context (lisp_runtime_t *rt)
{
  lisp_value_t nil = LISP_NIL;
  lisp_context_t *ctx = lisp_context_new (rt, "<GLOBAL>");

  LISP_DEFINE_FUNCTION (ctx, "DEFINE", nil, &lisp_define);
  LISP_DEFINE_FUNCTION (ctx, "QUOTE", nil, &lisp_quote);
  LISP_DEFINE_FUNCTION (ctx, "+", nil, &lisp_plus_or_multiply);
  LISP_DEFINE_FUNCTION (ctx, "*", nil, &lisp_plus_or_multiply);
  LISP_DEFINE_FUNCTION (ctx, "-", nil, &lisp_minus);
  LISP_DEFINE_FUNCTION (ctx, "DUMP-CONTEXT-INFO", nil, &lisp_dump_context);
  LISP_DEFINE_FUNCTION (ctx, "PRINT", nil, &lisp_print);
  LISP_DEFINE_FUNCTION (ctx, "IF", nil, &lisp_if);

  LISP_DEFINE_FUNCTION (ctx, ">", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "<", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, ">=", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "<=", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "=", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "!=", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "/", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "%", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "&", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "|", nil, &lisp_binary_op_number);
  LISP_DEFINE_FUNCTION (ctx, "^", nil, &lisp_binary_op_number);

  LISP_DEFINE_FUNCTION (ctx, "LAMBDA", nil, &lisp_lambda);

  LISP_DEFINE_FUNCTION (ctx, "NULL?", nil, &lisp_null_p);
  LISP_DEFINE_FUNCTION (ctx, "CAR", nil, &lisp_get_car);
  LISP_DEFINE_FUNCTION (ctx, "CDR", nil, &lisp_get_cdr);
  LISP_DEFINE_FUNCTION (ctx, "CONS", nil, &lisp_cons);
  LISP_DEFINE_FUNCTION (ctx, "LIST", nil, &lisp_list);
  LISP_DEFINE_FUNCTION (ctx, "ATOM?", nil, &lisp_atom_p);
  LISP_DEFINE_FUNCTION (ctx, "ZERO?", nil, &lisp_zero_p);

  LISP_DEFINE_FUNCTION (ctx, "COND", nil, &lisp_cond);

  lisp_set_value_in_context (ctx, lisp_new_symbol (ctx, "#T"), lisp_true ());
  lisp_set_value_in_context (ctx, lisp_new_symbol (ctx, "#F"), lisp_false ());
  lisp_set_value_in_context (ctx, lisp_new_symbol (ctx, "NIL"), lisp_nil ());

  return ctx;
}

lisp_value_t
lisp_eval (lisp_context_t *ctx, lisp_value_ref_t val)
{
  lisp_value_t r = LISP_NIL;
  if (LISP_IS_LIST (val) && !LISP_IS_NIL (val))
    {
      lisp_value_t args;
      lisp_value_t func_expr = lisp_car (ctx, val);
      lisp_value_t func = lisp_eval (ctx, func_expr);
      struct lisp_function *fn = NULL;

      lisp_free_value (ctx, func_expr);

      if (LISP_IS_EXCEPTION (func))
        {
          return lisp_exception ();
        }

      if (func.tag == LISP_TAG_LAMBDA)
        {
          fn = func.ptr;

          args = lisp_cdr (ctx, val);
          r = fn->invoker (ctx, args, fn);
          lisp_free_value (ctx, args);
          lisp_free_value (ctx, func);
        }

      else if (func.tag == LISP_TAG_FUNCTION_PROXY)
        {
          struct lisp_function_proxy *proxy = func.ptr;
          fn = proxy->func;
          args = lisp_cdr (ctx, val);
          r = fn->invoker (ctx, args, fn);
          lisp_free_value (ctx, args);
          lisp_free_value (ctx, func);
        }
      else
        {
          lisp_throw_internal_error (ctx, "Need a function");
          lisp_free_value (ctx, func);
          return lisp_exception ();
        }

      return r;
    }

  if (val.tag == LISP_TAG_SYMBOL)
    {
      struct lisp_variable_store *var = lisp_context_find_variable (ctx, val);
      lisp_value_t r;
      if (!var)
        return lisp_throw_internal_error (ctx, "Variable %s not found",
                                          LISP_SYMBOL_STR (val));
      r = lisp_dup_value (ctx, var->value);
      lisp_variable_store_unref (ctx, var);
      return r;
    }

  return val;
}
