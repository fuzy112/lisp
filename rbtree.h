/* rbtree.h
 *
 * Copyright 2022 Zhengyi Fu <tsingyat@outlook.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RBTREE_H
#define RBTREE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef container_of
#define container_of(ptr, type, member)                                       \
  ((type *)((char *)ptr - offsetof (type, member)))
#endif

  struct rb_root
  {
    struct rb_node *rb_node;
  };

  struct rb_node
  {
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    struct rb_node *rb_parent;
    bool rb_is_black;
  };

// clang-format off
#define RB_ROOT_INIT   { NULL }
  // clang-format on

  static inline void
  rb_root_init (struct rb_root *tree)
  {
    tree->rb_node = NULL;
  }

  void rb_link_node (struct rb_node *rb_node, struct rb_node *rb_parent,
                     struct rb_node **rb_link);

  struct rb_node *rb_first (const struct rb_root *tree);
  struct rb_node *rb_last (const struct rb_root *tree);
  struct rb_node *rb_max (const struct rb_node *x);
  struct rb_node *rb_min (const struct rb_node *x);
  struct rb_node *rb_next (const struct rb_node *x);
  struct rb_node *rb_prev (const struct rb_node *x);

  static inline bool
  rb_empty (const struct rb_root *tree)
  {
    return tree->rb_node == NULL;
  }

  static inline struct rb_node *
  rb_next_safe (const struct rb_node *n, const struct rb_root *tree)
  {
    return (n == NULL) ? NULL : rb_next (n);
  }

#define rb_for_each(pos, tree)                                                \
  for ((pos) = rb_first ((tree)); (pos) != NULL; (pos) = rb_next ((pos)))

#define rb_for_each_safe(pos, n, tree)                                        \
  for ((pos) = rb_first ((tree)), (n) = rb_next_safe ((pos), (tree));         \
       (pos) != NULL; (pos) = (n), (n) = rb_next_safe ((pos), (tree)))

#define rb_entry(ptr, type, member) container_of (ptr, type, member)

#if __STDC_VERSION__ >= 199901L || __cplusplus

#define rb_for_each_init(pos, tree)                                           \
  for (struct rb_node *pos = rb_first ((tree)); pos != NULL;                  \
       pos = rb_next ((pos)))

#define rb_for_each_safe_init(pos, tree)                                      \
  for (struct rb_node *pos = rb_first ((tree)),                               \
                      *_rb_next = rb_next_safe ((pos), (tree));               \
       pos != NULL;                                                           \
       (pos) = _rb_next, _rb_next = rb_next_safe ((pos), (tree)))

#endif

  void rb_balance_insert (struct rb_node *x, struct rb_root *root);

  void rb_erase (struct rb_node *x, struct rb_root *root);

  void rb_replace_node (struct rb_node *old, struct rb_node *new_node,
                        struct rb_root *tree);

  static inline void
  rb_add (struct rb_node *x, struct rb_root *root,
          bool (*less) (const struct rb_node *, const struct rb_node *))
  {
    struct rb_node *parent = NULL;
    struct rb_node **link = &root->rb_node;

    while (*link != NULL)
      {
        parent = *link;

        if (less (x, parent))
          link = &parent->rb_left;
        else
          link = &parent->rb_right;
      }

    rb_link_node (x, parent, link);
    rb_balance_insert (x, root);
  }

  static inline struct rb_node *
  rb_find (const void *key, struct rb_root *root,
           int (*comp) (const void *, const struct rb_node *))
  {
    struct rb_node *node = root->rb_node;
    while (node != NULL)
      {
        int c = comp (key, node);

        if (c < 0)
          node = node->rb_left;
        else if (c > 0)
          node = node->rb_right;
        else
          return node;
      }
    return NULL;
  }

  static inline struct rb_node *
  rb_find_or_insert (const void *key, struct rb_root *root,
                     struct rb_node *node,
                     int (*comp) (const void *, const struct rb_node *))
  {
    struct rb_node *parent = NULL;
    struct rb_node **link = &root->rb_node;
    int c;

    while (*link != NULL)
      {
        parent = *link;
        c = comp (key, parent);
        if (c < 0)
          link = &parent->rb_left;
        else if (c > 0)
          link = &parent->rb_right;
        else
          return parent;
      }

    rb_link_node (node, parent, link);
    rb_balance_insert (node, root);
    return NULL;
  }

#ifdef __cplusplus
}
#endif

#endif // !RBTREE_H
