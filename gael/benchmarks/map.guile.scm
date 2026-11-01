; Guile's own hash tables
(define (main)
  (let ((m (make-hash-table)))
    (do ((i 0 (+ i 1))) ((= i 1000000)) (hashv-set! m (modulo (* i 7919) 1000003) i))
    (let loop ((i 0) (sum 0))
      (if (= i 1000000)
          (begin (display (hash-count (lambda (k v) #t) m)) (display " ") (display sum) (newline))
          (loop (+ i 1) (+ sum (hashv-ref m i -1)))))))

(main)
