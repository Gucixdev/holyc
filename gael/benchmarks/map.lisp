(defun main ()
  (let ((m (make-hash-table)) (sum 0))
    (dotimes (i 1000000) (setf (gethash (mod (* i 7919) 1000003) m) i))
    (dotimes (i 1000000) (incf sum (gethash i m -1)))
    (format t "~d ~d~%" (hash-table-count m) sum)))

(main)
