
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(let loop ((n 5))
  (if (= n 0)
    'fib20timed
    (begin 
      (display 'fib20time=) 
      (display (let ((t (ms))) (fib 20) (- (ms) t)))
      (newline)
      (loop (- n 1)))))
