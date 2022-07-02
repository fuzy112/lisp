#include "string_buf.h"
#include <stdlib.h>
#include <string.h>

void
string_buf_destroy (struct string_buf *buf)
{
  free (buf->s);
}

int
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

int
string_buf_append_char (struct string_buf *buf, int ch)
{
  char c = ch;

  return string_buf_append (buf, &c, 1);
}