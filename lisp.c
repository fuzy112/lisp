#include "lisp.h"
#include "dynarray.h"
#include "hashtable.h"
#include "list.h"
#include "string_buf.h"
#include "symbol_enums.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gc.h>

#define LISP_INTERNED_SYM_TABLE_BITS 11
#define LISP_VAR_TABLE_BITS 8

#define LISP_GC_INTERVAL 2

struct lisp_object;

#define LISP_MAKE_PTR(obj) (lisp_value_t) { .ptr = (obj) }

enum lisp_class_id {
  LISP_CLASS_FUNCTION = 1,
  LISP_CLASS_SYNTAX,
  LISP_CLASS_STRING,
  LISP_CLASS_SYMBOL,
  LISP_CLASS_PAIR,
  LISP_CLASS_CONTEXT,
  LISP_CLASS_VECTOR,
};

struct lisp_object_class
{
  int class_id;
  int (*format) (lisp_context_t *ctx, struct lisp_object *,
                 struct string_buf *);
};

int lisp_value_format (lisp_context_t *ctx, lisp_value_ref_t val,
                       struct string_buf *);

struct lisp_object
{
  const struct lisp_object_class *ops;
};

static void
lisp_object_init (struct lisp_object *obj,
                  const struct lisp_object_class *ops)
{
  obj->ops = ops;
}

#define LISP_OBJECT_INIT(type, ops) lisp_object_init (&(type)->obj, ops)

struct lisp_symbol;

struct lisp_runtime
{
  lisp_value_t exception_list;

  DECLARE_HASHTABLE (interned_sym_table, LISP_INTERNED_SYM_TABLE_BITS);
  DECLARE_DYNARRAY (struct lisp_symbol *, interned_sym_array);
};

struct lisp_context
{
  struct lisp_object obj;

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

  void *cfunc;
  int arg_max;

  lisp_value_t name;
  lisp_context_t *ctx;
};

static int lisp_function_format (lisp_context_t *ctx, struct lisp_object *obj,
                                 struct string_buf *buf);


static const struct lisp_object_class lisp_function_operations = {
  .class_id = LISP_CLASS_FUNCTION,
  .format = lisp_function_format,
};

struct lisp_variable
{
  struct hlist_node node;

  lisp_value_t name;
  lisp_value_t value;
};

static lisp_value_t
lisp_nil (void)
{
  lisp_value_t val = LISP_NIL;
  return val;
}

static int lisp_install_default_symbols (lisp_runtime_t *rt);

static void lisp_remove_default_symbols (lisp_runtime_t *rt)
{
  assert (0 && "not implemented");
}

lisp_runtime_t *
lisp_runtime_new (void)
{
  lisp_runtime_t *rt = GC_MALLOC (sizeof (*rt));
  rt->exception_list = lisp_nil ();
  hash_init (rt->interned_sym_table);
  INIT_DYNARRAY (rt, interned_sym_array);
  lisp_install_default_symbols (rt);
  return rt;
}

void
lisp_runtime_free (lisp_runtime_t *rt)
{
  assert (LISP_IS_NIL (rt->exception_list));
  GC_FREE (rt);
}


static const struct lisp_object_class lisp_context_operations = {
  .class_id = LISP_CLASS_CONTEXT,
};

static int
lisp_context_init (struct lisp_context *ctx, lisp_runtime_t *rt,
                   const char *name, lisp_context_t *parent)
{
  ctx->obj.ops = &lisp_context_operations;
  hash_init (ctx->var_table);
  ctx->parent = NULL;
  ctx->runtime = rt;
  ctx->name = lisp_strdup_rt (rt, name);
  return 0;
}

lisp_context_t *
lisp_context_new (lisp_runtime_t *rt, const char *name)
{
  lisp_context_t *ctx = lisp_malloc_rt (rt, sizeof (*ctx));
  if (!ctx)
    return NULL;
  if (lisp_context_init (ctx, rt, name, NULL))
    {
      return NULL;
    }
  return ctx;
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
  assert (ctx);
  return ctx->runtime;
}

void *
lisp_malloc_rt (lisp_runtime_t *rt, size_t size)
{
  void *ptr;
  time_t now = time (NULL);

  ptr = GC_MALLOC (size);
  return ptr;
}

void *
lisp_malloc (lisp_context_t *ctx, size_t size)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  return lisp_malloc_rt (rt, size);
}

char *
lisp_strdup_rt (lisp_runtime_t *rt, char const *s)
{
  char *r = GC_STRDUP (s);
  return r;
}

char *
lisp_strdup (lisp_context_t *ctx, const char *s)
{
  return lisp_strdup_rt (lisp_get_runtime (ctx), s);
}

static void *
lisp_get_object (lisp_context_t *ctx, lisp_value_ref_t val, unsigned tag)
{
  if (LISP_IS_EXCEPTION (val))
    return NULL;

  if (val.ptr == NULL)
    {
      lisp_throw_internal_error (ctx, "Object pointer not set");
      return NULL;
    }

  {
    struct lisp_object *obj = val.ptr;
    if (obj->ops->class_id != tag) {
      lisp_throw_internal_error(ctx, "Object class mismatch");
      return NULL;
    }
  }

  return val.ptr;
}

static bool lisp_is_class (lisp_value_t val, int class_id)
{
  if (val.tag == 0) {
    struct lisp_object *obj = val.ptr;
    if (obj)
      return obj->ops->class_id == class_id;
  }
  return false;
}

#define LISP_IS_LIST(val) (LISP_IS_NIL((val)) || (lisp_is_class((val), LISP_CLASS_PAIR)))
#define LISP_IS_SYMBOL(val) (lisp_is_class((val), LISP_CLASS_SYMBOL))
#define LISP_IS_STRING(val) (lisp_is_class((val), LISP_CLASS_STRING))

struct lisp_cons
{
  struct lisp_object obj;

  lisp_value_t car;
  lisp_value_t cdr;
};

static lisp_value_t
lisp_car (lisp_context_t *ctx, lisp_value_ref_t val)
{
  struct lisp_cons *cons;
  cons = lisp_get_object (ctx, val, LISP_CLASS_PAIR);
  if (!cons)
    return lisp_exception ();
  return cons->car;
}

static lisp_value_t
lisp_cdr (lisp_context_t *ctx, lisp_value_ref_t val)
{
  struct lisp_cons *cons;
  cons = lisp_get_object (ctx, val, LISP_CLASS_PAIR);
  if (!cons)
    return lisp_exception ();
  return cons->cdr;
}

