#include "lisp.h"
#include "dynarray.h"
#include "hashtable.h"
#include "list.h"
#include "rbtree.h"
#include "string_buf.h"
#include "symbol_enums.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <gc.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LISP_INTERNED_SYM_TABLE_BITS 11
#define LISP_GC_INTERVAL 2

struct lisp_object;

#define LISP_MAKE_PTR(obj)                                                    \
  (lisp_value_t) { .ptr = (obj) }

enum lisp_class_id
{
  LISP_CLASS_PROCEDURE = 1,
  LISP_CLASS_SYNTAX,
  LISP_CLASS_STRING,
  LISP_CLASS_SYMBOL,
  LISP_CLASS_PAIR,
  LISP_CLASS_ENV,
  LISP_CLASS_VECTOR,
};

struct lisp_object_class
{
  uintptr_t class_id;
  int (*format) (lisp_env_t *env, struct lisp_object *, struct string_buf *);
};

int lisp_value_format (lisp_env_t *env, lisp_value_t val, struct string_buf *);

struct lisp_object
{
  const struct lisp_object_class *ops;
};

static void
lisp_object_init (struct lisp_object *obj, const struct lisp_object_class *ops)
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

struct lisp_env
{
  struct lisp_object obj;

  char *name;
  struct lisp_env *parent;
  struct lisp_runtime *runtime;

  struct rb_root var_map;
};

struct lisp_procedure;

typedef lisp_value_t (*lisp_native_invoke_t) (lisp_env_t *env,
                                              lisp_value_t args,
                                              struct lisp_procedure *proc);

struct lisp_procedure
{
  struct lisp_object obj;
  lisp_value_t params;
  lisp_value_t body;
  lisp_native_invoke_t invoker;

  void *native_procedure;
  int arg_max;

  lisp_value_t name;
  lisp_env_t *env;
};

static int lisp_procedure_format (lisp_env_t *env, struct lisp_object *obj,
                                  struct string_buf *buf);

static const struct lisp_object_class lisp_procedure_class = {
  .class_id = LISP_CLASS_PROCEDURE,
  .format = lisp_procedure_format,
};

