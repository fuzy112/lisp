/* rbtree.c
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

#include "rbtree.h"
#include <assert.h>

static void
rb_rotate_right (struct rb_node *x, struct rb_root *tree)
{
  /*
          x           y
         / \         / \
        y   z   ->  a   x
       / \             / \
      a   b           b   z
  */

  struct rb_node *y;

  assert (x != NULL && x->rb_left != NULL);
  y = x->rb_left;
  struct rb_node *parent = y->rb_parent = x->rb_parent;

  if (!parent)
    {
      tree->rb_node = y;
    }
  else
    {
      if (parent->rb_left == x)
        parent->rb_left = y;
      else
        parent->rb_right = y;
    }
  x->rb_parent = y;
  struct rb_node *right = x->rb_left = y->rb_right;
  if (right)
    right->rb_parent = x;
  y->rb_right = x;
}

static void
rb_rotate_left (struct rb_node *x, struct rb_root *tree)
{
  /*
      x             y
     / \           / \
    z   y    ->   x   b
       / \       / \
      a   b     z   a
  */

  struct rb_node *y;

  assert (x != NULL && x->rb_right != NULL);
  y = x->rb_right;
  struct rb_node *parent = y->rb_parent = x->rb_parent;
  if (!parent)
    {
      tree->rb_node = y;
    }
  else
    {
      if (parent->rb_left == x)
        parent->rb_left = y;
      else
        parent->rb_right = y;
    }
  x->rb_parent = y;
  struct rb_node *left = x->rb_right = y->rb_left;
  if (left)
    left->rb_parent = x;
  y->rb_left = x;
}

struct rb_node *
rb_first (const struct rb_root *tree)
{
  return rb_min (tree->rb_node);
}

struct rb_node *
rb_last (const struct rb_root *tree)
{
  return rb_max (tree->rb_node);
}

struct rb_node *
rb_min (const struct rb_node *x)
{
  struct rb_node *y = NULL;
  while (x != NULL)
    {
      y = (struct rb_node *)x;
      x = x->rb_left;
    }
  return y;
}

struct rb_node *
rb_max (const struct rb_node *x)
{
  struct rb_node *y = NULL;
  while (x != NULL)
    {
      y = (struct rb_node *)x;
      x = x->rb_right;
    }
  return y;
}

/* Precondition: x != rb_first(root) */
struct rb_node *
rb_prev (const struct rb_node *x)
{
  struct rb_node *p;

  if (x == NULL)
    return NULL;

  if (x->rb_left != NULL)
    return rb_max (x->rb_left);

  p = x->rb_parent;
  while (p && p->rb_left == x)
    {
      x = p;
      p = x->rb_parent;
    }

  return p;
}

struct rb_node *
rb_next (const struct rb_node *x)
{
  struct rb_node *p;

  if (x == NULL)
    return NULL;

  if (x->rb_right != NULL)
    return rb_min (x->rb_right);

  p = x->rb_parent;
  while (p && p->rb_right == x)
    {
      x = p;
      p = x->rb_parent;
    }

  return p;
}

/* Link node x to parent. */
void
rb_link_node (struct rb_node *x, struct rb_node *parent, struct rb_node **link)
{
  *link = x;
  x->rb_parent = parent;
  x->rb_left = x->rb_right = NULL;
  x->rb_is_black = false;
}

