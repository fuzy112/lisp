#ifndef LISP_H
#define LISP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum lisp_tag
{
  LISP_TAG_EXCEPTION,

  LISP_TAG_BOOLEAN,

  LISP_TAG_INT32,
  LISP_TAG_INT64,
  LISP_TAG_REAL,

  LISP_TAG_SYMBOL,
  LISP_TAG_STRING,

  LISP_TAG_LIST,

  LISP_TAG_LAMBDA,
  LISP_TAG_FUNCTION_PROXY,
};

typedef struct lisp_value
{
  union
  {
    enum lisp_tag tag;
    int tag_int;
  };
  union
  {
    int32_t i32;
    int64_t i64;
    double real;
    void *ptr;
  };
} lisp_value_t, lisp_value_ref_t;

typedef struct lisp_context lisp_context_t;
typedef struct lisp_runtime lisp_runtime_t;

lisp_runtime_t *lisp_runtime_new ();
void lisp_runtime_free (lisp_runtime_t *rt);

lisp_context_t *lisp_context_new (lisp_runtime_t *rt, const char *name);
void lisp_context_free (lisp_context_t *ctx);

lisp_runtime_t *lisp_get_runtime (lisp_context_t *ctx);

void *lisp_malloc (lisp_context_t *ctx, size_t size);
void lisp_free (lisp_context_t *ctx, void *p);

char *lisp_strdup_rt (lisp_runtime_t *rt, char const *s);

void *lisp_malloc_rt (lisp_runtime_t *ctx, size_t size);
void lisp_free_rt (lisp_runtime_t *ctx, void *p);

lisp_value_t lisp_dup_value (lisp_context_t *ctx, lisp_value_t val);
void lisp_free_value (lisp_context_t *ctx, lisp_value_t val);

lisp_value_t lisp_eval (lisp_context_t *ctx, lisp_value_ref_t exp);

#define LISP_NIL                                                              \
  {                                                                           \
    { .tag = LISP_TAG_LIST }, { .ptr = 0 }                                    \
  }
#define LISP_INT32(val)                                                       \
  {                                                                           \
    { .tag = LISP_TAG_INT32 }, { .i32 = (val) }                               \
  }
#define LISP_INT64(val)                                                       \
  {                                                                           \
    { .tag = LISP_TAG_INT64 }, { .i64 = (val) }                               \
  }
#define LISP_REAL(val)                                                        \
  {                                                                           \
    { .tag = LISP_TAG_REAL }, { .real = (val) }                               \
  }

#define LISP_EXCEPTION                                                        \
  {                                                                           \
    { .tag = LISP_TAG_EXCEPTION }, { .i32 = 0xffffffff },                     \
  }

#define LISP_TRUE                                                             \
  {                                                                           \
    { .tag = LISP_TAG_BOOLEAN }, { .i32 = 1 }                                 \
  }
#define LISP_FALSE                                                            \
  {                                                                           \
    { .tag = LISP_TAG_BOOLEAN }, { .i32 = 0 }                                 \
  }

#define LISP_OBJECT(Tag, Pointer)                                             \
  {                                                                           \
    { .tag = (Tag) }, { .ptr = (Pointer) }                                    \
  }

#define LISP_IS_EXCEPTION(val) ((val).tag == LISP_TAG_EXCEPTION)

#define LISP_IS_LIST(val) ((val).tag == LISP_TAG_LIST)
#define LISP_IS_NIL(val) ((val).tag == LISP_TAG_LIST && (val).ptr == NULL)

#define LISP_IS_ATOM(val) ((val).tag < LISP_TAG_LIST)

#define LISP_IS_SYMBOL(val) ((val).tag == LISP_TAG_SYMBOL)

#define LISP_IS_OBJECT(val)                                                   \
  ((val).tag != LISP_TAG_INT32 && (val).tag != LISP_TAG_INT64                 \
   && (val).tag != LISP_TAG_BOOLEAN && ((val).tag != LISP_TAG_EXCEPTION))

int lisp_to_bool (lisp_context_t *ctx, lisp_value_ref_t v, int *res);

lisp_value_t lisp_false (void);
lisp_value_t lisp_true (void);

lisp_value_t lisp_new_real (lisp_context_t *ctx, double v);
lisp_value_t lisp_new_int32 (lisp_context_t *ctx, int32_t v);

lisp_value_t lisp_new_cons (lisp_context_t *ctx, lisp_value_t car,
                            lisp_value_t cdr);

lisp_value_t lisp_new_symbol (lisp_context_t *ctx, const char *name);

lisp_value_t lisp_new_symbol_full (lisp_context_t *ctx, const char *name,
                                   int is_static);

lisp_value_t lisp_new_string_len (lisp_context_t *ctx, const char *n,
                                  size_t len);

lisp_value_t lisp_new_string (lisp_context_t *ctx, const char *str);

lisp_value_t lisp_new_string_full (lisp_context_t *ctx, const char *name,
                                   int is_static);

char *lisp_value_to_string (lisp_context_t *ctx, lisp_value_ref_t val);

void lisp_print_value (lisp_context_t *ctx, lisp_value_ref_t val);

int lisp_list_extract (lisp_context_t *ctx, lisp_value_ref_t list,
                       lisp_value_ref_t *heads, int n, lisp_value_ref_t *tail);

lisp_context_t *lisp_new_global_context (lisp_runtime_t *rt);

lisp_value_t lisp_throw (lisp_context_t *ctx, lisp_value_t error);

lisp_value_t lisp_throw_out_of_memory (lisp_context_t *ctx);

lisp_value_t lisp_throw_internal_error (lisp_context_t *ctx, const char *fmt,
                                        ...);

lisp_value_t lisp_get_exception (lisp_context_t *ctx);

void lisp_print_exception (lisp_context_t *ctx);

int lisp_to_int32 (lisp_context_t *ctx, int32_t *result, lisp_value_ref_t val);

typedef struct lisp_reader lisp_reader_t;

lisp_reader_t *lisp_reader_new (lisp_context_t *ctx, FILE *filep);
void lisp_reader_free (lisp_reader_t *reader);
lisp_value_t lisp_read_form (lisp_reader_t *reader);

#endif // LISP_H
