project('lisp', 'c',
  version : '0.1',
  default_options : ['warning_level=2'])

gc_dep = dependency('bdw-gc')

executable('lisp',
           'lisp.c',
           'main.c',
           'string_buf.c',
	   'rbtree.c',
           dependencies : [gc_dep],
           c_args : ['-std=gnu99', '-Werror=implicit-function-declaration',
              '-Wno-unused-parameter'],
           install : true)
