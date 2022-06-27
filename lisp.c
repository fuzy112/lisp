#include "lisp.h"
#include "list.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lisp_object;

struct lisp_object_operations
{
  void (*free) (lisp_context_t *ctx, struct lisp_object *);
  char *(*to_string) (lisp_context_t *ctx, struct lisp_object *);
};

struct lisp_object
{
  struct list_head list;
  long ref_count;
  const struct lisp_object_operations *ops;
};

struct lisp_runtime
{
  long nr_blocks;
  struct list_head object_list;
  lisp_value_t exception_list;
};

struct lisp_context
{
  char *name;
  struct lisp_context *parent;
  struct lisp_runtime *runtime;

  struct list_head object_list;
  struct list_head var_list;
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
};

static void
lisp_function_free (lisp_context_t *ctx, struct lisp_object *obj)
{
  struct lisp_function *func = (struct lisp_function *)obj;

  lisp_free_value (ctx, func->params);
  lisp_free_value (ctx, func->body);
  lisp_free_value (ctx, func->name);
  func->invoker = NULL;

  lisp_free (ctx, func);
}

static const struct lisp_object_operations lisp_function_operations = {
  .free = &lisp_function_free,
};

struct lisp_variable
{
  struct list_head list;

  lisp_value_t name;
  lisp_value_t value;
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
  INIT_LIST_HEAD (&rt->object_list);
  rt->exception_list = lisp_nil ();
  return rt;
}

void
lisp_runtime_free (lisp_runtime_t *rt)
{
  assert (rt->nr_blocks == 0);
  assert (list_empty (&rt->object_list));
  free (rt);
}

lisp_context_t *
lisp_context_new (lisp_runtime_t *rt, const char *name)
{
  lisp_context_t *ctx = lisp_malloc_rt (rt, sizeof (*ctx));
  if (!ctx)
    return NULL;
  INIT_LIST_HEAD (&ctx->object_list);
  INIT_LIST_HEAD (&ctx->var_list);
  ctx->parent = NULL;
  ctx->runtime = rt;
  ctx->name = lisp_strdup_rt (rt, name);
  return ctx;
}

void
lisp_context_free (lisp_context_t *ctx)
{
  struct lisp_variable *var, *tmp_var;

  list_for_each_entry_safe (var, tmp_var, &ctx->var_list, list)
  {
    lisp_free_value (ctx, var->name);
    lisp_free_value (ctx, var->value);
    list_del (&var->list);
    lisp_free (ctx, var);
  }

  if (!list_empty (&ctx->object_list))
    {
      if (ctx->parent)
        {
          list_splice (&ctx->object_list, &ctx->parent->object_list);
        }
      else if (ctx->runtime)
        {
          list_splice (&ctx->object_list, &ctx->runtime->object_list);
        }
      else
        {
          assert (0 && "unreachable");
        }
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
          list_del_init (&obj->list);

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

struct string_buf
{
  char *s;
  size_t capacity;
  size_t length;
};

static int
string_buf_append (struct string_buf *buf, const char *str, size_t len)
{
  char *tmp;

  while (buf->length + len >= buf->capacity)
    {
      if (buf->capacity == 0)
        {
          buf->capacity = len + 1;
        }
      else
        {
          buf->capacity = 2 * buf->capacity;
        }
    }

  tmp = realloc (buf->s, buf->capacity);
  if (!tmp)
    return -1;
  buf->s = tmp;

  memcpy (buf->s + buf->length, str, len);
  buf->length += len;
  buf->s[buf->length] = '\0';

  return 0;
}

static lisp_value_t
lisp_car (lisp_context_t *ctx, lisp_value_ref_t val)
{
  struct lisp_cons *cons = lisp_get_object (ctx, val, LISP_TAG_LIST);
  if (!cons)
    return lisp_exception ();
  return lisp_dup_value (ctx, cons->car);
}

static lisp_value_t
lisp_cdr (lisp_context_t *ctx, lisp_value_ref_t val)
{
  struct lisp_cons *cons = lisp_get_object (ctx, val, LISP_TAG_LIST);
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
  struct lisp_cons *cons = lisp_malloc (ctx, sizeof (*cons));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_LIST, cons);
  if (!cons)
    return lisp_throw_out_of_memory (ctx);

  cons->obj.ops = &lisp_cons_operations;
  cons->obj.ref_count = 1;

  cons->car = car;
  cons->cdr = cdr;

  list_add_tail (&cons->obj.list, &ctx->object_list);
  return val;
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
  size_t len = strlen (str);
  char *r = lisp_malloc (ctx, len + 1);
  if (!r)
    return NULL;
  for (i = 0; i < len + 1; ++i)
    r[i] = toupper(str[i]);
  return r;
}

lisp_value_t
lisp_new_symbol_full (lisp_context_t *ctx, const char *name, int is_static)
{
  struct lisp_symbol *sym = lisp_malloc (ctx, sizeof (*sym));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_SYMBOL, sym);
  sym->is_static = is_static;
  sym->name = lisp_toupper (ctx, name);

  list_add_tail (&sym->obj.list, &ctx->object_list);
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
    { LIST_HEAD_INIT ((name).obj.list), 1, &lisp_static_symbol_operations },  \
        1, (char *)(value)                                                    \
  }

