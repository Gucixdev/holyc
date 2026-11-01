(defun sieve (n)
  (let ((flags (make-array n :initial-element t)) (count 0))
    (loop for i from 2 below n
          when (aref flags i)
            do (incf count)
               (loop for k from (* i i) below n by i do (setf (aref flags k) nil)))
    count))

(format t "~d~%" (sieve 5000000))