struct lisp_variable
{
  struct hlist_node node;
  struct rb_node rb;

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

lisp_runtime_t *
lisp_runtime_new (void)
{
  lisp_runtime_t *rt = GC_NEW (lisp_runtime_t);
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

static const struct lisp_object_class lisp_env_class = {
  .class_id = LISP_CLASS_ENV,
};

static int
lisp_env_init (struct lisp_env *env, lisp_runtime_t *rt, const char *name,
               lisp_env_t *parent)
{
  env->obj.ops = &lisp_env_class;
  rb_root_init (&env->var_map);
  env->parent = NULL;
  env->runtime = rt;
  env->name = lisp_strdup_rt (rt, name);
  return 0;
}

lisp_env_t *
lisp_env_new (lisp_runtime_t *rt, const char *name)
{
  lisp_env_t *env = lisp_malloc_rt (rt, sizeof (*env));
  if (!env)
    return NULL;
  if (lisp_env_init (env, rt, name, NULL))
    {
      return NULL;
    }
  return env;
}

lisp_value_t
lisp_exception (void)
{
  lisp_value_t val = LISP_EXCEPTION;
  return val;
}

lisp_value_t
lisp_void (void)
{
  return LISP_VOID;
}

lisp_runtime_t *
lisp_get_runtime (lisp_env_t *env)
{
  assert (env);
  return env->runtime;
}

void *
lisp_malloc_rt (lisp_runtime_t *rt, size_t size)
{
  return GC_MALLOC (size);
}

void *
lisp_malloc (lisp_env_t *env, size_t size)
{
  lisp_runtime_t *rt = lisp_get_runtime (env);
  return lisp_malloc_rt (rt, size);
}

char *
lisp_strdup_rt (lisp_runtime_t *rt, char const *s)
{
  char *r = GC_STRDUP (s);
  return r;
}

char *
lisp_strdup (lisp_env_t *env, const char *s)
{
  return lisp_strdup_rt (lisp_get_runtime (env), s);
}

static void *
lisp_get_object (lisp_env_t *env, lisp_value_t val, unsigned tag)
{
  if (LISP_IS_EXCEPTION (val))
    return NULL;

  if (val.ptr == NULL)
    {
      lisp_throw_internal_error (env, "Object pointer not set");
      return NULL;
    }

  {
    struct lisp_object *obj = val.ptr;
    if (obj->ops->class_id != tag)
      {
        lisp_throw_internal_error (env, "Object class mismatch");
        return NULL;
      }
  }

  return val.ptr;
}

static bool
lisp_is_class (lisp_value_t val, uintptr_t class_id)
{
  if (val.tag == 0)
    {
      struct lisp_object *obj = val.ptr;
      if (obj)
        return obj->ops->class_id == class_id;
    }
  return false;
}

#define LISP_IS_LIST(val)                                                     \
  (LISP_IS_NIL ((val)) || (lisp_is_class ((val), LISP_CLASS_PAIR)))
#define LISP_IS_SYMBOL(val) (lisp_is_class ((val), LISP_CLASS_SYMBOL))
#define LISP_IS_STRING(val) (lisp_is_class ((val), LISP_CLASS_STRING))

struct lisp_pair
{
  struct lisp_object obj;

  lisp_value_t car;
  lisp_value_t cdr;
};

static lisp_value_t
lisp_car (lisp_env_t *env, lisp_value_t val)
{
  struct lisp_pair *pair;
  pair = lisp_get_object (env, val, LISP_CLASS_PAIR);
  if (!pair)
    return lisp_exception ();
  return pair->car;
}

static lisp_value_t
lisp_cdr (lisp_env_t *env, lisp_value_t val)
{
  struct lisp_pair *pair;
  pair = lisp_get_object (env, val, LISP_CLASS_PAIR);
  if (!pair)
    return lisp_exception ();
  return pair->cdr;
}

static lisp_value_t
lisp_car_f (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  return lisp_car (env, argv[0]);
}

static lisp_value_t
lisp_cdr_f (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  return lisp_cdr (env, argv[1]);
}

static int
lisp_pair_format (lisp_env_t *env, struct lisp_object *obj,
                  struct string_buf *buf)
{
  struct lisp_pair *pair = (struct lisp_pair *)obj;
  int is_first = 1;
  lisp_value_t car;
  lisp_value_t cdr;

  sbprintf (buf, "(");

  for (;;)
    {
      car = pair->car;
      cdr = pair->cdr;

      if (!is_first)
        {
          string_buf_append_char (buf, ' ');
        }
      is_first = 0;
      lisp_value_format (env, car, buf);

      if (LISP_IS_NIL (cdr))
        {
          string_buf_append_char (buf, ')');
          return 0;
        }

      if (LISP_IS_LIST (cdr))
        {
          pair = lisp_get_object (env, cdr, LISP_CLASS_PAIR);
        }

      else
        {
          sbprintf (buf, " . ");
          lisp_value_format (env, cdr, buf);
          string_buf_append_char (buf, ')');
          return 0;
        }
    }
}

static const struct lisp_object_class lisp_pair_class = {
  .class_id = LISP_CLASS_PAIR,
  .format = lisp_pair_format,
};

static int
lisp_pair_init (lisp_env_t *env, struct lisp_pair *pair, lisp_value_t car,
                lisp_value_t cdr)
{
  pair->obj.ops = &lisp_pair_class;

  pair->car = car;
  pair->cdr = cdr;

  return 0;
}

lisp_value_t
lisp_new_pair (lisp_env_t *env, lisp_value_t car, lisp_value_t cdr)
{
  struct lisp_pair *pair;

  if (LISP_IS_EXCEPTION (car) || LISP_IS_EXCEPTION (cdr))
    {
      return lisp_exception ();
    }

  pair = lisp_malloc (env, sizeof (*pair));
  if (!pair)
    return lisp_throw_out_of_memory (env);

  if (lisp_pair_init (env, pair, car, cdr))
    {
      return lisp_exception ();
    }

  return LISP_MAKE_PTR (pair);
}

static char *
lisp_toupper (lisp_env_t *env, const char *str)
{
  size_t i;
  size_t len;
  char *r;

  if (!str)
    return NULL;

  len = strlen (str);
  r = lisp_malloc (env, len + 1);
  if (!r)
    return NULL;
  for (i = 0; i < len + 1; ++i)
    r[i] = toupper (str[i]);
  return r;
}

typedef lisp_value_t lisp_syntax_proc_t (lisp_env_t *, lisp_value_t form,
                                         int magic, lisp_value_t *data);

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

  // C Proction.
  lisp_syntax_proc_t *proc;
};

static const struct lisp_object_class lisp_syntax_class = {
  .class_id = LISP_CLASS_SYNTAX,
};

static int
lisp_syntax_init (lisp_env_t *env, struct lisp_syntax *syntax,
                  lisp_syntax_proc_t proc, int magic, lisp_value_t *data,
                  int n)
{
  LISP_OBJECT_INIT (syntax, &lisp_syntax_class);
  syntax->magic = magic;
  syntax->proc = proc;
  syntax->data = lisp_malloc (env, sizeof (*data) * n);
  memcpy (syntax->data, data, sizeof (*data) * n);
  return 0;
}

lisp_value_t
lisp_new_syntax (lisp_env_t *env, lisp_syntax_proc_t proc, int magic,
                 lisp_value_t *data, int n)
{
  struct lisp_syntax *syntax = lisp_malloc (env, sizeof (*syntax));
  if (!syntax)
    goto fail;

  if (lisp_syntax_init (env, syntax, proc, magic, data, n))
    goto fail;

  return LISP_MAKE_PTR (syntax);

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
lisp_symbol_format (lisp_env_t *env, struct lisp_object *obj,
                    struct string_buf *buf)
{
  struct lisp_symbol *sym = (struct lisp_symbol *)obj;
  // string_buf_append_char (buf, '\'');
  return string_buf_append (buf, sym->name, strlen (sym->name));
}

static const struct lisp_object_class lisp_symbol_class = {
  .class_id = LISP_CLASS_SYMBOL,
  .format = &lisp_symbol_format,
};

static uint32_t lisp_hash_str (const char *s);

static unsigned string_equal (const char *, const char *);

lisp_value_t
lisp_interned_symbol (lisp_env_t *env, const char *name)
{
  struct lisp_symbol *sym;
  lisp_runtime_t *rt = lisp_get_runtime (env);
  uint32_t key = lisp_hash_str (name);

  hash_for_each_possible (rt->interned_sym_table, sym, snode, key)
    {
      if (string_equal (sym->name, name))
        {
          return LISP_MAKE_PTR (sym);
        }
    }

  sym = lisp_malloc (env, sizeof (*sym));
  if (!sym)
    return lisp_exception ();

  LISP_OBJECT_INIT (sym, &lisp_symbol_class);

  sym->name = lisp_toupper (env, name);
  if (sym->name == NULL)
    goto fail;
  sym->length = strlen (name);
  sym->index = rt->interned_sym_array_length;
  sym->is_static = 0;
  if (dynarray_add (rt->interned_sym_array, sym))
    goto fail;

  hash_add (rt->interned_sym_table, &sym->snode, key);

  return LISP_MAKE_PTR (sym);

fail:
  return lisp_exception ();
}

#define LISP_SYMBOL_DEF(name_, value)                                         \
  { \
    .obj = { \
      .ops = &lisp_symbol_class, \
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
lisp_eqv (lisp_value_t a, lisp_value_t b)
{
  assert (!LISP_IS_EXCEPTION (a));
  assert (!LISP_IS_EXCEPTION (b));
  if (a.tag != b.tag)
    return 0;

  if (LISP_IS_BOOL (a))
    return (a.i && b.i) || (!a.i && !b.i);
  if (LISP_IS_INT (a))
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
lisp_string_format (lisp_env_t *env, struct lisp_object *obj,
                    struct string_buf *buf)
{
  struct lisp_string *str = (struct lisp_string *)obj;

  string_buf_append_char (buf, '"');
  string_buf_append (buf, str->str, strlen (str->str));
  string_buf_append_char (buf, '"');
  return 0;
}

static const struct lisp_object_class lisp_string_class = {
  .class_id = LISP_CLASS_STRING,
  .format = &lisp_string_format,
};

lisp_value_t
lisp_new_string_full (lisp_env_t *env, const char *s, int is_static)
{
  struct lisp_string *str = lisp_malloc (env, sizeof (*str));
  lisp_value_t val = LISP_MAKE_PTR (str);
  if (!str)
    return lisp_throw_out_of_memory (env);

  LISP_OBJECT_INIT (str, &lisp_string_class);

  str->is_static = is_static;
  str->str = lisp_strdup (env, s);

  if (!str->str)
    {
      return lisp_exception ();
    }

  return val;
}

lisp_value_t
lisp_new_string (lisp_env_t *env, const char *s)
{
  return lisp_new_string_full (env, s, 0);
}

lisp_value_t
lisp_new_string_len (lisp_env_t *env, const char *n, size_t len)
{
  lisp_value_t val;
  char *s = GC_STRNDUP (n, len);
  if (!s)
    return lisp_throw_out_of_memory (env);

  val = lisp_new_string (env, s);
  return val;
}

#define LISP_STRING_OBJECT_INIT(name, value)                                  \
  {                                                                           \
    {                                                                         \
      &lisp_string_class,                                                     \
    },                                                                        \
        1, (char *)(value)                                                    \
  }

#define LISP_DEFINE_STRING_OBJECT(name, value)                                \
  struct lisp_string name = LISP_STRING_OBJECT_INIT (name, value)

#define LISP_DEFINE_STRING(name, value)                                       \
  static LISP_DEFINE_STRING_OBJECT (static_string_##name, value);             \
  static lisp_value_t name = LISP_MAKE_PTR (&static_string_##name);

static size_t
lisp_list_length (lisp_env_t *env, lisp_value_t list)
{
  size_t len = 0;
  lisp_value_t head;
  while (!LISP_IS_NIL (list))
    {
      lisp_list_extract (env, list, &head, 1, &list);
      ++len;
    }
  return len;
}

lisp_value_t
lisp_throw (lisp_env_t *env, lisp_value_t error)
{
  lisp_runtime_t *rt = lisp_get_runtime (env);
  printf ("%s: throwing ", env->name);
  lisp_print_value (env, error);
  rt->exception_list = lisp_new_pair (env, error, rt->exception_list);
  return lisp_exception ();
}

lisp_value_t
lisp_throw_out_of_memory (lisp_env_t *env)
{
  LISP_DEFINE_STRING (error_string, "Out of memory");
  return lisp_throw (env, error_string);
}

lisp_value_t
lisp_throw_internal_error_v (lisp_env_t *env, const char *fmt, va_list ap)
{
  char buffer[500];
  vsnprintf (buffer, sizeof (buffer), fmt, ap);
  return lisp_throw (env, lisp_new_string (env, buffer));
}

lisp_value_t
lisp_throw_internal_error (lisp_env_t *env, const char *fmt, ...)
{
  va_list ap;

  assert (fmt != NULL);

  va_start (ap, fmt);
  lisp_throw_internal_error_v (env, fmt, ap);
  va_end (ap);

  return lisp_exception ();
}

typedef lisp_value_t lisp_native_procedure_data (lisp_env_t *env, int argc,
                                                 lisp_value_t *argv, int magic,
                                                 lisp_value_t *data);

typedef lisp_value_t lisp_native_procedure_magic (lisp_env_t *env, int argc,
                                                  lisp_value_t *argv,
                                                  int magic);

typedef lisp_value_t lisp_native_procedure_simple (lisp_env_t *env, int argc,
                                                   lisp_value_t *argv);

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
lisp_throw_parse_error (lisp_env_t *env, enum lisp_parse_error code,
                        const char *fmt, ...)
{
  va_list ap;
  (void)code;

  va_start (ap, fmt);
  lisp_throw_internal_error_v (env, fmt, ap);
  va_end (ap);

  return lisp_exception ();
}

lisp_value_t
lisp_get_exception (lisp_env_t *env)
{
  lisp_runtime_t *rt = lisp_get_runtime (env);
  lisp_value_t val = lisp_car (env, rt->exception_list);
  rt->exception_list = lisp_cdr (env, rt->exception_list);
  return val;
}

void
lisp_print_exception (lisp_env_t *env)
{
  // lisp_print_value (env, env->runtime->exception_list);
  lisp_value_t error = lisp_get_exception (env);
  if (LISP_IS_EXCEPTION (error))
    abort ();

  printf ("%s: ", env->name);
  lisp_print_value (env, error);
}

static inline const char *
LISP_SYMBOL_STR (lisp_value_t sym)
{
  return ((struct lisp_symbol *)(sym).ptr)->name;
}

enum lisp_reader_state
{
  LISP_READER_STATE_PEEK,
  LISP_READER_STATE_NEXT,
};

inline static unsigned
string_equal (const char *a, const char *b)
{
  return strcasecmp (a, b) == 0;
}

struct lisp_reader
{
  lisp_env_t *env;
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
lisp_reader_init (struct lisp_reader *reader, lisp_env_t *env)
{
  reader->filep = NULL;
  reader->env = env;
  reader->state = LISP_READER_STATE_NEXT;
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
lisp_reader_new (lisp_env_t *env, FILE *filep)
{
  lisp_reader_t *reader = lisp_malloc (env, sizeof (*reader));
  if (!reader)
    return reader;

  lisp_reader_init (reader, env);
  reader->filep = filep;
  return reader;
}

void
lisp_reader_free (lisp_reader_t *reader)
{
  lisp_reader_destroy (reader);
}

lisp_value_t
lisp_read_form (struct lisp_reader *reader)
{
  const char *token;

  token = lisp_peek_token (reader);
  if (!token)
    return lisp_throw_parse_error (reader->env, LISP_PE_EOF, "EOF");

  if (string_equal (token, "(") || string_equal (token, "["))
    return lisp_read_list (reader);

  if (string_equal (token, ")") || string_equal (token, "]"))
    return lisp_throw_parse_error (reader->env, LISP_PE_EXPECT_RIGHT_PAREN,
                                   "Unexpected '%s'", token);

  if (string_equal (token, "'"))
    {
      lisp_next_token (reader);
      lisp_value_t quoted = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (quoted))
        return lisp_exception ();
      return lisp_new_pair (reader->env,
                            lisp_interned_symbol (reader->env, "QUOTE"),
                            lisp_new_pair (reader->env, quoted, lisp_nil ()));
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
  assert (string_equal (token, "(") || string_equal (token, "["));

  if (string_equal (token, "("))
    closing = ")";
  else
    closing = "]";

  for (;;)
    {
      lisp_value_t form;
      token = lisp_peek_token (reader);
      if (!token)
        {
          lisp_throw_parse_error (reader->env, LISP_PE_EARLY_EOF,
                                  "Unexpected eof when parsing list");
          goto fail;
        }

      if (string_equal (token, closing))
        break;

      if (string_equal (token, "."))
        {
          lisp_next_token (reader);
          *tail = lisp_read_form (reader);
          if (LISP_IS_EXCEPTION (*tail))
            goto fail;
          token = lisp_next_token (reader);
          if (!string_equal (token, closing))
            {
              lisp_throw_parse_error (reader->env, LISP_PE_EXPECT_RIGHT_PAREN,
                                      "expected '%s' but got '%s'", closing,
                                      token);
              goto fail;
            }
          return val;
        }

      form = lisp_read_form (reader);
      if (LISP_IS_EXCEPTION (form))
        goto fail;

      *tail = lisp_new_pair (reader->env, form, lisp_nil ());
      tail = &((struct lisp_pair *)tail->ptr)->cdr;
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
  assert (!string_equal (token, "(") && !string_equal (token, "["));
  assert (!string_equal (token, ")") && !string_equal (token, "]"));

  if (isdigit (token[0]))
    {
      if (strchr (token, '.'))
        {
          /* char *endptr = NULL; */
          /* double n = strtod (token, &endptr); */
          /* if (endptr != strend (token)) */
          /*   return lisp_throw_parse_error ( */
          /*       reader->env, LISP_PE_INVALID_NUMBER_LITERAL, */
          /*       "invalid number literal: %s", token); */
          /* return lisp_new_real (reader->env, n); */
          assert (0);
        }
      else
        {
          char *endptr = NULL;
          long n = strtol (token, &endptr, 0);
          if (endptr != strend (token))
            return lisp_throw_parse_error (
                reader->env, LISP_PE_INVALID_NUMBER_LITERAL,
                "invalid number literal: %s", token);
          return lisp_new_int32 (reader->env, n);
        }
    }

  if (token[0] == '"')
    return lisp_new_string_len (reader->env, token + 1, strlen (token) - 2);

  if (token[0] == '#')
    {
      if (strlen (token) != 2
          || (toupper (token[1]) != 'T' && toupper (token[1]) != 'F'))
        return lisp_throw_parse_error (reader->env,
                                       LISP_PE_INVALID_BOOLEAN_LITERAL,
                                       "Invalid boolean: %s", token);
      if (toupper (token[1]) == 'T')
        return lisp_true ();
      else
        return lisp_false ();
    }

  return lisp_interned_symbol (reader->env, token);
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
  if (reader->state == LISP_READER_STATE_PEEK)
    {
      reader->state = LISP_READER_STATE_NEXT;
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
  if (reader->state == LISP_READER_STATE_NEXT)
    {
      lisp_do_next_token (reader);
      // printf ("token : %s\n", reader->token);

      reader->state = LISP_READER_STATE_PEEK;
      return reader->token;
    }
  else
    {
      return reader->token;
    }
}

int
lisp_value_format (lisp_env_t *env, lisp_value_t val, struct string_buf *buf)
{
  if (LISP_IS_NIL (val))
    return sbprintf (buf, "()");

  if (LISP_IS_PTR (val))
    {
      struct lisp_object *obj = val.ptr;
      if (obj->ops->format)
        {
          return obj->ops->format (env, obj, buf);
        }
      else
        {
          return sbprintf (buf, "#OBJECT");
        }
    }

  if (LISP_IS_INT (val))
    {
      return sbprintf (buf, "%" PRIuPTR, (uintptr_t)val.i);
    }

  if (LISP_IS_BOOL (val))
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
lisp_value_to_string (lisp_env_t *env, lisp_value_t val)
{
  char *rv;
  struct string_buf sb;

  string_buf_init (&sb);
  if (lisp_value_format (env, val, &sb))
    {
      string_buf_destroy (&sb);
      return NULL;
    }

  rv = lisp_strdup (env, sb.s);
  string_buf_destroy (&sb);

  return rv;
}

void
lisp_print_value (lisp_env_t *env, lisp_value_t val)
{
  char *s = lisp_value_to_string (env, val);
  puts (s);
}

lisp_env_t *
lisp_new_env_extended (lisp_env_t *parent, const char *name)
{
  lisp_env_t *new_env = lisp_env_new (lisp_get_runtime (parent), name);
  new_env->parent = parent;
  return new_env;
}

int
lisp_to_int32 (lisp_env_t *env, int32_t *result, lisp_value_t val)
{
  if (LISP_IS_EXCEPTION (val))
    return -1;
  if (LISP_IS_INT (val))
    *result = val.i;
  else
    {
      lisp_throw_internal_error (env, "Value error: %i", val.tag);
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

static struct lisp_variable *
lisp_find_var_from_map (struct rb_root *tree, lisp_value_t name)
{
  struct rb_node *node = tree->rb_node;
  while (node != NULL)
    {
      struct lisp_variable *var = rb_entry (node, struct lisp_variable, rb);
      int c = strcasecmp (LISP_SYMBOL_STR (name), LISP_SYMBOL_STR (var->name));
      if (c < 0)
        node = node->rb_left;
      else if (c > 0)
        node = node->rb_right;
      else
        return var;
    }
  return NULL;
}

static lisp_value_t
lisp_env_get_var (lisp_env_t *env, lisp_value_t name)
{
  struct lisp_variable *var;
  lisp_env_t *orig_env = env;
  if (LISP_IS_EXCEPTION (name))
    return lisp_exception ();
  if (!LISP_IS_SYMBOL (name))
    return lisp_throw_internal_error (env, "type error");

  while (env != NULL)
    {
      var = lisp_find_var_from_map (&env->var_map, name);
      if (var != NULL)
        return var->value;

      env = env->parent;
    }

  return lisp_throw_internal_error (orig_env, "no such variable: %s",
                                    LISP_SYMBOL_STR (name));
}

static lisp_value_t
lisp_env_set_var (lisp_env_t *env, lisp_value_t name, lisp_value_t value)
{
  struct lisp_variable *var;
  lisp_env_t *orig_env = env;
  if (LISP_IS_EXCEPTION (name))
    {
      return lisp_exception ();
    }
  if (!LISP_IS_SYMBOL (name))
    {
      return lisp_throw_internal_error (env, "type error");
    }

  while (env != NULL)
    {
      var = lisp_find_var_from_map (&env->var_map, name);
      if (var != NULL)
        {
          var->value = value;
          return lisp_void ();
        }

      env = env->parent;
    }

  return lisp_throw_internal_error (orig_env, "no such variable");
}

static struct lisp_variable *
lisp_add_var_to_map (struct rb_root *tree, struct lisp_variable *var)
{
  struct rb_node *parent = NULL;
  struct rb_node **link = &tree->rb_node;
  int c;

  while (*link != NULL)
    {
      parent = *link;
      c = strcasecmp (
          LISP_SYMBOL_STR (var->name),
          LISP_SYMBOL_STR (rb_entry (parent, struct lisp_variable, rb)->name));
      if (c < 0)
        link = &parent->rb_left;
      else if (c > 0)
        link = &parent->rb_right;
      else
        return rb_entry (parent, struct lisp_variable, rb);
    }

  rb_link_node (&var->rb, parent, link);
  rb_balance_insert (&var->rb, tree);
  return NULL;
}

static int
lisp_env_define_var (lisp_env_t *env, lisp_value_t name, lisp_value_t value)
{
  struct lisp_variable *var;

  if (LISP_IS_EXCEPTION (name) || LISP_IS_EXCEPTION (value))
    goto fail;

  if (!LISP_IS_SYMBOL (name))
    {
      lisp_throw_internal_error (env, "name is not symbol");
      goto fail;
    }

  var = lisp_malloc (env, sizeof (*var));
  if (!var)
    goto fail;

  var->name = name;
  var->value = value;

  if (lisp_add_var_to_map (&env->var_map, var))
    {
      lisp_throw_internal_error (env, "name is already defined");
      goto fail;
    }

  return 0;

fail:
  return -1;
}

static int
lisp_procedure_set_args (lisp_env_t *env, lisp_env_t *new_env,
                         lisp_value_t params, lisp_value_t args)
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
              expr = lisp_car (env, args);
              *tail = lisp_new_pair (env, lisp_eval (env, expr), lisp_nil ());
              tail = &((struct lisp_pair *)tail->ptr)->cdr;
              args = lisp_cdr (env, args);
            }

          if (lisp_env_define_var (new_env, name, value))
            {
              return -1;
            }
          return 0;
        }
      else
        {
          name = lisp_car (env, params);
          expr = lisp_car (env, args);
          value = lisp_eval (env, expr);
        }

      if (lisp_env_define_var (new_env, name, value))
        {
          return -1;
        }
      params = lisp_cdr (env, params);
      args = lisp_cdr (env, args);
    }

  return 0;
}

static lisp_value_t
lisp_eval_list (lisp_env_t *env, lisp_value_t list)
{
  lisp_value_t val = LISP_NIL;
  while (!LISP_IS_NIL (list) && !LISP_IS_EXCEPTION (val))
    {
      lisp_value_t exp = lisp_car (env, list);
      list = lisp_cdr (env, list);
      val = lisp_eval (env, exp);
    }
  return val;
}

static lisp_value_t
lisp_procedure_invoker (lisp_env_t *env, lisp_value_t args,
                        struct lisp_procedure *proc)
{
  lisp_value_t val = lisp_exception ();
  lisp_env_t *new_env
      = lisp_new_env_extended (proc->env, LISP_SYMBOL_STR (proc->name));
  if (new_env)
    {
      if (!lisp_procedure_set_args (env, new_env, proc->params, args))
        val = lisp_eval_list (new_env, proc->body);
    }
  return val;
}

static lisp_value_t *
lisp_eval_args (lisp_env_t *env, lisp_value_t args, int max_arg, int *n_args)
{
  size_t length;
  size_t argc = lisp_list_length (env, args);
  lisp_value_t *arr;
  size_t i;

  if (max_arg == -1)
    length = argc;
  else if ((size_t)max_arg >= argc)
    length = max_arg;
  else
    {
      lisp_throw_internal_error (env, "too many arguments");
      return NULL;
    }

  arr = lisp_malloc (env, sizeof (lisp_value_t) * length);
  if (!arr)
    return NULL;

  if (lisp_list_extract (env, args, arr, argc, NULL))
    {
      return NULL;
    }
  for (i = 0; i < argc; ++i)
    {
      arr[i] = lisp_eval (env, arr[i]);
      if (LISP_IS_EXCEPTION (arr[i]))
        {
          return NULL;
        }
    }

  *n_args = (int)argc;
  return arr;
}

static lisp_value_t
lisp_native_procedure_invoker (lisp_env_t *env, lisp_value_t args,
                               struct lisp_procedure *proc)
{
  int argc = -1;
  lisp_value_t *argv;
  lisp_value_t val;
  lisp_env_t *tmp_env;

  lisp_native_procedure_simple *native_procedure = proc->native_procedure;

  argv = lisp_eval_args (env, args, proc->arg_max, &argc);
  if (!argv)
    return lisp_exception ();

  tmp_env = lisp_new_env_extended (env, "#NATIVE_PROCEDURE");
  if (tmp_env)
    {
      val = native_procedure (tmp_env, argc, argv);
    }
  else
    {
      val = lisp_exception ();
    }
  return val;
}

static lisp_value_t
lisp_new_procedure (lisp_env_t *env, lisp_value_t name, lisp_value_t params,
                    lisp_value_t body, lisp_native_invoke_t invoker)
{
  struct lisp_procedure *fn = lisp_malloc (env, sizeof (*fn));
  lisp_value_t val = LISP_MAKE_PTR (fn);

  if (!fn)
    return lisp_exception ();

  LISP_OBJECT_INIT (fn, &lisp_procedure_class);

  fn->params = params;
  fn->body = body;
  fn->invoker = invoker;
  fn->name = name;

  fn->env = lisp_new_env_extended (env, LISP_SYMBOL_STR (name));
  if (!fn->env)
    {
      return lisp_exception ();
    }

  return val;
}

static lisp_value_t
lisp_new_native_procedure (lisp_env_t *env, const char *name,
                           lisp_native_procedure_simple *native_procedure,
                           int n)
{
  lisp_value_t proc = lisp_new_procedure (
      env, lisp_interned_symbol (env, name), lisp_interned_symbol (env, name),
      lisp_nil (), lisp_native_procedure_invoker);
  if (LISP_IS_EXCEPTION (proc))
    return lisp_exception ();

  {
    struct lisp_procedure *fn
        = lisp_get_object (env, proc, LISP_CLASS_PROCEDURE);
    fn->arg_max = n;
    fn->native_procedure = native_procedure;
  }

  return proc;
}

lisp_value_t
lisp_new_int32 (lisp_env_t *env, int32_t v)
{
  lisp_value_t val = LISP_INT32 (v);
  (void)env;
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
lisp_to_bool (lisp_env_t *env, lisp_value_t val, int *res)
{
  if (LISP_IS_EXCEPTION (val))
    return -1;

  if (!LISP_IS_BOOL (val))
    {
      lisp_throw_internal_error (env, "Expected a boolean");
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
 * (define (proc params) body)
 */
static lisp_value_t
lisp_define (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  lisp_value_t sig = lisp_car (env, args);

  if (LISP_IS_LIST (sig))
    { // defining proction
      lisp_value_t name = lisp_car (env, sig);
      lisp_value_t params = lisp_cdr (env, sig);
      lisp_value_t body = lisp_cdr (env, args);
      lisp_value_t proc = lisp_new_procedure (env, name, params, body,
                                              &lisp_procedure_invoker);

      // lisp_set_procedure_name (env, proc, lisp_dup_value (env, name));
      if (lisp_env_define_var (env, name, proc))
        return lisp_exception ();

      return lisp_void ();
    }
  else if (LISP_IS_SYMBOL (sig))
    { // define variable
      lisp_value_t tmp = lisp_cdr (env, args);
      lisp_value_t expr = lisp_car (env, tmp);
      lisp_value_t value = lisp_eval (env, expr);
      if (lisp_env_define_var (env, sig, value))
        return lisp_exception ();
      return lisp_void ();
    }

  return lisp_throw_internal_error (env, "Invalid syntax");
}

/**
 * (set! var value)
 */
static lisp_value_t
lisp_set_ (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  lisp_value_t val[2];

  if (lisp_list_extract (env, args, val, 2, NULL))
    return lisp_exception ();

  val[1] = lisp_eval (env, val[1]);

  return lisp_env_set_var (env, val[0], val[1]);
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
lisp_let (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  lisp_value_t variables;
  lisp_value_t binding;
  lisp_value_t body;
  lisp_env_t *new_env;
  lisp_value_t ret = LISP_EXCEPTION;

  if (lisp_list_extract (env, args, &variables, 1, &body))
    return lisp_exception ();

  new_env = lisp_new_env_extended (env, "LET");
  if (!new_env)
    return lisp_exception ();

  while (!LISP_IS_NIL (variables))
    {
      lisp_value_t val[2];

      if (lisp_list_extract (env, variables, &binding, 1, &variables))
        goto fail;

      if (lisp_list_extract (env, binding, val, 2, NULL))
        goto fail;

      if (magic == LISP_MAGIC_LETSTAR)
        {
          lisp_env_t *tmp = lisp_new_env_extended (new_env, "#LET*");
          new_env = tmp;
        }

      if (magic == LISP_MAGIC_LET || magic == LISP_MAGIC_LETREC)
        val[1] = lisp_eval (new_env, val[1]);
      else
        val[1] = lisp_eval (env, val[1]);

      if (lisp_env_define_var (new_env, val[0], val[1]))
        goto fail;
    }

  ret = lisp_eval_list (new_env, body);

fail:
  return ret;
}

/**
 * (quote exp)
 */
static lisp_value_t
lisp_quote (lisp_env_t *env, lisp_value_t list, int magic, lisp_value_t *data)
{
  return lisp_car (env, list);
}

static lisp_value_t
lisp_if (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  lisp_value_t vals[2];
  lisp_value_t tail;
  lisp_value_t cond_v;
  int cond;

  if (lisp_list_extract (env, args, vals, 2, &tail))
    return lisp_exception ();

  cond_v = lisp_eval (env, vals[0]);

  if (lisp_to_bool (env, cond_v, &cond))
    {
      return lisp_exception ();
    }

  if (cond)
    {
      return lisp_eval (env, vals[1]);
    }
  else
    {
      return lisp_eval_list (env, tail);
    }
}

int
lisp_list_extract (lisp_env_t *env, lisp_value_t list, lisp_value_t *heads,
                   int n, lisp_value_t *tail)
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
      heads[i] = lisp_car (env, *tail);
      if (LISP_IS_EXCEPTION (heads[i]))
        goto fail;

      *tail = lisp_cdr (env, *tail);
    }

  res = 0;

fail:
  return res;
}

/**
 * (cond (CONDTION BODY...) ...)
 */
static lisp_value_t
lisp_cond (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  lisp_value_t clause;

  while (!LISP_IS_NIL (args))
    {
      int value;
      lisp_value_t condition_value;
      lisp_value_t condition, body;
      if (lisp_list_extract (env, args, &clause, 1, &args))
        return lisp_exception ();

      if (lisp_list_extract (env, clause, &condition, 1, &body))
        return lisp_exception ();

      if (LISP_IS_SYMBOL (condition)
          && lisp_sym_eq (condition, lisp_interned_symbol (env, "ELSE")))
        {
          if (!LISP_IS_NIL (args))
            return lisp_throw_internal_error (
                env, "ELSE must be the last clause in COND");
          condition_value = lisp_true ();
        }
      else
        condition_value = lisp_eval (env, condition);
      if (LISP_IS_EXCEPTION (condition_value))
        return lisp_exception ();
      if (lisp_to_bool (env, condition_value, &value))
        {
          return lisp_exception ();
        }
      if (value)
        {
          return lisp_eval_list (env, body);
        }
    }

  return lisp_nil ();
}

/**
 * (named-lambda (name params...) body)
 */
static lisp_value_t
lisp_named_lambda (lisp_env_t *env, lisp_value_t args, int magic,
                   lisp_value_t *data)
{
  lisp_value_t params;
  lisp_value_t body;
  lisp_value_t val;

  params = lisp_car (env, args);
  body = lisp_cdr (env, args);

  val = lisp_new_procedure (env, lisp_car (env, params),
                            lisp_cdr (env, params), body,
                            lisp_procedure_invoker);

  return val;
}

/**
 * (lambda (name params...) body)
 */
static lisp_value_t
lisp_lambda (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  lisp_value_t params;
  lisp_value_t body;
  lisp_value_t val;

  params = lisp_car (env, args);
  body = lisp_cdr (env, args);

  val = lisp_new_procedure (env, lisp_interned_symbol (env, "#[lambda]"),
                            params, body, lisp_procedure_invoker);

  return val;
}

static lisp_value_t
lisp_begin (lisp_env_t *env, lisp_value_t args, int magic, lisp_value_t *data)
{
  return lisp_eval_list (env, args);
}

/********************************************************************************
 *  Proctions
 *******************************************************************************/

// static lisp_value_t
// lisp_eqv_p (lisp_env_t *env, int argc, lisp_value_t *argv)
// {

// }

static lisp_value_t
lisp_gc_ (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  GC_collect_a_little ();
  return lisp_nil ();
}

static lisp_value_t
lisp_eval_ (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  return lisp_eval (env, argv[0]);
}

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

static const struct lisp_object_class lisp_vector_class = {
  .class_id = LISP_CLASS_VECTOR,
};

static lisp_value_t
lisp_new_vector (lisp_env_t *env, int n, lisp_value_t *elems)
{
  struct lisp_vector *vec;
  int i;

  vec = lisp_malloc (env, sizeof (*vec));
  if (!vec)
    return lisp_exception ();

  LISP_OBJECT_INIT (vec, &lisp_vector_class);

  vec->capacity = n;
  vec->length = n;
  vec->data = lisp_malloc (env, sizeof (lisp_value_t) * n);
  if (vec->data == NULL)
    {
      return lisp_exception ();
    }

  for (i = 0; i < n; ++i)
    {
      vec->data[i] = elems[i];
    }

  return LISP_MAKE_PTR (vec);
}

static lisp_value_t
lisp_make_vector (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_vector *vec;
  int32_t k;
  lisp_value_t o;

  if (argc < 1)
    return lisp_throw_internal_error (env, "require at least one argument");

  if (lisp_to_int32 (env, &k, argv[0]))
    return lisp_exception ();

  o = argv[1];

  vec = lisp_malloc (env, sizeof (*vec));
  if (!vec)
    return lisp_exception ();

  LISP_OBJECT_INIT (vec, &lisp_vector_class);

  vec->length = k;
  vec->capacity = k;
  vec->data = lisp_malloc (env, sizeof (o) * k);
  if (!vec->data)
    {
      return lisp_exception ();
    }

  while (k > 0)
    {
      vec->data[--k] = o;
    }

  return LISP_MAKE_PTR (vec);
}

static lisp_value_t
lisp_vector_copy (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (env, argv[0], LISP_CLASS_VECTOR);
  if (!vec)
    return lisp_exception ();

  return lisp_new_vector (env, vec->length, vec->data);
}

static lisp_value_t
lisp_vector_length (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (env, argv[0], LISP_CLASS_VECTOR);

  return lisp_new_int32 (env, vec->length);
}

static lisp_value_t
lisp_vector_capacity (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (env, argv[0], LISP_CLASS_VECTOR);

  return lisp_new_int32 (env, vec->capacity);
}

/**
 * (vector-ref my-vec 1)
 */
static lisp_value_t
lisp_vector_ref (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (env, argv[0], LISP_CLASS_VECTOR);
  int32_t pos = -1;

  if (lisp_to_int32 (env, &pos, argv[1]))
    return lisp_exception ();

  if (pos < 0 || (size_t)pos >= vec->length)
    return lisp_throw_internal_error (env, "Out of range");

  return vec->data[pos];
}

/**
 * (vector-set! my-vec 2 elem)
 */
static lisp_value_t
lisp_vector_set (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_vector *vec = lisp_get_object (env, argv[0], LISP_CLASS_VECTOR);
  int32_t pos = -1;

  if (lisp_to_int32 (env, &pos, argv[1]))
    return lisp_exception ();

  if (pos < 0 || (size_t)pos >= vec->length)
    return lisp_throw_internal_error (env, "Out of range");

  vec->data[pos] = argv[2];

  return lisp_nil ();
}

static lisp_value_t
lisp_display (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  int i;
  char *s;
  bool need_delim = false;

  for (i = 0; i < argc; ++i)
    {
      s = lisp_value_to_string (env, argv[i]);

      if (need_delim)
        {
          printf (" ");
        }
      need_delim = true;
      printf ("%s", s);
    }

  return lisp_nil ();
}

static lisp_value_t
lisp_less (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  int i;

  for (i = 0; i + 1 < argc; ++i)
    {
      int32_t v1 = -1;
      int32_t v2 = -2;

      if (lisp_to_int32 (env, &v1, argv[i])
          || lisp_to_int32 (env, &v2, argv[i + 1]))
        return lisp_exception ();
      if (!(v1 < v2))
        return lisp_false ();
    }

  return lisp_true ();
}

lisp_value_t
lisp_sum (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  int sum = 0;
  int i;

  for (i = 0; i < argc; ++i)
    {
      int32_t v = 0;
      if (lisp_to_int32 (env, &v, argv[i]))
        return lisp_exception ();
      sum += v;
    }

  return lisp_new_int32 (env, sum);
}

lisp_value_t
lisp_subtract (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  int result = 0;
  int i;

  if (argc == 0)
    return lisp_new_int32 (env, 0);

  if (lisp_to_int32 (env, &result, argv[0]))
    return lisp_exception ();

  if (argc == 1)
    return lisp_new_int32 (env, -result);

  for (i = 1; i < argc; ++i)
    {
      int v = 0;
      if (lisp_to_int32 (env, &v, argv[i]))
        return lisp_exception ();
      result -= v;
    }

  return lisp_new_int32 (env, result);
}

static lisp_value_t
lisp_apply (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  struct lisp_procedure *fn
      = lisp_get_object (env, argv[0], LISP_CLASS_PROCEDURE);
  if (!fn)
    return lisp_exception ();

  return lisp_eval (env, lisp_new_pair (env, LISP_MAKE_PTR (fn), argv[1]));
}

static lisp_value_t
lisp_nullp (lisp_env_t *env, int argc, lisp_value_t *argv)
{
  if (LISP_IS_NIL (argv[0]))
    return lisp_true ();
  return lisp_false ();
}

static int
lisp_define_native_procedure (lisp_env_t *env, lisp_value_t name,
                              lisp_native_procedure_simple *native_procedure,
                              int n)
{
  return lisp_env_define_var (
      env, name,
      lisp_new_native_procedure (env, LISP_SYMBOL_STR (name), native_procedure,
                                 n));
}

#define LISP_DEFINE_NATIVE_PROCEDURE(env, name, native_procedure, n)          \
  lisp_define_native_procedure (env, lisp_interned_symbol (env, name),        \
                                native_procedure, n)

static int
lisp_define_syntax (lisp_env_t *env, lisp_value_t name,
                    lisp_syntax_proc_t proc, int magic, lisp_value_t *data,
                    int n)
{
  return lisp_env_define_var (env, name,
                              lisp_new_syntax (env, proc, magic, data, n));
}

#define LISP_DEFINE_MACRO(env, name, proc, magic)                             \
  lisp_define_syntax (env, lisp_interned_symbol (env, name), proc, magic,     \
                      NULL, 0)

lisp_env_t *
lisp_new_top_level_env (lisp_runtime_t *rt)
{
  lisp_env_t *env = lisp_env_new (rt, "<GLOBAL>");
  lisp_env_t *r;

  LISP_DEFINE_MACRO (env, "BEGIN", lisp_begin, 0);
  LISP_DEFINE_MACRO (env, "COND", lisp_cond, 0);
  LISP_DEFINE_MACRO (env, "DEFINE", lisp_define, 0);
  LISP_DEFINE_MACRO (env, "IF", lisp_if, 0);
  LISP_DEFINE_MACRO (env, "NAMED-LAMBDA", lisp_named_lambda, 0);
  LISP_DEFINE_MACRO (env, "LAMBDA", lisp_lambda, 0);
  LISP_DEFINE_MACRO (env, "LET", lisp_let, LISP_MAGIC_LET);
  LISP_DEFINE_MACRO (env, "LET*", lisp_let, LISP_MAGIC_LETSTAR);
  LISP_DEFINE_MACRO (env, "LETREC", lisp_let, LISP_MAGIC_LETREC);
  LISP_DEFINE_MACRO (env, "QUOTE", lisp_quote, 0);
  LISP_DEFINE_MACRO (env, "SET!", lisp_set_, 0);

  LISP_DEFINE_NATIVE_PROCEDURE (env, "EVAL", lisp_eval_, 1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "APPLY", lisp_apply, 2);

  LISP_DEFINE_NATIVE_PROCEDURE (env, "NULL?", lisp_nullp, 1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "CAR", lisp_car_f, 1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "CDR", lisp_cdr_f, 1);

  LISP_DEFINE_NATIVE_PROCEDURE (env, "MAKE-VECTOR", lisp_make_vector, 2);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "VECTOR", lisp_new_vector, -1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "VECTOR-COPY", lisp_vector_copy, 1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "VECTOR-LENGTH", lisp_vector_length, 1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "VECTOR-CAPACITY", lisp_vector_capacity,
                                1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "VECTOR-REF", lisp_vector_ref, 2);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "VECTOR-SET!", lisp_vector_set, 3);

  lisp_env_define_var (env, lisp_interned_symbol (env, "#T"), lisp_true ());
  lisp_env_define_var (env, lisp_interned_symbol (env, "#F"), lisp_false ());
  lisp_env_define_var (env, lisp_interned_symbol (env, "NIL"), lisp_nil ());

  LISP_DEFINE_NATIVE_PROCEDURE (env, "+", lisp_sum, -1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "-", lisp_subtract, -1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "<", lisp_less, -1);
  LISP_DEFINE_NATIVE_PROCEDURE (env, "DISPLAY", lisp_display, -1);

  LISP_DEFINE_NATIVE_PROCEDURE (env, "GC", lisp_gc_, 0);

  r = lisp_new_env_extended (env, "TOP-LEVEL");

  return r;
}

lisp_value_t
lisp_eval (lisp_env_t *env, lisp_value_t val)
{
  lisp_value_t r = LISP_NIL;

  if (LISP_IS_LIST (val) && !LISP_IS_NIL (val))
    {
      lisp_value_t args;
      lisp_value_t proc_expr = lisp_car (env, val);
      lisp_value_t proc = lisp_eval (env, proc_expr);

      if (LISP_IS_EXCEPTION (proc))
        {
          return lisp_exception ();
        }

      if (lisp_is_class (proc, LISP_CLASS_PROCEDURE))
        {

          struct lisp_procedure *fn
              = lisp_get_object (env, proc, LISP_CLASS_PROCEDURE);

          args = lisp_cdr (env, val);
          r = fn->invoker (env, args, fn);
          return r;
        }

      else if (lisp_is_class (proc, LISP_CLASS_SYNTAX))
        {
          struct lisp_syntax *syntax
              = lisp_get_object (env, proc, LISP_CLASS_SYNTAX);
          args = lisp_cdr (env, val);
          r = syntax->proc (env, args, syntax->magic, syntax->data);
        }

      else
        {
          lisp_throw_internal_error (env, "Need a proction");
          return lisp_exception ();
        }

      return r;
    }

  if (LISP_IS_SYMBOL (val))
    {
      return lisp_env_get_var (env, val);
    }

  return val;
}

static int
lisp_procedure_format (lisp_env_t *env, struct lisp_object *obj,
                       struct string_buf *buf)
{
  struct lisp_procedure *proc = (struct lisp_procedure *)obj;
  sbprintf (buf, "[Proction %s]", LISP_SYMBOL_STR (proc->name));
  return 0;
}
