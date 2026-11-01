(defstruct particle x y vx vy)

(defvar *seed* 1)
(defun rand ()
  (setf *seed* (mod (* *seed* 16807) 2147483647)))

(defun main ()
  (let ((ps (make-array 10000)) (sum 0d0))
    (dotimes (i 10000)
      (let* ((x (float (mod (rand) 1000) 1d0)) (y (float (mod (rand) 1000) 1d0))
             (vx (float (- (mod (rand) 21) 10) 1d0)) (vy (float (- (mod (rand) 21) 10) 1d0)))
        (setf (aref ps i) (make-particle :x x :y y :vx vx :vy vy))))
    (dotimes (frame 600)
      (loop for p across ps
            do (incf (particle-x p) (* (particle-vx p) 0.5d0))
               (incf (particle-y p) (* (particle-vy p) 0.5d0))
               (when (or (< (particle-x p) 0d0) (> (particle-x p) 1000d0)) (setf (particle-vx p) (- 0d0 (particle-vx p))))
               (when (or (< (particle-y p) 0d0) (> (particle-y p) 1000d0)) (setf (particle-vy p) (- 0d0 (particle-vy p))))))
    (loop for p across ps do (incf sum (+ (particle-x p) (particle-y p))))
    (format t "~d~%" (truncate sum))))

(main)
