#ifndef SYMBOL_ENUMS_H
#define SYMBOL_ENUMS_H

#ifdef LISP_SYMBOL_DEF
# error LISP_SYMBOL_DEF already defined!
#endif

#define LISP_SYMBOL_DEF(name, value)	\
	LISP_SYMBOL_##name,

enum lisp_symbol_enum
{
#include "symbols.inc"
};

#undef LISP_SYMBOL_DEF

#endif // SYMBOL_ENUMS_H
