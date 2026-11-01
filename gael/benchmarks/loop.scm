(define (run n)
  (let loop ((i 0) (sum 0))
    (if (= i n)
        sum
        (loop (+ i 1) (if (or (= (modulo i 3) 0) (= (modulo i 5) 0)) (+ sum i) sum)))))

(display (run 20000000))
(newline)
