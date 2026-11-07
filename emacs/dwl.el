;;; dwl.el --- drive dwl from Emacs through its command fifo  -*- lexical-binding: t -*-
;; dwl reads one command per line from $XDG_RUNTIME_DIR/dwl.cmd:
;;   focus next|prev, view N, tag N, toggletag N, layout N, kill, float, zoom, quit, spawn CMD

(defun dwl--fifo ()
  (expand-file-name "dwl.cmd" (getenv "XDG_RUNTIME_DIR")))

(defun dwl-send (fmt &rest args)
  "Send one command line to dwl."
  (unless (file-exists-p (dwl--fifo)) (user-error "dwl fifo missing: not running under dwl?"))
  (write-region (concat (apply #'format fmt args) "\n") nil (dwl--fifo) t 'silent))

(defun dwl-view (n) (interactive "nTag: ") (dwl-send "view %d" n))
(defun dwl-tag (n) (interactive "nMove window to tag: ") (dwl-send "tag %d" n))
(defun dwl-focus-next () (interactive) (dwl-send "focus next"))
(defun dwl-focus-prev () (interactive) (dwl-send "focus prev"))
(defun dwl-kill () (interactive) (dwl-send "kill"))
(defun dwl-float () (interactive) (dwl-send "float"))
(defun dwl-layout (n) (interactive "nLayout index: ") (dwl-send "layout %d" n))
(defun dwl-spawn (cmd) (interactive "sRun: ") (dwl-send "spawn %s" cmd))

(provide 'dwl)