/* Rebalance after inserting node x into tree root. */
void
rb_balance_insert (struct rb_node *x, struct rb_root *root)
{
  struct rb_node *parent = x->rb_parent;
  x->rb_is_black = x == root->rb_node;

  while (x != root->rb_node && !parent->rb_is_black)
    {
      struct rb_node *gparent = parent->rb_parent;

      struct rb_node *tmp = gparent->rb_right;
      if (tmp != parent)
        {
          struct rb_node *y = tmp;

          if (y != NULL && !y->rb_is_black)
            {
              /*
                Case 1: p is red & y is red

                          g
                         / \
                        p   y
                       / \
                      x   x

                      We violate the rule that a red node cannot have red
                 children. So we color p and y black, and g red. This doesn't
                 change black height, but g's parent may still be red. So we
                 continue to rebalance at g.
               */
              parent->rb_is_black = true;
              y->rb_is_black = true;
              gparent->rb_is_black = gparent == root->rb_node;
              x = gparent;
              parent = x->rb_parent;
            }
          else
            { /* y == NULL || y->is_black */
              if (x == parent->rb_right)
                {
                  /*
                    Case 2: y is black & x is p's right child.
                    x and p are red.

                           g           g
                          / \         / \
                         p   y  ->   x   y
                          \         /
                          x         p

                    Left rotate at p to transform into case 3.
                  */
                  tmp = x;
                  x = parent;
                  parent = tmp;
                  rb_rotate_left (x, root);
                } /* !rb_is_left_child_of_parent(x) */

              /*
                Case 3: y is black & x is p's left child.
                x and p are red.

                       g               p
                      / \             / \
                     p   y     ->    x   g
                    /                     \
                   x                       y

                We color g red and p black,
                And rotate right at g.
                As a result we transfer one of the two red nodes
                of the left subtree to the right subtree.
              */

              parent->rb_is_black = true;
              gparent->rb_is_black = false;
              rb_rotate_right (gparent, root);
              break;
            }
        }
      else
        { /* !rb_is_left_child_of_parent(x->rb_parent) */
          struct rb_node *y = gparent->rb_left;

          if (y != NULL && !y->rb_is_black)
            {
              parent->rb_is_black = true;
              y->rb_is_black = true;
              gparent->rb_is_black = gparent == root->rb_node;
              x = gparent;
              parent = x->rb_parent;
            }
          else
            { /* y == NULL || y->rb_is_black */
              if (x == parent->rb_left)
                {
                  tmp = x;
                  x = parent;
                  parent = tmp;
                  rb_rotate_right (x, root);
                }
              parent->rb_is_black = true;
              gparent->rb_is_black = false;
              rb_rotate_left (gparent, root);
              break;
            }
        }
    } /* while (x != root && !x->rb_parent->rb_is_black) */
}