static int
lisp_cons_format (lisp_context_t *ctx, struct lisp_object *obj,
                  struct string_buf *buf)
{
  struct lisp_cons *cons = (struct lisp_cons *)obj;
  int is_first = 1;
  lisp_value_t car;
  lisp_value_t cdr;

  sbprintf (buf, "(");

  for (;;)
    {
      car = cons->car;
      cdr = cons->cdr;

      if (!is_first)
        {
          string_buf_append_char (buf, ' ');
        }
      is_first = 0;
      lisp_value_format (ctx, car, buf);

      if (LISP_IS_NIL (cdr))
        {
          string_buf_append_char (buf, ')');
          return 0;
        }

      if (LISP_IS_LIST (cdr))
        {
          cons = lisp_get_object (ctx, cdr, LISP_CLASS_PAIR);
        }

      else
        {
          sbprintf (buf, " . ");
          lisp_value_format (ctx, cdr, buf);
          string_buf_append_char (buf, ')');
          return 0;
        }
    }
}

static const struct lisp_object_class lisp_cons_operations = {
  .class_id = LISP_CLASS_PAIR,
  .format = lisp_cons_format,
};

static int
lisp_cons_init (lisp_context_t *ctx, struct lisp_cons *cons, lisp_value_t car,
                lisp_value_t cdr)
{
  cons->obj.ops = &lisp_cons_operations;

  cons->car = car;
  cons->cdr = cdr;

  return 0;
}

lisp_value_t
lisp_new_cons (lisp_context_t *ctx, lisp_value_t car, lisp_value_t cdr)
{
  struct lisp_cons *cons;

  if (LISP_IS_EXCEPTION (car) || LISP_IS_EXCEPTION (cdr))
    {
      return lisp_exception ();
    }

  cons = lisp_malloc (ctx, sizeof (*cons));
  if (!cons)
    return lisp_throw_out_of_memory (ctx);

  if (lisp_cons_init (ctx, cons, car, cdr))
    {
      return lisp_exception ();
    }

  return LISP_MAKE_PTR(cons);
}

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

typedef lisp_value_t lisp_syntax_func_t (lisp_context_t *,
                                         lisp_value_ref_t form, int magic,
                                         lisp_value_t *data);

struct lisp_syntax
{
  // GC object header
  struct lisp_object obj;

  // User data
  lisp_value_t *data;

  // Magic number
  int magic;

  // Number of user data
  int n_data;

  // C Function.
  lisp_syntax_func_t *func;
};

static const struct lisp_object_class lisp_syntax_operations = {
  .class_id = LISP_CLASS_SYNTAX,
};

static int
lisp_syntax_init (lisp_context_t *ctx, struct lisp_syntax *syntax,
                  lisp_syntax_func_t func, int magic, lisp_value_t *data,
                  int n)
{
  LISP_OBJECT_INIT (syntax, &lisp_syntax_operations);
  syntax->magic = magic;
  syntax->func = func;
  syntax->data = lisp_malloc (ctx, sizeof (*data) * n);
  memcpy (syntax->data, data, sizeof (*data) * n);
  return 0;
}

lisp_value_t
lisp_new_syntax (lisp_context_t *ctx, lisp_syntax_func_t func, int magic,
                 lisp_value_t *data, int n)
{
  struct lisp_syntax *syntax = lisp_malloc (ctx, sizeof (*syntax));
  if (!syntax)
    goto fail;

  if (lisp_syntax_init (ctx, syntax, func, magic, data, n))
    goto fail;

  return LISP_MAKE_PTR(syntax);

fail:
  return lisp_exception ();
}

struct lisp_symbol
{
  struct lisp_object obj;

  /* node in name hashtable. */
  struct hlist_node snode;

  /* index in symbol table */
  size_t index : sizeof (size_t) - 2;

  /* whether the symbol is static allocated */
  size_t is_static : 1;

  /* pointeer to name of the symbol */
  char *name;

  /* length of the symbol name */
  size_t length;
};

static int
lisp_symbol_format (lisp_context_t *ctx, struct lisp_object *obj,
                    struct string_buf *buf)
{
  struct lisp_symbol *sym = (struct lisp_symbol *)obj;
  // string_buf_append_char (buf, '\'');
  return string_buf_append (buf, sym->name, strlen (sym->name));
}

static const struct lisp_object_class lisp_symbol_operations = {
  .class_id = LISP_CLASS_SYMBOL,
  .format = &lisp_symbol_format,
};

static uint32_t lisp_hash_str (const char *s);

static unsigned streq (const char *, const char *);

lisp_value_t
lisp_interned_symbol (lisp_context_t *ctx, const char *name)
{
  struct lisp_symbol *sym;
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  uint32_t key = lisp_hash_str (name);

  hash_for_each_possible (rt->interned_sym_table, sym, snode, key)
    {
      if (streq (sym->name, name))
        {
          return LISP_MAKE_PTR(sym);
        }
    }

  sym = lisp_malloc (ctx, sizeof (*sym));
  if (!sym)
    return lisp_exception ();

  LISP_OBJECT_INIT (sym, &lisp_symbol_operations);

  sym->name = lisp_toupper (ctx, name);
  if (sym->name == NULL)
    goto fail;
  sym->length = strlen (name);
  sym->index = rt->interned_sym_array_length;
  sym->is_static = 0;
  if (dynarray_add (rt->interned_sym_array, sym))
    goto fail;

  hash_add (rt->interned_sym_table, &sym->snode, key);

  return LISP_MAKE_PTR(sym);

fail:
  return lisp_exception ();
}

#define LISP_SYMBOL_DEF(name_, value)                                         \
  { \
    .obj = { \
      .ops = &lisp_symbol_operations, \
    }, \
    .index = LISP_SYMBOL_##name_,  \
    .is_static = 1, \
    .name = (value),  \
    .length = strlen ((value) ? (value) : ""), \
  },

static struct lisp_symbol lisp_default_symbol_table[] = {
#include "symbols.inc"
};
#undef LISP_SYMBOL_DEF

static int
lisp_install_default_symbols (lisp_runtime_t *rt)
{
  size_t i;

  for (i = 0; i < LISP_SYMBOL__MAX; ++i)
    {
      assert (lisp_default_symbol_table[i].index == i);
      assert (rt->interned_sym_array_length == i);
      dynarray_add (rt->interned_sym_array, &lisp_default_symbol_table[i]);
      hash_add (rt->interned_sym_table, &lisp_default_symbol_table[i].snode,
                lisp_hash_str (lisp_default_symbol_table[i].name));
    }
  return 0;
}

