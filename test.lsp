(defun fib (n)
	(if (< n 2)
		n
		(+ (fib (- n 1)) (fib (- n 2)))))

(defun fib-1 (n)
	(defun fib-iter (cur last i n)
		(if (!= i n)
			(fib-iter (+ cur last) cur (+ 1 i) n)
			cur))
	(fib-iter 1 0 1 n))

(print (fib-1 25))

