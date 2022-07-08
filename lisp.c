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
#include <time.h>

#define LISP_VAR_TABLE_BITS 8

struct lisp_object;

struct lisp_object_operations
{
  void (*finalize) (lisp_runtime_t *rt, struct lisp_object *);
  void (*gc_mark) (lisp_runtime_t *rt, struct lisp_object *,
                   void mark_func (lisp_runtime_t *rt, struct lisp_object *));
};

enum lisp_mark_color
{
  LISP_MARK_BLACK,
  LISP_MARK_WHITE,
  LISP_MARK_GRAY,
  LISP_MARK_HATCH,
};

struct lisp_object
{
  long ref_count;
  const struct lisp_object_operations *ops;
  struct list_head list;
  int mark_flag;
};

struct lisp_runtime
{
  long gc_threshold;
  long nr_blocks;
  lisp_value_t exception_list;
  struct list_head gc_list;
  struct list_head tmp_list;
  time_t last_gc;
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

  lisp_value_t name;

  lisp_context_t *ctx;
};

static void
lisp_object_unref (lisp_runtime_t *rt, struct lisp_object *obj)
{
  assert (obj);
  if (--obj->ref_count > 0)
    {
      obj->mark_flag = LISP_MARK_HATCH;
      list_move_tail (&obj->list, &rt->gc_list);
      return;
    }

  list_del (&obj->list);

  if (obj->ops->gc_mark)
    {
      obj->ops->gc_mark (rt, obj, lisp_object_unref);
    }

  if (obj->ops->finalize)
    {
      obj->ops->finalize (rt, obj);
    }

  lisp_free_rt (rt, obj);
}

static void
lisp_function_gc_mark (lisp_runtime_t *rt, struct lisp_object *obj,
                       void mark_func (lisp_runtime_t *rt,
                                       struct lisp_object *))
{
  struct lisp_function *func = (struct lisp_function *)obj;

  lisp_mark_value (rt, func->body, mark_func);
  lisp_mark_value (rt, func->name, mark_func);
  lisp_mark_value (rt, func->params, mark_func);

  mark_func (rt, &func->ctx->obj);
}

