#ifndef DYNARRAY_H
#define DYNARRAY_H

#include "fls.h"

#include <stddef.h>
#include <string.h>

#define DECLARE_DYNARRAY(type, name)                                          \
  type *name;                                                                 \
  size_t name##_capacity;                                                     \
  size_t name##_length;

#define INIT_DYNARRAY(ptr, name)                                              \
  do                                                                          \
    {                                                                         \
      ptr->name = NULL;                                                       \
      ptr->name##_capacity = 0;                                               \
      ptr->name##_length = 0;                                                 \
    }                                                                         \
  while (0)

static inline int
__dynarray_add (const void *elem, void **arr, size_t size, size_t *capacity,
                size_t *length)
{
  if (*capacity < *length + 1)
    {
      size_t new_capacity = 1ul << (fls (*length) + 1);
      void *new_arr = realloc (*arr, new_capacity * size);
      if (!new_arr)
        return -1;

      *arr = new_arr;
      *capacity = new_capacity;
    }

  memmove ((char *)*arr + *length * size, elem, size);
  *length += 1;
  return 0;
}

#define dynarray_add(array, elem)                                             \
  ({                                                                          \
    __typeof__ (elem) **__ptr = &array;                                       \
    __typeof__ (elem) __elem = (elem);                                        \
    __dynarray_add (&__elem, (void **)__ptr, sizeof (array[0]),               \
                    &array##_capacity, &array##_length);                      \
  })

#endif // DYNARRAY_H
