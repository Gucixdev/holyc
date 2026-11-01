(defun run (n)
  (let ((sum 0))
    (dotimes (i n sum)
      (when (or (zerop (mod i 3)) (zerop (mod i 5))) (incf sum i)))))

(format t "~d~%" (run 20000000))