static const struct lisp_object_operations lisp_function_operations = {
  .gc_mark = &lisp_function_gc_mark,
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

lisp_runtime_t *
lisp_runtime_new (void)
{
  lisp_runtime_t *rt = malloc (sizeof (*rt));
  rt->nr_blocks = 0;
  rt->exception_list = lisp_nil ();
  INIT_LIST_HEAD (&rt->gc_list);
  rt->last_gc = time (NULL);
  rt->gc_threshold = 128;
  return rt;
}

void
lisp_runtime_free (lisp_runtime_t *rt)
{
  assert (rt->nr_blocks == 0);
  assert (list_empty (&rt->gc_list));
  free (rt);
}

void
lisp_mark_value (lisp_runtime_t *rt, lisp_value_ref_t val,
                 void mark_func (lisp_runtime_t *rt, struct lisp_object *))
{
  if (LISP_IS_OBJECT (val))
    {
      struct lisp_object *obj = val.ptr;
      if (!obj)
        return;
      mark_func (rt, obj);
    }
}

static void lisp_paint_gray (lisp_runtime_t *rt, struct lisp_object *obj);

void
lisp_decref_paint_gray (lisp_runtime_t *rt, struct lisp_object *obj)
{
  obj->ref_count--;
  lisp_paint_gray (rt, obj);
}

void
lisp_paint_gray (lisp_runtime_t *rt, struct lisp_object *obj)
{
  if (obj->mark_flag == LISP_MARK_HATCH || obj->mark_flag == LISP_MARK_BLACK)
    {
      obj->mark_flag = LISP_MARK_GRAY;

      if (obj->ops->gc_mark)
        obj->ops->gc_mark (rt, obj, lisp_decref_paint_gray);
    }
}

static void lisp_paint_black (lisp_runtime_t *rt, struct lisp_object *obj);

void
lisp_incref_paint_black (lisp_runtime_t *rt, struct lisp_object *obj)
{
  obj->ref_count++;

  if (obj->mark_flag != LISP_MARK_BLACK)
    {
      lisp_paint_black (rt, obj);
    }
}

void
lisp_paint_black (lisp_runtime_t *rt, struct lisp_object *obj)
{
  obj->mark_flag = LISP_MARK_BLACK;
  if (obj->ops->gc_mark)
    obj->ops->gc_mark (rt, obj, lisp_incref_paint_black);
}

void
lisp_scan_gray (lisp_runtime_t *rt, struct lisp_object *obj)
{
  if (obj->mark_flag == LISP_MARK_GRAY)
    {
      if (obj->ref_count > 0)
        {
          lisp_paint_black (rt, obj);
        }
      else
        {
          obj->mark_flag = LISP_MARK_WHITE;
          if (obj->ops->gc_mark)
            obj->ops->gc_mark (rt, obj, lisp_scan_gray);
        }
    }
}

void
lisp_collect_white (lisp_runtime_t *rt, struct lisp_object *obj)
{
  assert (obj);
  if (obj->mark_flag == LISP_MARK_WHITE)
    {
      obj->mark_flag = LISP_MARK_BLACK;
      if (obj->ops->gc_mark)
        obj->ops->gc_mark (rt, obj, lisp_collect_white);

      // reclaim
      list_move (&obj->list, &rt->tmp_list);
      if (obj->ops->finalize)
        {
          obj->ops->finalize (rt, obj);
        }
    }
}

void
lisp_scan_gc_list (lisp_runtime_t *rt)
{
  struct lisp_object *obj, *tmp;

  INIT_LIST_HEAD (&rt->tmp_list);

  while (!list_empty (&rt->gc_list))
    {
      obj = list_first_entry (&rt->gc_list, struct lisp_object, list);
      list_del_init (&obj->list);

      if (obj->mark_flag == LISP_MARK_HATCH)
        {
          lisp_paint_gray (rt, obj);
          lisp_scan_gray (rt, obj);

          lisp_collect_white (rt, obj);

          break;
        }
    }

  list_for_each_entry_safe (obj, tmp, &rt->tmp_list, list)
    {
      lisp_free_rt (rt, obj);
    }

  rt->last_gc = time (NULL);
  if (rt->nr_blocks >= rt->gc_threshold)
    {
      rt->gc_threshold *= 2;
    }
}

void
lisp_gc_rt (lisp_runtime_t *rt)
{
  struct lisp_object *obj, *tmp;

  INIT_LIST_HEAD (&rt->tmp_list);

  while (!list_empty (&rt->gc_list))
    {
      obj = list_first_entry (&rt->gc_list, struct lisp_object, list);
      list_del_init (&obj->list);

      if (obj->mark_flag == LISP_MARK_HATCH)
        {
          lisp_paint_gray (rt, obj);
          lisp_scan_gray (rt, obj);
          lisp_collect_white (rt, obj);
        }
    }

  list_for_each_entry_safe (obj, tmp, &rt->tmp_list, list)
    {
      lisp_free_rt (rt, obj);
    }

  rt->last_gc = time (NULL);
  if (rt->nr_blocks >= rt->gc_threshold)
    {
      rt->gc_threshold *= 2;
    }
}

static void
lisp_context_finalize (lisp_runtime_t *rt, struct lisp_object *obj)
{
  struct lisp_variable *var;
  struct hlist_node *tmp;
  size_t bkt;
  lisp_context_t *ctx = (lisp_context_t *)obj;
  lisp_free_rt (rt, ctx->name);

  hash_for_each_safe (ctx->var_table, bkt, tmp, var, node)
    {
      lisp_free_rt (rt, var);
    }
}

static void
lisp_context_gc_mark (lisp_runtime_t *rt, struct lisp_object *obj,
                      void mark_func (lisp_runtime_t *rt,
                                      struct lisp_object *))
{
  struct lisp_variable *var;
  size_t bkt;
  lisp_context_t *ctx = (lisp_context_t *)obj;

  hash_for_each (ctx->var_table, bkt, var, node)
    {
      lisp_mark_value (rt, var->name, mark_func);
      lisp_mark_value (rt, var->value, mark_func);
    }

  if (ctx->parent)
    mark_func (rt, &ctx->parent->obj);
}

static const struct lisp_object_operations lisp_context_operations = {
  .finalize = lisp_context_finalize,
  .gc_mark = lisp_context_gc_mark,
};

lisp_context_t *
lisp_context_new (lisp_runtime_t *rt, const char *name)
{
  lisp_context_t *ctx = lisp_malloc_rt (rt, sizeof (*ctx));
  if (!ctx)
    return NULL;
  ctx->obj.ref_count = 1;
  ctx->obj.ops = &lisp_context_operations;
  INIT_LIST_HEAD (&ctx->obj.list);
  ctx->obj.mark_flag = LISP_MARK_BLACK;
  hash_init (ctx->var_table);
  ctx->parent = NULL;
  ctx->runtime = rt;
  ctx->name = lisp_strdup_rt (rt, name);
  return ctx;
}

lisp_context_t *
lisp_context_ref (lisp_context_t *ctx)
{
  ctx->obj.ref_count++;
  return ctx;
}

void
lisp_context_unref (lisp_context_t *ctx)
{
  lisp_object_unref (lisp_get_runtime (ctx), &ctx->obj);
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

  if (now - rt->last_gc > 1 || rt->nr_blocks > rt->gc_threshold)
    {
      lisp_scan_gc_list (rt);
    }

  ptr = calloc (1, size);
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
  lisp_mark_value (lisp_get_runtime (ctx), val, lisp_object_unref);
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

static void
lisp_cons_gc_mark (lisp_runtime_t *rt, struct lisp_object *obj,
                   void mark_func (lisp_runtime_t *, struct lisp_object *))
{
  struct lisp_cons *cons = (struct lisp_cons *)obj;
  lisp_mark_value (rt, cons->car, mark_func);
  lisp_mark_value (rt, cons->cdr, mark_func);
}

static const struct lisp_object_operations lisp_cons_operations = {
  .gc_mark = lisp_cons_gc_mark,
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
  cons->obj.mark_flag = LISP_MARK_BLACK;
  INIT_LIST_HEAD (&cons->obj.list);

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
lisp_symbol_finalize (lisp_runtime_t *rt, struct lisp_object *obj)
{
  struct lisp_symbol *sym = (struct lisp_symbol *)obj;
  if (!sym->is_static)
    lisp_free_rt (rt, sym->name);
}

static const struct lisp_object_operations lisp_symbol_operations = {
  .finalize = &lisp_symbol_finalize,
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
  sym->obj.mark_flag = LISP_MARK_BLACK;
  INIT_LIST_HEAD (&sym->obj.list);

  return val;
}

lisp_value_t
lisp_new_symbol (lisp_context_t *ctx, const char *name)
{
  return lisp_new_symbol_full (ctx, name, 0);
}

#define LISP_SYMBOL_OBJECT_INIT(name, value)                                  \
  {                                                                           \
    { 1, &lisp_symbol_operations, LIST_HEAD_INIT (name.obj.list), 0 }, 1,     \
        (char *)(value)                                                       \
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
lisp_string_finalize (lisp_runtime_t *rt, struct lisp_object *obj)
{
  struct lisp_string *str = (struct lisp_string *)obj;
  if (!str->is_static)
    lisp_free_rt (rt, str->str);
}

static const struct lisp_object_operations lisp_string_operations = {
  .finalize = &lisp_string_finalize,
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
  str->obj.mark_flag = LISP_MARK_BLACK;
  INIT_LIST_HEAD (&str->obj.list);

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
    { 1, &lisp_string_operations, LIST_HEAD_INIT (name.obj.list), 0 }, 1,     \
        (char *)(value)                                                       \
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
  // struct lisp_object *obj;

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

    case LISP_TAG_STRING:
      {
        struct lisp_string *s = val.ptr;
        return lisp_strdup (ctx, s->str);
      }

    case LISP_TAG_SYMBOL:
      return lisp_strdup (ctx, LISP_SYMBOL_STR (val));

    case LISP_TAG_LIST:
      if (LISP_IS_NIL (val))
        return lisp_strdup (ctx, "()");
      /* fall through */

    default:

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
  new_ctx->parent = lisp_context_ref (parent);
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
            return lisp_dup_value (ctx, var->value);
        }

      ctx = ctx->parent;
    }

  return lisp_throw_internal_error (orig_ctx, "no such variable");
}

static lisp_value_t
lisp_context_set_var (lisp_context_t *ctx, lisp_value_ref_t name,
                      lisp_value_t value)
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
            {
              lisp_free_value (ctx, var->value);
              var->value = value;

              return lisp_nil ();
            }
        }

      ctx = ctx->parent;
    }

  return lisp_throw_internal_error (orig_ctx, "no such variable");
}