static unsigned
lisp_eqv (lisp_value_ref_t a, lisp_value_ref_t b)
{
  assert (!LISP_IS_EXCEPTION (a));
  assert (!LISP_IS_EXCEPTION (b));
  if (a.tag != b.tag)
    return 0;

  if (LISP_IS_BOOL(a))
    return (a.i && b.i) || (!a.i && !b.i);
  if (LISP_IS_INT(a))
    return a.i == b.i;
  assert (LISP_IS_PTR (a));
  return a.ptr == b.ptr;
}

#define lisp_sym_eq lisp_eqv

struct lisp_string
{
  struct lisp_object obj;
  int is_static : 1;
  char *str;
};

static int
lisp_string_format (lisp_context_t *ctx, struct lisp_object *obj,
                    struct string_buf *buf)
{
  struct lisp_string *str = (struct lisp_string *)obj;
  string_buf_append_char (buf, '"');
  string_buf_append (buf, str->str, strlen (str->str));
  string_buf_append_char (buf, '"');
  return 0;
}

static const struct lisp_object_class lisp_string_operations = {
  .class_id = LISP_CLASS_STRING,
  .format = &lisp_string_format,
};

lisp_value_t
lisp_new_string_full (lisp_context_t *ctx, const char *s, int is_static)
{
  struct lisp_string *str = lisp_malloc (ctx, sizeof (*str));
  lisp_value_t val = LISP_MAKE_PTR(str);
  if (!str)
    return lisp_throw_out_of_memory (ctx);

  LISP_OBJECT_INIT (str, &lisp_string_operations);

  str->is_static = is_static;
  str->str = lisp_strdup (ctx, s);

  if (!str->str)
    {
      return lisp_exception ();
    }

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
  char *s = GC_STRNDUP (n, len);
  if (!s)
    return lisp_throw_out_of_memory (ctx);

  val = lisp_new_string (ctx, s);
  return val;
}

#define LISP_STRING_OBJECT_INIT(name, value)                                  \
  {                                                                           \
    { &lisp_string_operations, },					\
        1, (char *)(value)                                                    \
      }

#define LISP_DEFINE_STRING_OBJECT(name, value)                                \
  struct lisp_string name = LISP_STRING_OBJECT_INIT (name, value)

#define LISP_DEFINE_STRING(name, value)                                       \
  static LISP_DEFINE_STRING_OBJECT (static_string_##name, value);             \
  static lisp_value_t name                                                    \
  = LISP_MAKE_PTR (&static_string_##name);

static size_t
lisp_list_length (lisp_context_t *ctx, lisp_value_ref_t list)
{
  size_t len = 0;
  lisp_value_t head;
  while (!LISP_IS_NIL (list))
    {
      lisp_list_extract (ctx, list, &head, 1, &list);
      ++len;
    }
  return len;
}

lisp_value_t
lisp_throw (lisp_context_t *ctx, lisp_value_t error)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);
  printf("%s: throwing ", ctx->name);
  lisp_print_value (ctx, error);
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

typedef lisp_value_t lisp_cfunc_data (lisp_context_t *ctx, int argc,
                                      lisp_value_ref_t *argv, int magic,
                                      lisp_value_t *data);

typedef lisp_value_t lisp_cfunc_magic (lisp_context_t *ctx, int argc,
                                       lisp_value_ref_t *argv, int magic);

typedef lisp_value_t lisp_cfunc_simple (lisp_context_t *ctx, int argc,
                                        lisp_value_ref_t *argv);

enum lisp_parse_error
{
  LISP_PE_EOF = 1,
  LISP_PE_EARLY_EOF = 2,
  LISP_PE_EXPECT_RIGHT_PAREN = 3,
  LISP_PE_INVALID_NUMBER_LITERAL = 4,
  LISP_PE_INVALID_BOOLEAN_LITERAL = 5,
  LISP_PE_INVALID_TOKEN = 6,
  LISP_PE_INVOKE_ESCAPE_SEQUENCE = 7,
};

static lisp_value_t
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
  rt->exception_list = lisp_cdr (ctx, rt->exception_list);
  return val;
}

void
lisp_print_exception (lisp_context_t *ctx)
{
  // lisp_print_value (ctx, ctx->runtime->exception_list);
  lisp_value_t error = lisp_get_exception (ctx);
  if (LISP_IS_EXCEPTION (error))
    abort ();

  printf ("%s: ", ctx->name);
  lisp_print_value (ctx, error);
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
  return strcasecmp (a, b) == 0;
}

struct lisp_reader
{
  lisp_context_t *ctx;
  FILE *filep;

  int state;
  char *token;

  struct string_buf buf;
};

static int
lisp_reader_getc (struct lisp_reader *reader)
{
  return getc (reader->filep);
}

static int
lisp_reader_ungetc (struct lisp_reader *reader, int ch)
{
  return ungetc (ch, reader->filep);
}

static const char *lisp_next_token (struct lisp_reader *reader);

static const char *lisp_peek_token (struct lisp_reader *reader);

static lisp_value_t lisp_read_atom (struct lisp_reader *reader);

static lisp_value_t lisp_read_list (struct lisp_reader *reader);

void
lisp_reader_init (struct lisp_reader *reader, lisp_context_t *ctx)
{
  reader->filep = NULL;
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
  GC_FREE (reader->buf.s);
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
}