#define LISP_DEFINE_SYMBOL_OBJECT(name, value)                                \
  struct lisp_symbol name = LISP_SYMBOL_OBJECT_INIT (name, value)

#define LISP_DEFINE_SYMBOL(name, value)                                       \
  static LISP_DEFINE_SYMBOL_OBJECT (static_symbol_##name, value);             \
  static lisp_value_t name                                                    \
      = LISP_OBJECT (LISP_TAG_SYMBOL, &static_symbol_##name);

static int
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
  list_add_tail (&str->obj.list, &ctx->object_list);
  return val;
}

lisp_value_t
lisp_new_string (lisp_context_t *ctx, const char *s)
{
  return lisp_new_string_full (ctx, s, 0);
}

#define LISP_STRING_OBJECT_INIT(name, value)                                  \
  {                                                                           \
    { LIST_HEAD_INIT ((name).obj.list), 1, &lisp_static_string_operations },  \
        1, (char *)(value)                                                    \
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
lisp_throw_internal_error (lisp_context_t *ctx, const char *fmt, ...)
{
  char buffer[500];
  va_list ap;
  lisp_value_t err_msg;

  va_start (ap, fmt);
  vsnprintf (buffer, sizeof (buffer), fmt, ap);
  va_end (ap);

  err_msg = lisp_new_string (ctx, buffer);
  return lisp_throw (ctx, err_msg);
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

static char *
skipws (const char *text)
{
  while (*text && isspace (*text))
    ++text;

  if (*text)
    return (char *)text;
  return NULL;
}

static lisp_value_t
lisp_parse_atom (lisp_context_t *ctx, const char **text)
{
  if (isdigit ((*text)[0]))
    {
      long n;
      char *endptr = NULL;

      errno = 0;
      n = strtol (*text, &endptr, 0);
      assert (errno == 0);
      *text = endptr;

      {
        lisp_value_t val = LISP_INT32 (n);
        return val;
      }
    }

  if (isalpha ((*text)[0]) || strchr ("#+-*/%<>=!&|^_?:", **text) != NULL)
    {
      size_t pos = strcspn (*text, " \t\n\r\f)");
      char *sym = strndup (*text, pos);
      lisp_value_t val = lisp_new_symbol (ctx, sym);
      *text += pos;

      free (sym);
      return val;
    }

  if (**text == '\"')
    {
      size_t pos = 1;
      do
        {
          pos += strcspn (*text + pos, "\"");
        }
      while ((*text)[pos - 1] == '\\');

      {
        char *str = strndup (*text + 1, pos - 1);
        lisp_value_t val = lisp_new_string (ctx, str);
        *text += pos;
        free (str);
        return val;
      }
    }

  return lisp_throw_internal_error (ctx, "Syntax error");
}

static lisp_value_t
lisp_parse_sexpr (lisp_context_t *ctx, const char **text)
{
  lisp_value_t val = LISP_NIL;

  *text = skipws (*text);

  if (!*text)
    return lisp_throw_internal_error (ctx, "Unexpected end of file");
  if (**text == ')')
    {
      ++*text;
      return val;
    }
  val = lisp_parse (ctx, text);
  if (LISP_IS_EXCEPTION (val))
    return lisp_exception ();

  if (!*text)
    return lisp_throw_internal_error (ctx, "Unexpected end of file");
  *text = skipws (*text);

  if (!*text)
    return lisp_throw_internal_error (ctx, "Unexpected end of file");
  if (**text == ')')
    {
      lisp_value_t nil = LISP_NIL;
      ++*text;
      return lisp_new_cons (ctx, val, nil);
    }
  else if (**text == '.')
    {
      lisp_value_t cdr;
      ++*text;
      cdr = lisp_parse (ctx, text);
      if (LISP_IS_EXCEPTION (cdr))
        return lisp_exception ();
      if (!*text)
        return lisp_throw_internal_error (ctx, "Unexpected end of file");
      *text = skipws (*text);
      if (**text != ')')
        {
          lisp_free_value (ctx, cdr);
          return lisp_throw_internal_error (
              ctx, "Syntax error: expecting ')' but got %c", **text);
        }
      ++*text;
      return lisp_new_cons (ctx, val, cdr);
    }
  else
    {
      lisp_value_t cdr;
      cdr = lisp_parse_sexpr (ctx, text);
      if (LISP_IS_EXCEPTION (cdr))
        return lisp_exception ();
      return lisp_new_cons (ctx, val, cdr);
    }
}

lisp_value_t
lisp_parse (lisp_context_t *ctx, const char **text)
{
  *text = skipws (*text);
  if (!*text)
    {
      *text = NULL;
      return lisp_nil ();
    }
  if ((*text)[0] == '(')
    {
      ++*text;
      return lisp_parse_sexpr (ctx, text);
    }
  if ((*text)[0] == '\'')
    {
      *text += 1;
      return lisp_new_cons (
          ctx, lisp_new_symbol (ctx, "quote"),
          lisp_new_cons (ctx, lisp_parse (ctx, text), lisp_nil ()));
    }
  return lisp_parse_atom (ctx, text);
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
      snprintf (r, 20, "%d", val.i32);
      return r;

    case LISP_TAG_INT64:
      r = lisp_malloc (ctx, 41);
      snprintf (r, 40, "%ld", val.i64);
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

static struct lisp_variable *
lisp_context_find_variable (lisp_context_t *ctx, lisp_value_ref_t name)
{
  struct lisp_variable *var;

  list_for_each_entry (var, &ctx->var_list, list)
  {
    if (lisp_sym_eq (var->name, name))
      return var;
  }

  if (ctx->parent)
    {
      return lisp_context_find_variable (ctx->parent, name);
    }

  return NULL;
}

/**
 * (set var1 value1
 *       [var2 value2
 *        ...])
 */
static lisp_value_t
lisp_set (lisp_context_t *ctx, lisp_value_ref_t params,
          struct lisp_function *unused)
{
  (void)unused;
  lisp_value_t nil = LISP_NIL;
  lisp_value_t tmp;

  lisp_value_t name;
  lisp_value_t value_exp;
  lisp_value_t value;

  struct lisp_variable *var;

  LIST_HEAD (var_list);

  lisp_dup_value (ctx, params);
  while (!LISP_IS_NIL (params))
    {
      name = lisp_car (ctx, params);
      tmp = lisp_cdr (ctx, params);
      value_exp = lisp_car (ctx, tmp);
      value = lisp_eval (ctx, value_exp);

      var = lisp_malloc (ctx, sizeof (*var));
      var->name = lisp_eval (ctx, name);
      var->value = value;

      lisp_free_value (ctx, name);
      lisp_free_value (ctx, value_exp);
      lisp_free_value (ctx, params);
      params = lisp_cdr (ctx, tmp);
      lisp_free_value (ctx, tmp);

      list_add (&var->list, &var_list);
    }
  lisp_free_value (ctx, params);

  list_splice (&var_list, &ctx->var_list);
  return nil;
}

/**
 * (set var1 value1
 *       [var2 value2
 *        ...])
 */
static lisp_value_t
lisp_setq (lisp_context_t *ctx, lisp_value_ref_t params,
           struct lisp_function *unused)
{
  (void)unused;
  lisp_value_t nil = LISP_NIL;
  lisp_value_t tmp;

  lisp_value_t name;
  lisp_value_t value_exp;
  lisp_value_t value;

  struct lisp_variable *var;

  LIST_HEAD (var_list);

  lisp_dup_value (ctx, params);
  while (!LISP_IS_NIL (params))
    {
      name = lisp_car (ctx, params);
      tmp = lisp_cdr (ctx, params);
      value_exp = lisp_car (ctx, tmp);
      value = lisp_eval (ctx, value_exp);

      var = lisp_malloc (ctx, sizeof (*var));
      var->name = name;
      var->value = value;

      lisp_free_value (ctx, value_exp);
      lisp_free_value (ctx, params);
      params = lisp_cdr (ctx, tmp);
      lisp_free_value (ctx, tmp);

      list_add (&var->list, &var_list);
    }
  lisp_free_value (ctx, params);

  list_splice (&var_list, &ctx->var_list);
  return nil;
}

static int
lisp_function_set_args (lisp_context_t *ctx, lisp_value_ref_t params,
                        lisp_value_ref_t args)
{
  lisp_value_t list = LISP_NIL;

  lisp_dup_value (ctx, params);
  lisp_dup_value (ctx, args);
  while (!LISP_IS_NIL (params))
    {
      lisp_value_t tmp;

      list = lisp_new_cons (ctx, lisp_car (ctx, args), list);
      list = lisp_new_cons (ctx, lisp_car (ctx, params), list);

      tmp = lisp_cdr (ctx, params);
      lisp_free_value (ctx, params);
      params = tmp;

      tmp = lisp_cdr (ctx, args);
      lisp_free_value (ctx, args);
      args = tmp;
    }
  lisp_free_value (ctx, args);
  lisp_free_value (ctx, params);

  lisp_setq (ctx, list, NULL);
  lisp_free_value (ctx, list);

  return 0;
}

static lisp_value_t
lisp_eval_list (lisp_context_t *ctx, lisp_value_t list)
{
  lisp_value_t val = LISP_NIL;
  lisp_dup_value (ctx, list);
  while (!LISP_IS_NIL (list))
    {
      lisp_value_t tmp;
      lisp_value_t exp = lisp_car (ctx, list);
      lisp_free_value (ctx, val);
      val = lisp_eval (ctx, exp);
      lisp_free_value (ctx, exp);
      tmp = lisp_cdr (ctx, list);
      lisp_free_value (ctx, list);
      list = tmp;
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
      = lisp_context_new_with_parent (ctx, LISP_SYMBOL_STR (func->name));
  lisp_function_set_args (new_ctx, func->params, args);
  val = lisp_eval_list (new_ctx, func->body);
  lisp_context_free (new_ctx);
  return val;
}

static lisp_value_t
lisp_new_function (lisp_context_t *ctx, lisp_value_t params, lisp_value_t body,
                   lisp_native_invoke_t invoker)
{
  LISP_DEFINE_SYMBOL (lambda, "#LAMBDA");
  struct lisp_function *fn = lisp_malloc (ctx, sizeof (*fn));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_LAMBDA, fn);

  fn->obj.ref_count = 1;
  fn->obj.ops = &lisp_function_operations;

  fn->params = params;
  fn->body = body;
  fn->invoker = invoker;

  fn->name = lisp_dup_value (ctx, lambda);

  list_add_tail (&fn->obj.list, &ctx->object_list);

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
  return 0;
}

static int
lisp_set_value_in_context (lisp_context_t *ctx, lisp_value_t name,
                           lisp_value_t value)
{
  struct lisp_variable *var = lisp_malloc (ctx, sizeof (*var));
  if (!var)
    {
      lisp_free_value (ctx, name);
      lisp_free_value (ctx, value);
      return -1;
    }
  var->name = name;
  var->value = value;

  list_add (&var->list, &ctx->var_list);

  return 0;
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

      lisp_set_function_name (ctx, func, lisp_dup_value (ctx, name));

      if (lisp_set_value_in_context (ctx, name, func))
        return lisp_exception ();
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

/**
 * (+ a b [c ...])
 */
static lisp_value_t
lisp_plus (lisp_context_t *ctx, lisp_value_ref_t args,
           struct lisp_function *unused)
{
  int32_t sum = 0;
  int32_t num = 0;
  lisp_value_t result = LISP_EXCEPTION;
  lisp_value_t list = lisp_dup_value (ctx, args);

  (void)unused;

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
      sum += num;
      list = lisp_cdr_take (ctx, list);
    }

  result = lisp_new_int32 (ctx, sum);

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
        result = lisp_nil ();
    }
  else if (lisp_sym_eq (fn->params, less))
    {
      if (n1 < n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_nil ();
    }
  else if (lisp_sym_eq (fn->params, greater_equal))
    {
      if (n1 >= n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_nil ();
    }
  else if (lisp_sym_eq (fn->params, less_equal))
    {
      if (n1 <= n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_nil ();
    }
  else if (lisp_sym_eq (fn->params, equal))
    {
      if (n1 == n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_nil ();
    }
  else if (lisp_sym_eq (fn->params, not_equal))
    {
      if (n1 != n2)
        result = lisp_dup_value (ctx, true_);
      else
        result = lisp_nil ();
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
  long long nr_objects = list_length (&ctx->object_list);
  long long nr_variables = list_length (&ctx->var_list);

  printf ("context [ name = %s, nr_objects: %lld, "
          "nr_variables: %lld ]\n",
          ctx->name, nr_objects, nr_variables);
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
    lisp_value_t cond_var = lisp_eval (ctx, cond);
    if (LISP_IS_EXCEPTION (cond_var))
      goto fail;

    if (LISP_IS_NIL (cond_var))
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

static lisp_value_t
lisp_true ()
{
  lisp_value_t val = LISP_TRUE;
  return val;
}

static lisp_value_t
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
           || LISP_IS_NIL (av) )
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
      lisp_define_function (context, name_symbol, data, invoker);             \
    }                                                                         \
  while (0)

lisp_context_t *
lisp_new_global_context (lisp_runtime_t *rt)
{
  lisp_value_t nil = LISP_NIL;
  lisp_context_t *ctx = lisp_context_new (rt, "<GLOBAL>");

  LISP_DEFINE_FUNCTION (ctx, "DEFINE", nil, &lisp_define);
  LISP_DEFINE_FUNCTION (ctx, "SET", nil, &lisp_set);
  LISP_DEFINE_FUNCTION (ctx, "SETQ", nil, &lisp_setq);
  LISP_DEFINE_FUNCTION (ctx, "QUOTE", nil, &lisp_quote);
  LISP_DEFINE_FUNCTION (ctx, "+", nil, &lisp_plus);
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

  return ctx;
}

lisp_value_t
lisp_eval (lisp_context_t *ctx, lisp_value_ref_t val)
{
  lisp_value_t r = LISP_NIL;
  if (LISP_IS_LIST (val) && !LISP_IS_NIL(val))
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

      if (func.tag != LISP_TAG_LAMBDA)
        {
          lisp_throw_internal_error (ctx, "Need a function");
          lisp_free_value (ctx, func);
          return lisp_exception ();
        }
      fn = func.ptr;

      args = lisp_cdr (ctx, val);
      r = fn->invoker (ctx, args, fn);
      lisp_free_value (ctx, args);
      lisp_free_value (ctx, func);

      return r;
    }

  if (val.tag == LISP_TAG_SYMBOL)
    {
      struct lisp_variable *var = lisp_context_find_variable (ctx, val);
      if (!var)
        return lisp_throw_internal_error (ctx, "Variable %s not found",
                                          LISP_SYMBOL_STR (val));

      return lisp_dup_value (ctx, var->value);
    }

  return val;
}
