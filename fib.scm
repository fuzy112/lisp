(define fib (lambda (n)
	      (if (< n 2)
		  n
		  (+ (fib (- n 1))
		     (fib (- n 2))))))

(define fib2
  (lambda (n)
    (define fib-iter
      (lambda (acc val count)
	(if (= count n)
	    acc
	    (fib-iter val (+ acc val) (+ count 1)))))
    (fib-iter 0 1 0)))

(display (fib 35))
(newline)
