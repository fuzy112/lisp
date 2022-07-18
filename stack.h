#ifndef STACK_H
#define STACK_H

#include <string.h>

struct stack
{
  char *data;
  size_t size;
  size_t capacity;
};

static inline void
stack_init (struct stack *stk)
{
  stk->data = NULL;
  stk->size = 0;
  stk->capacity = 0;
}

static inline void
stack_destroy (struct stack *stk)
{
  free (stk->data);
  stk->data = NULL;
  stk->size = NULL;
  stk->capacity = NULL;
}

static inline void *
stack_pointer (struct stack *stk)
{
  return stk->data + stk->size;
}

static inline int
stack_push (struct stack *stk, const void *buf, size_t size)
{
  if (stk->size + size > stk->capacity)
    {
      size_t new_capacity = (stk->size + size) * 2;
      char *new_data = realloc (stk->data, new_capacity);
      if (!new_data)
        return -1;
      stk->data = new_data;
      stk->capacity = new_capacity;
    }

  memmove (stack_pointer (stk), buf, size);
  stk->size += size;
  return 0;
}

static inline void *
stack_top (struct stack *stk, size_t size)
{
  if (stk->size < size)
    return NULL;

  return stack_pointer (stk) - size;
}

static inline void *
stack_pop (struct stack *stk, size_t size)
{
  void *ret = stack_top (stk, size);
  if (ret)
    {
      stk->size -= size;
    }
  return ret;
}

#endif // STACK_H
