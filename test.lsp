(define (fib n)
    (if (< n 2)
	n
	(+ (fib (- n 1)) (fib (- n 2)))))

(define (fib-1 n)
  (define (fib-iter cur last i n)
    (if (!= i n)
	(fib-iter (+ cur last) cur (+ 1 i) n)
	cur))
  (fib-iter 1 0 1 n))

(print (fib-1 25))

(define my-lambda (lambda (y) (+ y 1)))
(print (my-lambda 10))

(print (fib 10))

(print (fib 10))
(print (fib 10))

(print '(a . b))
(print (cdr '(a . b)))
(print (cdr '(a)))

(print (cons 'a 'b))

(print (null? ()))
(if (null? ()) (print 1) (print 2))

(define (my-cdr lst)
  (print lst)
  (cdr lst)
  )

(define (length lst)
  (if (null? lst)
      0
      (+ 1 (length (cdr lst)))))
(print (length '(1 2 3)))

(define (sum lst)
  (if (null? lst)
      0
      (+ (sum (cdr lst)) (car lst))
      ))
(print (sum (list 1 2 3 4)))
(print (sum '(1 2 3 4)))

(print (zero? 1))
(print (zero? 0))

(print (zero? ()))
(print (atom? 0))
(print (atom? #t))
(print (atom? (list 1 2)))
