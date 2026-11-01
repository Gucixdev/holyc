; R6RS hash tables, as Chez has them
(define (main)
  (let ((m (make-eqv-hashtable)))
    (do ((i 0 (+ i 1))) ((= i 1000000)) (hashtable-set! m (modulo (* i 7919) 1000003) i))
    (let loop ((i 0) (sum 0))
      (if (= i 1000000)
          (begin (display (hashtable-size m)) (display " ") (display sum) (newline))
          (loop (+ i 1) (+ sum (hashtable-ref m i -1)))))))

(main)
