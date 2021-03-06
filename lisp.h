#ifndef LISP_H
#define LISP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum lisp_value_tag {
  LISP_TAG_EXCEPTION = 1,
  LISP_TAG_VOID = 2,
  LISP_TAG_INT = 3,
  LISP_TAG_BOOL = 4,
};

typedef union {
  struct {
    uintptr_t tag : 3;
    uintptr_t i : 61;
  };
  void *ptr;
} lisp_value_t, lisp_value_ref_t;

typedef struct lisp_context lisp_context_t;
typedef struct lisp_runtime lisp_runtime_t;

lisp_runtime_t *lisp_runtime_new ();
void lisp_runtime_free (lisp_runtime_t *rt);

lisp_context_t *lisp_context_new (lisp_runtime_t *rt, const char *name);

lisp_runtime_t *lisp_get_runtime (lisp_context_t *ctx);

void *lisp_malloc (lisp_context_t *ctx, size_t size);
char *lisp_strdup_rt (lisp_runtime_t *rt, char const *s);

void *lisp_malloc_rt (lisp_runtime_t *ctx, size_t size);

lisp_value_t lisp_eval (lisp_context_t *ctx, lisp_value_ref_t exp);

#define LISP_NIL (lisp_value_t) { .ptr = NULL }

#define LISP_INT32(val)                                                       \
  (lisp_value_t){							\
    .tag = LISP_TAG_INT, .i = (val)					\
  }

#define LISP_EXCEPTION                                                        \
  (lisp_value_t){							\
    .tag = LISP_TAG_EXCEPTION,  .i = 0xffffff				\
      }

#define LISP_TRUE                                                             \
  (lisp_value_t) {							\
    .tag = LISP_TAG_BOOL, .i = 1					\
      }
#define LISP_FALSE					\
  (lisp_value_t) { .tag = LISP_TAG_BOOL, .i = 0 }

#define LISP_IS_EXCEPTION(val) ((val).tag == LISP_TAG_EXCEPTION)

#define LISP_IS_NIL(val) ((val).ptr == NULL)

#define LISP_IS_PTR(val)			\
  ((val).tag == 0)

#define LISP_IS_INT(val) ((val).tag == LISP_TAG_INT)

#define LISP_IS_BOOL(val) ((val).tag == LISP_TAG_BOOL)

int lisp_to_bool (lisp_context_t *ctx, lisp_value_ref_t v, int *res);

lisp_value_t lisp_false (void);
lisp_value_t lisp_true (void);

lisp_value_t lisp_new_real (lisp_context_t *ctx, double v);
lisp_value_t lisp_new_int32 (lisp_context_t *ctx, int32_t v);

lisp_value_t lisp_new_cons (lisp_context_t *ctx, lisp_value_t car,
                            lisp_value_t cdr);

lisp_value_t lisp_interned_symbol (lisp_context_t *ctx, const char *name);

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