void
rb_erase (struct rb_node *x, struct rb_root *root)
{
  /*
    y is either x or x's successor,
    which will have at most one child.
  */
  struct rb_node *y = x;

  /* z will be y's (possibly null) single child. */
  struct rb_node *z = NULL;

  /*	w is z's uncle, and will be z's sibling. */
  struct rb_node *w = NULL;

  /* y's original color. */
  bool remove_black;

  struct rb_node *parent = NULL;

  /* find y */
  if (x->rb_left && x->rb_right)
    {
      y = x->rb_right;
      while (y->rb_left)
        y = y->rb_left;
    }

  z = y->rb_left ? y->rb_left : y->rb_right;

  /* find w */
  parent = y->rb_parent;
  if (parent)
    {
      if (parent->rb_left == (y))
        w = parent->rb_right;
      else
        w = parent->rb_left;
    }

  remove_black = y->rb_is_black;

  /* remove y */
  if (z != NULL)
    z->rb_parent = parent;
  if (!parent)
    {
      root->rb_node = z;
    }
  else
    {
      if (parent->rb_left == (y))
        parent->rb_left = z;
      else
        parent->rb_right = z;
    }

  if (x != y)
    {
      /* replace x by y */
      y->rb_left = x->rb_left;
      if (x->rb_left)
        x->rb_left->rb_parent = y;
      y->rb_right = x->rb_right;
      if (x->rb_right)
        x->rb_right->rb_parent = y;
      y->rb_parent = x->rb_parent;

      parent = x->rb_parent;
      if (!parent)
        {
          root->rb_node = y;
        }
      else
        {
          if (parent->rb_left == (x))
            parent->rb_left = y;
          else
            parent->rb_right = y;
        }
      y->rb_is_black = x->rb_is_black;
    }

  if (remove_black)
    {
      /* rebalance if we removed a black node */

      for (;;)
        {
          if (root->rb_node == NULL)
            break;

          if (z == root->rb_node)
            {
              z->rb_is_black = true;
              break;
            }

          if (z != NULL && !z->rb_is_black)
            {
              /*
                 Case 1: z is red.
                 Color z black.
                 Done
              */

              z->rb_is_black = true;
              break;
            }

          assert (w != NULL);
          if (!w->rb_is_black)
            {
              /*
                 Case 2: z is black & w is red.

                         p            w
                        / \          / \
                      *z   w   ->   p  d
                          / \      / \
                          c d      z c

                 Left rotate at p to turn into other cases.
              */
              struct rb_node *p = w->rb_parent;

              if (p->rb_left == (w))
                rb_rotate_right (p, root);
              else
                rb_rotate_left (p, root);

              p->rb_is_black = false;
              p->rb_parent->rb_is_black = true;

              w = p->rb_left == z ? p->rb_right : p->rb_left;
              assert (w != NULL);
              assert (w->rb_is_black);
            }

          if ((w->rb_left == NULL || w->rb_left->rb_is_black)
              && (w->rb_right == NULL || w->rb_right->rb_is_black))
            {
              /*
                 Case 3: z and w are black, w has no red children

                       p
                      / \
                    *z   w
                        / \
                        b c

                 We decrease the black height of the right subtree by coloring
                 w red. If p is red, painting p black will increase black
                 height of both subtree. Otherwise, assign p to z and continue
                 to rebalance.
              */
              w->rb_is_black = false;
              z = w->rb_parent;
              if (z == root->rb_node)
                break;
              if (z->rb_parent->rb_left == z)
                w = z->rb_parent->rb_right;
              else
                w = z->rb_parent->rb_left;
              assert (w != NULL);
            }
          else
            {
              if (w->rb_parent->rb_right == w)
                {
                  if (w->rb_left != NULL && !w->rb_left->rb_is_black)
                    {
                      /*
                         Case 4:
                              p              p
                             / \            / \
                            z   w    ->    z   c
                               /                \
                               c                 w
                       */
                      w->rb_is_black = false;
                      w->rb_left->rb_is_black = true;
                      rb_rotate_right (w, root);
                      w = w->rb_parent;
                    }

                  /*
                     Case 5:
                         p                w
                        / \              / \
                       z   w       ->   p   d
                          / \          / \
                          c d          z c
                   */
                  w->rb_is_black = w->rb_parent->rb_is_black;
                  w->rb_right->rb_is_black = true;
                  w = w->rb_parent;
                  assert (w != NULL);
                  w->rb_is_black = true;
                  rb_rotate_left (w, root);
                  break;
                }
              else
                {
                  /* Symmetric cases 4 & 5. */
                  if (w->rb_right != NULL && !w->rb_right->rb_is_black)
                    {
                      w->rb_is_black = false;
                      w->rb_right->rb_is_black = true;
                      rb_rotate_left (w, root);
                      w = w->rb_parent;
                    }

                  w->rb_is_black = w->rb_parent->rb_is_black;
                  w->rb_left->rb_is_black = true;
                  w = w->rb_parent;
                  assert (w != NULL);
                  w->rb_is_black = true;
                  rb_rotate_right (w, root);
                  break;
                }
            }
        }
    }
}

void
rb_replace_node (struct rb_node *old, struct rb_node *new_node,
                 struct rb_root *tree)
{
  struct rb_node *parent;
  struct rb_node *left;
  struct rb_node *right;

  parent = new_node->rb_parent = old->rb_parent;
  left = new_node->rb_left = old->rb_left;
  right = new_node->rb_right = old->rb_right;
  new_node->rb_is_black = old->rb_is_black;

  if (!parent)
    {
      tree->rb_node = new_node;
    }
  else
    {
      if (parent->rb_left == old)
        parent->rb_left = new_node;
      else
        parent->rb_right = new_node;
    }

  if (left)
    left->rb_parent = new_node;
  if (right)
    right->rb_parent = new_node;

  old->rb_parent = NULL;
  old->rb_left = NULL;
  old->rb_right = NULL;
  old->rb_is_black = false;
}
