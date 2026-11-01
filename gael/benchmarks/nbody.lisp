(setf *read-default-float-format* 'double-float) ; 1.0 below is a double

(defstruct body x y z vx vy vz mass)

(defconstant +pi+ 3.141592653589793)
(defparameter *solar-mass* (* 4.0 +pi+ +pi+))
(defparameter *days* 365.24)

(defparameter *bodies*
  (vector
   (make-body :x 0.0 :y 0.0 :z 0.0 :vx 0.0 :vy 0.0 :vz 0.0 :mass *solar-mass*)
   (make-body :x 4.84143144246472090e+00 :y -1.16032004402742839e+00 :z -1.03622044471123109e-01
              :vx (* 1.66007664274403694e-03 *days*) :vy (* 7.69901118419740425e-03 *days*)
              :vz (* -6.90460016972063023e-05 *days*) :mass (* 9.54791938424326609e-04 *solar-mass*))
   (make-body :x 8.34336671824457987e+00 :y 4.12479856412430479e+00 :z -4.03523417114321381e-01
              :vx (* -2.76742510726862411e-03 *days*) :vy (* 4.99852801234917238e-03 *days*)
              :vz (* 2.30417297573763929e-05 *days*) :mass (* 2.85885980666130812e-04 *solar-mass*))
   (make-body :x 1.28943695621391310e+01 :y -1.51111514016986312e+01 :z -2.23307578892655734e-01
              :vx (* 2.96460137564761618e-03 *days*) :vy (* 2.37847173959480950e-03 *days*)
              :vz (* -2.96589568540237556e-05 *days*) :mass (* 4.36624404335156298e-05 *solar-mass*))
   (make-body :x 1.53796971148509165e+01 :y -2.59193146099879641e+01 :z 1.79258772950371181e-01
              :vx (* 2.68067772490389322e-03 *days*) :vy (* 1.62824170038242295e-03 *days*)
              :vz (* -9.51592254519715870e-05 *days*) :mass (* 5.15138902046611451e-05 *solar-mass*))))

(defun energy (bodies)
  (let ((e 0.0) (n (length bodies)))
    (dotimes (i n e)
      (let ((b (aref bodies i)))
        (incf e (* 0.5 (body-mass b) (+ (* (body-vx b) (body-vx b)) (* (body-vy b) (body-vy b)) (* (body-vz b) (body-vz b)))))
        (loop for j from (+ i 1) below n
              do (let* ((c (aref bodies j))
                        (dx (- (body-x b) (body-x c))) (dy (- (body-y b) (body-y c))) (dz (- (body-z b) (body-z c))))
                   (decf e (/ (* (body-mass b) (body-mass c)) (sqrt (+ (* dx dx) (* dy dy) (* dz dz)))))))))))

(defun advance (bodies dt)
  (let ((n (length bodies)))
    (dotimes (i n)
      (let ((b (aref bodies i)))
        (loop for j from (+ i 1) below n
              do (let* ((c (aref bodies j))
                        (dx (- (body-x b) (body-x c))) (dy (- (body-y b) (body-y c))) (dz (- (body-z b) (body-z c)))
                        (d2 (+ (* dx dx) (* dy dy) (* dz dz)))
                        (mag (/ dt (* d2 (sqrt d2)))))
                   (decf (body-vx b) (* dx (body-mass c) mag))
                   (decf (body-vy b) (* dy (body-mass c) mag))
                   (decf (body-vz b) (* dz (body-mass c) mag))
                   (incf (body-vx c) (* dx (body-mass b) mag))
                   (incf (body-vy c) (* dy (body-mass b) mag))
                   (incf (body-vz c) (* dz (body-mass b) mag))))))
    (dotimes (i n)
      (let ((b (aref bodies i)))
        (incf (body-x b) (* dt (body-vx b)))
        (incf (body-y b) (* dt (body-vy b)))
        (incf (body-z b) (* dt (body-vz b)))))))

(defun main ()
  (let ((px 0.0) (py 0.0) (pz 0.0) (sun (aref *bodies* 0)))
    (loop for b across *bodies*
          do (incf px (* (body-vx b) (body-mass b)))
             (incf py (* (body-vy b) (body-mass b)))
             (incf pz (* (body-vz b) (body-mass b))))
    (setf (body-vx sun) (- 0.0 (/ px *solar-mass*))
          (body-vy sun) (- 0.0 (/ py *solar-mass*))
          (body-vz sun) (- 0.0 (/ pz *solar-mass*))))
  (let ((before (energy *bodies*)))
    (dotimes (k 200000) (advance *bodies* 0.01))
    (format t "~d ~d~%" (truncate (* before 1000000000.0)) (truncate (* (energy *bodies*) 1000000000.0)))))

(main)