int
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
  lisp_free_value (ctx, name);
  lisp_free_value (ctx, value);
  return -1;
}

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
      if (lisp_context_define_var (new_ctx, name, value))
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
  lisp_context_unref (new_ctx);
  return val;
}

static lisp_value_t
lisp_new_function (lisp_context_t *ctx, lisp_value_t name, lisp_value_t params,
                   lisp_value_t body, lisp_native_invoke_t invoker)
{
  struct lisp_function *fn = lisp_malloc (ctx, sizeof (*fn));
  lisp_value_t val = LISP_OBJECT (LISP_TAG_LAMBDA, fn);

  fn->obj.ref_count = 1;
  fn->obj.ops = &lisp_function_operations;
  fn->obj.mark_flag = LISP_MARK_BLACK;
  INIT_LIST_HEAD (&fn->obj.list);

  fn->params = params;
  fn->body = body;
  fn->invoker = invoker;

  fn->name = name;

  fn->ctx = lisp_context_new_with_parent (ctx, LISP_SYMBOL_STR (name));

  return val;
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
      lisp_value_t func = lisp_new_function (ctx, name, params, body,
                                             &lisp_function_invoker);
      lisp_free_value (ctx, sig);

      // lisp_set_function_name (ctx, func, lisp_dup_value (ctx, name));
      if (lisp_context_define_var (ctx, lisp_dup_value (ctx, name), func))
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
      if (lisp_context_define_var (ctx, sig, value))
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

static int
lisp_do_dump_context (lisp_context_t *ctx)
{
  size_t bucket;
  long long nr_variables = 0;
  struct lisp_variable *var;

  hash_for_each (ctx->var_table, bucket, var, node)
    {
      nr_variables += 1;
    }

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

  LISP_DEFINE_SYMBOL (name, "#LAMBDA");
  (void)unused;

  params = lisp_car (ctx, args);
  body = lisp_cdr (ctx, args);

  val = lisp_new_function (ctx, lisp_dup_value (ctx, name), params, body,
                           lisp_function_invoker);

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
  return lisp_context_define_var (
      ctx, name,
      lisp_new_function (ctx, lisp_dup_value (ctx, name),
                         lisp_dup_value (ctx, name), data, invoker));
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
  lisp_context_t *r;

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

  lisp_context_define_var (ctx, lisp_new_symbol (ctx, "#T"), lisp_true ());
  lisp_context_define_var (ctx, lisp_new_symbol (ctx, "#F"), lisp_false ());
  lisp_context_define_var (ctx, lisp_new_symbol (ctx, "NIL"), lisp_nil ());

  r = lisp_context_new_with_parent (ctx, "TOP-LEVEL");
  lisp_context_unref (ctx);

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
      return lisp_context_get_var (ctx, val);
    }

  return val;
}