lisp_value_t
lisp_read_form (struct lisp_reader *reader)
{
  const char *token;

  token = lisp_peek_token (reader);
  if (!token)
    return lisp_throw_parse_error (reader->ctx, LISP_PE_EOF, "EOF");

  if (streq (token, "(") || streq (token, "["))
    return lisp_read_list (reader);

  if (streq (token, ")") || streq (token, "]"))
    return lisp_throw_parse_error (reader->ctx, LISP_PE_EXPECT_RIGHT_PAREN,
                                   "Unexpected '%s'", token);

  if (streq (token, "'"))
    {
      lisp_next_token (reader);
      lisp_value_t quoted = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (quoted))
        return lisp_exception ();
      return lisp_new_cons (reader->ctx,
                            lisp_interned_symbol (reader->ctx, "QUOTE"),
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
  const char *closing;

  token = lisp_next_token (reader);
  assert (streq (token, "(") || streq (token, "["));

  if (streq (token, "("))
    closing = ")";
  else
    closing = "]";

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

      if (streq (token, closing))
        break;

      if (streq (token, "."))
        {
          lisp_next_token (reader);
          *tail = lisp_read_form (reader);
          if (LISP_IS_EXCEPTION (*tail))
            goto fail;
          token = lisp_next_token (reader);
          if (!streq (token, closing))
            {
              lisp_throw_parse_error (reader->ctx, LISP_PE_EXPECT_RIGHT_PAREN,
                                      "expected '%s' but got '%s'", closing,
                                      token);
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
  assert (!streq (token, "(") && !streq (token, "["));
  assert (!streq (token, ")") && !streq (token, "]"));

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
          //return lisp_new_real (reader->ctx, n);
	  assert(0);
	 
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

  return lisp_interned_symbol (reader->ctx, token);
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

  while ((ch = lisp_reader_getc (reader)) != EOF)
    {
      if (!isspace (ch))
        {
          if (ch == ';')
            {
              while ((ch = lisp_reader_getc (reader)) != EOF)
                if (ch == '\n' || ch == '\r' || ch == '\f')
                  break;
            }
          else
            break;
        }
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

    case '[':
      reader->token = "[";
      return 0;

    case ']':
      reader->token = "]";
      return 0;

    case '\'':
      reader->token = "'";
      return 0;

    case '.':
      reader->token = ".";
      return 0;

    case '0' ... '9':
      string_buf_append_char (&reader->buf, ch);
      while ((ch = lisp_reader_getc (reader)) != EOF)
        if (isdigit (ch))
          string_buf_append_char (&reader->buf, ch);
        else if (isspace (ch))
          break;
        else if (strchr ("()[]{};'`\"|", ch))
          {
            lisp_reader_ungetc (reader, ch);
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
      while ((ch = lisp_reader_getc (reader)) != EOF)
        {
          if (isalnum (ch) || !!strchr ("+-*/%^><=!&?", ch))
            string_buf_append_char (&reader->buf, ch);
          else if (isspace (ch))
            break;
          else if (strchr ("()[]{};'`\"|", ch))
            {
              lisp_reader_ungetc (reader, ch);
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

    case '"':
      string_buf_append_char (&reader->buf, '"');

      while ((ch = lisp_reader_getc (reader)) != EOF)
        {
          if (ch == '\\')
            {
              ch = lisp_reader_getc (reader);
              if (ch == EOF)
                break;
              if (ch == 't')
                string_buf_append_char (&reader->buf, '\t');
              else if (ch == 'f')
                string_buf_append_char (&reader->buf, '\f');
              else if (ch == '\\')
                string_buf_append_char (&reader->buf, '\\');
              else if (ch == 'n')
                string_buf_append_char (&reader->buf, '\n');
              else if (ch == 'r')
                string_buf_append_char (&reader->buf, '\r');
              else if (ch == '"')
                string_buf_append_char (&reader->buf, '"');
              else
                {
                  reader->token = NULL;
                  return LISP_PE_INVOKE_ESCAPE_SEQUENCE;
                }
            }
          else if (ch == '"')
            {
              string_buf_append_char (&reader->buf, '"');

              reader->token = reader->buf.s;
              return 0;
            }
          else
            {
              string_buf_append_char (&reader->buf, ch);
            }
        }

      reader->token = NULL;
      return LISP_PE_EARLY_EOF;

    default:
      fprintf (stderr, "invalid char: %o (%c)\n", ch, ch);
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
      // printf ("token : %s\n", reader->token);
      return reader->token;
    }
}

static const char *
lisp_peek_token (struct lisp_reader *reader)
{
  if (reader->state == LISP_RS_NEXT)
    {
      lisp_do_next_token (reader);
      // printf ("token : %s\n", reader->token);

      reader->state = LISP_RS_PEEK;
      return reader->token;
    }
  else
    {
      return reader->token;
    }
}

int
lisp_value_format (lisp_context_t *ctx, lisp_value_ref_t val,
                   struct string_buf *buf)
{
  if (LISP_IS_NIL (val))
    return sbprintf (buf, "()");

  if (LISP_IS_PTR (val))
    {
      struct lisp_object *obj = val.ptr;
      if (obj->ops->format)
        {
          return obj->ops->format (ctx, obj, buf);
        }
      else
        {
          return sbprintf (buf, "#OBJECT");
        }
    }

  if (LISP_IS_INT(val))
    {
      return sbprintf (buf, "%" PRIi32, val.i);
    }

  if (LISP_IS_BOOL(val))
    {
      if (val.i)
        {
          return sbprintf (buf, "#T");
        }
      else
        {
          return sbprintf (buf, "#F");
        }
    }

  assert (0 && "unreachable");
}

char *
lisp_value_to_string (lisp_context_t *ctx, lisp_value_ref_t val)
{
  char *rv;
  struct string_buf sb;

  string_buf_init (&sb);
  if (lisp_value_format (ctx, val, &sb))
    {
      string_buf_destroy (&sb);
      return NULL;
    }

  rv = lisp_strdup (ctx, sb.s);
  string_buf_destroy (&sb);

  return rv;
}

void
lisp_print_value (lisp_context_t *ctx, lisp_value_ref_t val)
{
  char *s = lisp_value_to_string (ctx, val);
  puts (s);
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
  if (LISP_IS_INT(val))
    *result = val.i;
  else
    {
      lisp_throw_internal_error (ctx, "Value error: %i", val.tag);
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
      val = (val + toupper (s[i]) * p_pow) % m;
      p_pow = p_pow * p % m;
    }

  return val;
}

static lisp_value_t
lisp_context_get_var (lisp_context_t *ctx, lisp_value_ref_t name)
{
  uint32_t key;
  struct lisp_variable *var;
  lisp_context_t *orig_ctx = ctx;
  if (LISP_IS_EXCEPTION (name))
    return lisp_exception ();
  if (!LISP_IS_SYMBOL (name))
    return lisp_throw_internal_error (ctx, "type error");

  key = lisp_hash_str (LISP_SYMBOL_STR (name));
  while (ctx != NULL)
    {
      hash_for_each_possible (ctx->var_table, var, node, key)
        {
          if (lisp_sym_eq (name, var->name))
            return var->value;
        }

      ctx = ctx->parent;
    }

  return lisp_throw_internal_error (orig_ctx, "no such variable: %s",
                                    LISP_SYMBOL_STR (name));
}

static lisp_value_t
lisp_context_set_var (lisp_context_t *ctx, lisp_value_ref_t name,
                      lisp_value_t value)
{
  uint32_t key;
  struct lisp_variable *var;
  lisp_context_t *orig_ctx = ctx;
  if (LISP_IS_EXCEPTION (name))
    {
      return lisp_exception ();
    }
  if (!LISP_IS_SYMBOL (name))
    {
      return lisp_throw_internal_error (ctx, "type error");
    }

  key = lisp_hash_str (LISP_SYMBOL_STR (name));
  while (ctx != NULL)
    {
      hash_for_each_possible (ctx->var_table, var, node, key)
        {
          if (lisp_sym_eq (name, var->name))
            {
              var->value = value;

              return lisp_nil ();
            }
        }

      ctx = ctx->parent;
    }

  return lisp_throw_internal_error (orig_ctx, "no such variable");
}

static int
lisp_context_define_var (lisp_context_t *ctx, lisp_value_t name,
                         lisp_value_t value)
{
  struct lisp_variable *var;
  uint32_t key;

  if (LISP_IS_EXCEPTION (name) || LISP_IS_EXCEPTION (value))
    goto fail;

  if (!LISP_IS_SYMBOL (name))
    {
      lisp_throw_internal_error (ctx, "name is not symbol");
      goto fail;
    }

  var = lisp_malloc (ctx, sizeof (*var));
  if (!var)
    goto fail;

  var->name = name;
  var->value = value;

  key = lisp_hash_str (LISP_SYMBOL_STR (name));

  hash_add (ctx->var_table, &var->node, key);

  return 0;

fail:
  return -1;
}

static int
lisp_function_set_args (lisp_context_t *ctx, lisp_context_t *new_ctx,
                        lisp_value_ref_t params, lisp_value_ref_t args)
{
  lisp_value_t name;
  lisp_value_t expr;
  lisp_value_t value;
  while (!LISP_IS_NIL (params))
    {
      if (LISP_IS_SYMBOL (params))
        {
          lisp_value_t *tail = &value;
          name = params;

          while (!LISP_IS_NIL (args))
            {
              expr = lisp_car (ctx, args);
              *tail = lisp_new_cons (ctx, lisp_eval (ctx, expr), lisp_nil ());
              tail = &((struct lisp_cons *)tail->ptr)->cdr;
              args = lisp_cdr (ctx, args);
            }

          if (lisp_context_define_var (new_ctx, name, value))
            {
              return -1;
            }
          return 0;
        }
      else
        {
          name = lisp_car (ctx, params);
          expr = lisp_car (ctx, args);
          value = lisp_eval (ctx, expr);
        }

      if (lisp_context_define_var (new_ctx, name, value))
        {
          return -1;
        }
      params = lisp_cdr (ctx, params);
      args = lisp_cdr (ctx, args);
    }

  return 0;
}

static lisp_value_t
lisp_eval_list (lisp_context_t *ctx, lisp_value_ref_t list)
{
  lisp_value_t val = LISP_NIL;
   while (!LISP_IS_NIL (list) && !LISP_IS_EXCEPTION (val))
    {
      lisp_value_t exp = lisp_car (ctx, list);
      list = lisp_cdr (ctx, list);
      val = lisp_eval (ctx, exp);
    }
  return val;
}

static lisp_value_t
lisp_function_invoker (lisp_context_t *ctx, lisp_value_ref_t args,
                       struct lisp_function *func)
{
  lisp_value_t val = lisp_exception ();
  lisp_context_t *new_ctx
      = lisp_context_new_with_parent (func->ctx, LISP_SYMBOL_STR (func->name));
  if (new_ctx)
    {
      if (!lisp_function_set_args (ctx, new_ctx, func->params, args))
        val = lisp_eval_list (new_ctx, func->body);
    }
  return val;
}

static lisp_value_t *
lisp_eval_args (lisp_context_t *ctx, lisp_value_ref_t args, int max_arg,
                int *n_args)
{
  size_t length;
  size_t argc = lisp_list_length (ctx, args);
  lisp_value_t *arr;
  size_t i;

  if (max_arg == -1)
    length = argc;
  else if ((size_t)max_arg >= argc)
    length = max_arg;
  else
    {
      lisp_throw_internal_error (ctx, "too many arguments");
      return NULL;
    }

  arr = lisp_malloc (ctx, sizeof (lisp_value_t) * length);
  if (!arr)
    return NULL;

  if (lisp_list_extract (ctx, args, arr, argc, NULL))
    {
      return NULL;
    }
  for (i = 0; i < argc; ++i)
    {
      arr[i] = lisp_eval (ctx, arr[i]);
      if (LISP_IS_EXCEPTION (arr[i]))
        {
          return NULL;
        }
    }

  *n_args = (int)argc;
  return arr;
}

static lisp_value_t
lisp_cfunc_invoker (lisp_context_t *ctx, lisp_value_ref_t args,
                    struct lisp_function *func)
{
  int argc = -1;
  lisp_value_t *argv;
  lisp_value_t val;
  lisp_context_t *tmp_ctx;

  lisp_cfunc_simple *cfunc = func->cfunc;

  argv = lisp_eval_args (ctx, args, func->arg_max, &argc);
  if (!argv)
    return lisp_exception ();

  tmp_ctx = lisp_context_new_with_parent (ctx, "#CFUNC");
  if (tmp_ctx)
    {
      val = cfunc (tmp_ctx, argc, argv);
    }
  else
    {
      val = lisp_exception ();
    }
  return val;
}

static lisp_value_t
lisp_new_function (lisp_context_t *ctx, lisp_value_t name, lisp_value_t params,
                   lisp_value_t body, lisp_native_invoke_t invoker)
{
  struct lisp_function *fn = lisp_malloc (ctx, sizeof (*fn));
  lisp_value_t val = LISP_MAKE_PTR(fn);

  if (!fn)
    return lisp_exception ();

  LISP_OBJECT_INIT (fn, &lisp_function_operations);

  fn->params = params;
  fn->body = body;
  fn->invoker = invoker;
  fn->name = name;

  fn->ctx = lisp_context_new_with_parent (ctx, LISP_SYMBOL_STR (name));
  if (!fn->ctx)
    {
      return lisp_exception ();
    }

  return val;
}

static lisp_value_t
lisp_new_cfunc (lisp_context_t *ctx, const char *name,
                lisp_cfunc_simple *cfunc, int n)
{
  lisp_value_t func = lisp_new_function (ctx, lisp_interned_symbol (ctx, name),
                                         lisp_interned_symbol (ctx, name),
                                         lisp_nil (), lisp_cfunc_invoker);
  if (LISP_IS_EXCEPTION (func))
    return lisp_exception ();

  {
    struct lisp_function *fn = lisp_get_object (ctx, func, LISP_CLASS_FUNCTION);
    fn->arg_max = n;
    fn->cfunc = cfunc;
  }

  return func;
}

lisp_value_t
lisp_new_int32 (lisp_context_t *ctx, int32_t v)
{
  lisp_value_t val = LISP_INT32 (v);
  (void)ctx;
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

int
lisp_to_bool (lisp_context_t *ctx, lisp_value_ref_t val, int *res)
{
  if (LISP_IS_EXCEPTION (val))
    return -1;

  if (!LISP_IS_BOOL(val))
    {
      lisp_throw_internal_error (ctx, "Expected a boolean");
      return -1;
    }

  *res = val.i;
  return 0;
}

/***************************************************************************************
 *  Special forms
 ***************************************************************************************/

/**
 * (define var value)
 * (define (func params) body)
 */
static lisp_value_t
lisp_define (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
             lisp_value_t *data)
{
  lisp_value_t sig = lisp_car (ctx, args);

  if (LISP_IS_LIST(sig))
    { // defining function
      lisp_value_t name = lisp_car (ctx, sig);
      lisp_value_t params = lisp_cdr (ctx, sig);
      lisp_value_t body = lisp_cdr (ctx, args);
      lisp_value_t func = lisp_new_function (ctx, name, params, body,
                                             &lisp_function_invoker);

      // lisp_set_function_name (ctx, func, lisp_dup_value (ctx, name));
      if (lisp_context_define_var (ctx, name, func))
        return lisp_exception ();

      return lisp_nil ();
    }
  else if (LISP_IS_SYMBOL(sig))
    { // define variable
      lisp_value_t tmp = lisp_cdr (ctx, args);
      lisp_value_t expr = lisp_car (ctx, tmp);
      lisp_value_t value = lisp_eval (ctx, expr);
      if (lisp_context_define_var (ctx, sig, value))
        return lisp_exception ();
      return lisp_nil ();
    }

  return lisp_throw_internal_error (ctx, "Invalid syntax");
}

/**
 * (set! var value)
 */
static lisp_value_t
lisp_set_ (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
           lisp_value_t *data)
{
  lisp_value_t val[2];

  if (lisp_list_extract (ctx, args, val, 2, NULL))
    return lisp_exception ();

  val[1] = lisp_eval (ctx, val[1]);

  return lisp_context_set_var (ctx, val[0], val[1]);
}

enum
{
  LISP_MAGIC_LET,
  LISP_MAGIC_LETREC,
  LISP_MAGIC_LETSTAR,
};

/**
 * (let* ((a b)
 *       (c d))
 *    body...)
 */
static lisp_value_t
lisp_let (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
          lisp_value_t *data)
{
  lisp_value_t variables;
  lisp_value_t binding;
  lisp_value_t body;
  lisp_context_t *new_ctx;
  lisp_value_t ret = LISP_EXCEPTION;

  if (lisp_list_extract (ctx, args, &variables, 1, &body))
    return lisp_exception ();

  new_ctx = lisp_context_new_with_parent (ctx, "LET");
  if (!new_ctx)
    return lisp_exception ();

  while (!LISP_IS_NIL (variables))
    {
      lisp_value_t val[2];

      if (lisp_list_extract (ctx, variables, &binding, 1, &variables))
        goto fail;

      if (lisp_list_extract (ctx, binding, val, 2, NULL))
        goto fail;

      if (magic == LISP_MAGIC_LETSTAR)
        {
          lisp_context_t *tmp
              = lisp_context_new_with_parent (new_ctx, "#LET*");
          new_ctx = tmp;
        }

      if (magic == LISP_MAGIC_LET || magic == LISP_MAGIC_LETREC)
        val[1] = lisp_eval (new_ctx, val[1]);
      else
        val[1] = lisp_eval (ctx, val[1]);

      if (lisp_context_define_var (new_ctx, val[0], val[1]))
        goto fail;
    }

  ret = lisp_eval_list (new_ctx, body);

fail:
  return ret;
}

/**
 * (quote exp)
 */
static lisp_value_t
lisp_quote (lisp_context_t *ctx, lisp_value_ref_t list, int magic,
            lisp_value_t *data)
{
  return lisp_car (ctx, list);
}

static lisp_value_t
lisp_if (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
         lisp_value_t *data)
{
  lisp_value_t vals[2];
  lisp_value_t tail;
  lisp_value_t cond_v;
  int cond;

  if (lisp_list_extract (ctx, args, vals, 2, &tail))
    return lisp_exception ();

  cond_v = lisp_eval (ctx, vals[0]);

  if (lisp_to_bool (ctx, cond_v, &cond))
    {
      return lisp_exception ();
    }

  if (cond)
    {
      return lisp_eval (ctx, vals[1]);
    }
  else
    {
      return lisp_eval_list (ctx, tail);
    }
}

int
lisp_list_extract (lisp_context_t *ctx, lisp_value_ref_t list,
                   lisp_value_ref_t *heads, int n, lisp_value_ref_t *tail)
{

  int i;
  int res = -1;

  lisp_value_t tmp;

  if (LISP_IS_EXCEPTION (list))
    return -1;

  if (!tail)
    tail = &tmp;

  *tail = list;

  for (i = 0; i < n; ++i)
    {
      heads[i] = lisp_car (ctx, *tail);
      if (LISP_IS_EXCEPTION (heads[i]))
        goto fail;

      *tail = lisp_cdr (ctx, *tail);
    }

  res = 0;

fail:
  return res;
}

/**
 * (cond (CONDTION BODY...) ...)
 */
static lisp_value_t
lisp_cond (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
           lisp_value_t *data)
{
  lisp_value_ref_t clause;

  while (!LISP_IS_NIL (args))
    {
      int value;
      lisp_value_t condition_value;
      lisp_value_ref_t condition, body;
      if (lisp_list_extract (ctx, args, &clause, 1, &args))
        return lisp_exception ();

      if (lisp_list_extract (ctx, clause, &condition, 1, &body))
        return lisp_exception ();

      if (LISP_IS_SYMBOL (condition)
          && lisp_sym_eq (condition, lisp_interned_symbol (ctx, "ELSE")))
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
          return lisp_exception ();
        }
      if (value)
        {
          return lisp_eval_list (ctx, body);
        }
    }

  return lisp_nil ();
}

/**
 * (named-lambda (name params...) body)
 */
static lisp_value_t
lisp_named_lambda (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
                   lisp_value_t *data)
{
  lisp_value_t params;
  lisp_value_t body;
  lisp_value_t val;

  params = lisp_car (ctx, args);
  body = lisp_cdr (ctx, args);

  val = lisp_new_function (ctx, lisp_car (ctx, params),
                           lisp_cdr (ctx, params), body,
                           lisp_function_invoker);

  return val;
}

/**
 * (lambda (name params...) body)
 */
static lisp_value_t
lisp_lambda (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
             lisp_value_t *data)
{
  lisp_value_t params;
  lisp_value_t body;
  lisp_value_t val;

  params = lisp_car (ctx, args);
  body = lisp_cdr (ctx, args);

  val = lisp_new_function (ctx, lisp_interned_symbol (ctx, "#[lambda]"),
                           params, body, lisp_function_invoker);

  return val;
}

static lisp_value_t
lisp_begin (lisp_context_t *ctx, lisp_value_ref_t args, int magic,
            lisp_value_t *data)
{
  return lisp_eval_list (ctx, args);
}

/********************************************************************************
 *  Functions
 *******************************************************************************/

// static lisp_value_t
// lisp_eqv_p (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
// {

// }

static lisp_value_t
lisp_gc_ (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  GC_collect_a_little();
  return lisp_nil ();
}

static lisp_value_t
lisp_eval_ (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  return lisp_eval (ctx, argv[0]);
}

static lisp_value_t
lisp_dump_runtime (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  lisp_runtime_t *rt = lisp_get_runtime (ctx);

  return lisp_nil ();
}

// static lisp_value_t
// lisp_read (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
// {
// }

/**
 * Vector
 */
struct lisp_vector
{
  struct lisp_object obj;

  size_t capacity;
  size_t length;

  lisp_value_t *data;
};

static const struct lisp_object_class lisp_vector_operations = {
  .class_id = LISP_CLASS_VECTOR,
};

static lisp_value_t
lisp_new_vector (lisp_context_t *ctx, int n, lisp_value_ref_t *elems)
{
  struct lisp_vector *vec;
  int i;

  vec = lisp_malloc (ctx, sizeof (*vec));
  if (!vec)
    return lisp_exception ();

  LISP_OBJECT_INIT (vec, &lisp_vector_operations);

  vec->capacity = n;
  vec->length = n;
  vec->data = lisp_malloc (ctx, sizeof (lisp_value_t) * n);
  if (vec->data == NULL)
    {
      return lisp_exception ();
    }

  for (i = 0; i < n; ++i)
    {
      vec->data[i] = elems[i];
    }

  return LISP_MAKE_PTR(vec);
}

static lisp_value_t
lisp_make_vector (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  struct lisp_vector *vec;
  int32_t k;
  lisp_value_t o;

  if (argc < 1)
    return lisp_throw_internal_error (ctx, "require at least one argument");

  if (lisp_to_int32 (ctx, &k, argv[0]))
    return lisp_exception ();

  o = argv[1];

  vec = lisp_malloc (ctx, sizeof (*vec));
  if (!vec)
    return lisp_exception ();

  LISP_OBJECT_INIT (vec, &lisp_vector_operations);

  vec->length = k;
  vec->capacity = k;
  vec->data = lisp_malloc (ctx, sizeof (o) * k);
  if (!vec->data)
    {
      return lisp_exception ();
    }

  while (k > 0)
    {
      vec->data[--k] = o;
    }

  return LISP_MAKE_PTR(vec);
}

static lisp_value_t
lisp_vector_copy (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (ctx, argv[0], LISP_CLASS_VECTOR);
  if (!vec)
    return lisp_exception ();

  return lisp_new_vector (ctx, vec->length, vec->data);
}

static lisp_value_t
lisp_vector_length (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (ctx, argv[0], LISP_CLASS_VECTOR);

  return lisp_new_int32 (ctx, vec->length);
}

static lisp_value_t
lisp_vector_capacity (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (ctx, argv[0], LISP_CLASS_VECTOR);

  return lisp_new_int32 (ctx, vec->capacity);
}

/**
 * (vector-ref my-vec 1)
 */
static lisp_value_t
lisp_vector_ref (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (ctx, argv[0], LISP_CLASS_VECTOR);
  int32_t pos = -1;

  if (lisp_to_int32 (ctx, &pos, argv[1]))
    return lisp_exception ();

  if (pos < 0 || (size_t)pos >= vec->length)
    return lisp_throw_internal_error (ctx, "Out of range");

  return vec->data[pos];
}

/**
 * (vector-set! my-vec 2 elem)
 */
static lisp_value_t
lisp_vector_set (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (ctx, argv[0], LISP_CLASS_VECTOR);
  int32_t pos = -1;

  if (lisp_to_int32 (ctx, &pos, argv[1]))
    return lisp_exception ();

  if (pos < 0 || (size_t)pos >= vec->length)
    return lisp_throw_internal_error (ctx, "Out of range");

  {
    lisp_value_t tmp = vec->data[pos];
    vec->data[pos] = argv[2];
  }

  return lisp_nil ();
}

static lisp_value_t
lisp_display(lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  int i;
  char *s;
  bool need_delim = false;
  
  for (i = 0; i < argc; ++i) {
    s = lisp_value_to_string(ctx, argv[i]);
    
    if (need_delim) {
      printf(" ");
    }
    need_delim = true;
    printf("%s", s);
  }

  return lisp_nil();
}

static lisp_value_t
lisp_less (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  int i;

  for (i = 0; i + 1 < argc; ++i) {
    int32_t v1 = -1;
    int32_t v2 = -2;

    if (lisp_to_int32(ctx, &v1, argv[i]) ||
	lisp_to_int32(ctx, &v2, argv[i + 1]))
      return lisp_exception();
    if (!(v1 < v2))
      return lisp_false();
  }

  return lisp_true();
}

lisp_value_t
lisp_sum (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  int sum = 0;
  int i;

  for (i = 0; i < argc; ++i) {
    int32_t v=0;
    if (lisp_to_int32(ctx, &v, argv[i]))
      return lisp_exception();
    sum += v;
  }

  return lisp_new_int32(ctx, sum);
}

lisp_value_t
lisp_subtract (lisp_context_t *ctx, int argc, lisp_value_ref_t *argv)
{
  int result = 0;
  int i;

  
  if (argc == 0)
    return lisp_new_int32(ctx, 0);

  if (lisp_to_int32(ctx, &result, argv[0]))
    return lisp_exception();

  if (argc == 1)
    return lisp_new_int32(ctx, -result);
  
  for (i = 1; i < argc; ++i) {
    int v = 0;
    if (lisp_to_int32(ctx, &v, argv[i]))
      return lisp_exception();
    result -= v;
  }

  return lisp_new_int32(ctx, result);
}

static int
lisp_define_cfunc (lisp_context_t *ctx, lisp_value_t name,
                   lisp_cfunc_simple *cfunc, int n)
{
  return lisp_context_define_var (
      ctx, name, lisp_new_cfunc (ctx, LISP_SYMBOL_STR (name), cfunc, n));
}

#define LISP_DEFINE_CFUNC(context, name, cfunc, n)                            \
  lisp_define_cfunc (context, lisp_interned_symbol (ctx, name), cfunc, n)

static int
lisp_define_syntax (lisp_context_t *ctx, lisp_value_t name,
                    lisp_syntax_func_t func, int magic, lisp_value_t *data,
                    int n)
{
  return lisp_context_define_var (ctx, name,
                                  lisp_new_syntax (ctx, func, magic, data, n));
}

#define LISP_DEFINE_MACRO(context, name, func, magic)                         \
  lisp_define_syntax (context, lisp_interned_symbol (context, name), func,    \
                      magic, NULL, 0)

lisp_context_t *
lisp_new_global_context (lisp_runtime_t *rt)
{
  lisp_context_t *ctx = lisp_context_new (rt, "<GLOBAL>");
  lisp_context_t *r;

  LISP_DEFINE_MACRO (ctx, "BEGIN", lisp_begin, 0);
  LISP_DEFINE_MACRO (ctx, "COND", lisp_cond, 0);
  LISP_DEFINE_MACRO (ctx, "DEFINE", lisp_define, 0);
  LISP_DEFINE_MACRO (ctx, "IF", lisp_if, 0);
  LISP_DEFINE_MACRO (ctx, "NAMED-LAMBDA", lisp_named_lambda, 0);
  LISP_DEFINE_MACRO (ctx, "LAMBDA", lisp_lambda, 0);
  LISP_DEFINE_MACRO (ctx, "LET", lisp_let, LISP_MAGIC_LET);
  LISP_DEFINE_MACRO (ctx, "LET*", lisp_let, LISP_MAGIC_LETSTAR);
  LISP_DEFINE_MACRO (ctx, "LETREC", lisp_let, LISP_MAGIC_LETREC);
  LISP_DEFINE_MACRO (ctx, "QUOTE", lisp_quote, 0);
  LISP_DEFINE_MACRO (ctx, "SET!", lisp_set_, 0);

  LISP_DEFINE_CFUNC (ctx, "EVAL", lisp_eval_, 1);

  LISP_DEFINE_CFUNC (ctx, "MAKE-VECTOR", lisp_make_vector, 2);
  LISP_DEFINE_CFUNC (ctx, "VECTOR", lisp_new_vector, -1);
  LISP_DEFINE_CFUNC (ctx, "VECTOR-COPY", lisp_vector_copy, 1);
  LISP_DEFINE_CFUNC (ctx, "VECTOR-LENGTH", lisp_vector_length, 1);
  LISP_DEFINE_CFUNC (ctx, "VECTOR-CAPACITY", lisp_vector_capacity, 1);
  LISP_DEFINE_CFUNC (ctx, "VECTOR-REF", lisp_vector_ref, 2);
  LISP_DEFINE_CFUNC (ctx, "VECTOR-SET!", lisp_vector_set, 3);

  lisp_context_define_var (ctx, lisp_interned_symbol (ctx, "#T"),
                           lisp_true ());
  lisp_context_define_var (ctx, lisp_interned_symbol (ctx, "#F"),
                           lisp_false ());
  lisp_context_define_var (ctx, lisp_interned_symbol (ctx, "NIL"),
                           lisp_nil ());

  LISP_DEFINE_CFUNC(ctx, "+", lisp_sum, -1);
  LISP_DEFINE_CFUNC(ctx, "-", lisp_subtract, -1);
  LISP_DEFINE_CFUNC(ctx, "<", lisp_less, -1);
  LISP_DEFINE_CFUNC(ctx, "DISPLAY", lisp_display, -1);
  
  LISP_DEFINE_CFUNC (ctx, "DUMP-RUNTIME", lisp_dump_runtime, 0);

  LISP_DEFINE_CFUNC (ctx, "GC", lisp_gc_, 0);
  
  r = lisp_context_new_with_parent (ctx, "TOP-LEVEL");

  return r;
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

      if (LISP_IS_EXCEPTION (func))
        {
          return lisp_exception ();
        }

      if (lisp_is_class(func, LISP_CLASS_FUNCTION))
        {

          struct lisp_function *fn
              = lisp_get_object (ctx, func, LISP_CLASS_FUNCTION);

          args = lisp_cdr (ctx, val);
          r = fn->invoker (ctx, args, fn);
        }

      else if (lisp_is_class (func, LISP_CLASS_SYNTAX))
        {
          struct lisp_syntax *syntax
              = lisp_get_object (ctx, func, LISP_CLASS_SYNTAX);
          args = lisp_cdr (ctx, val);
          r = syntax->func (ctx, args, syntax->magic, syntax->data);
        }

      else
        {
          lisp_throw_internal_error (ctx, "Need a function");
          return lisp_exception ();
        }

      return r;
    }

  if (LISP_IS_SYMBOL(val))
    {
      return lisp_context_get_var (ctx, val);
    }

  return val;
}

static int
lisp_function_format (lisp_context_t *ctx, struct lisp_object *obj,
                      struct string_buf *buf)
{
  struct lisp_function *func = (struct lisp_function *)obj;
  sbprintf (buf, "[Function %s]", LISP_SYMBOL_STR (func->name));
  return 0;
}


