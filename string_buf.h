#ifndef STRING_BUF_H
#define STRING_BUF_H

#include <stddef.h>

struct string_buf
{
  char *s;
  size_t capacity;
  size_t length;
};

static inline void
string_buf_init (struct string_buf *buf)
{
  buf->s = NULL;
  buf->capacity = 0;
  buf->length = 0;
}

void string_buf_destory (struct string_buf *buf);

int string_buf_append (struct string_buf *buf, const char *str, size_t len);

int string_buf_append_char (struct string_buf *buf, int ch);

#endif // STRING_BUF_H
