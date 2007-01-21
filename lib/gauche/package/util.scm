;;;
;;; gauche.package.util - internal utilities used in package manager
;;;  
;;;   Copyright (c) 2004 Shiro Kawai, All rights reserved.
;;;   
;;;   Redistribution and use in source and binary forms, with or without
;;;   modification, are permitted provided that the following conditions
;;;   are met:
;;;   
;;;   1. Redistributions of source code must retain the above copyright
;;;      notice, this list of conditions and the following disclaimer.
;;;  
;;;   2. Redistributions in binary form must reproduce the above copyright
;;;      notice, this list of conditions and the following disclaimer in the
;;;      documentation and/or other materials provided with the distribution.
;;;  
;;;   3. Neither the name of the authors nor the names of its contributors
;;;      may be used to endorse or promote products derived from this
;;;      software without specific prior written permission.
;;;  
;;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;;;   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;;;   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;;;   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;;;   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
;;;   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
;;;   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
;;;   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
;;;   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
;;;   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;  
;;;  $Id: util.scm,v 1.4 2007-01-21 14:21:54 rui314159 Exp $
;;;

;;; NB: this module is not intended for external use.

(define-module gauche.package.util
  (use gauche.process)
  (use gauche.parameter)
  (use gauche.termios)
  (export run dry-run verbose-run get-password))
(select-module gauche.package.util)

(define dry-run     (make-parameter #f))
(define verbose-run (make-parameter #f))

(define (run cmdline . opts)
  (let-keywords opts ((stdin-string #f))
    (when (or (dry-run) (verbose-run))
      (print cmdline))
    (unless (dry-run)
      (let1 p (run-process "/bin/sh" "-c" cmdline
                           :input (if stdin-string :pipe "/dev/null")
                           :wait #f)
        (when stdin-string
          (let1 pi (process-input p)
            (display stdin-string pi)
            (flush pi)
            (close-output-port pi)))
        (process-wait p)
        (unless (zero? (process-exit-status p))
          (errorf "command execution failed: ~a" cmdline))))))

;; Read password from the terminal without echoing
(define (get-password)
  (let ((prompt "Password: ")
        (iport (open-input-file "/dev/tty"))
        (oport (open-output-file "/dev/tty")))
    (let* ((attr  (sys-tcgetattr iport))
           (lflag (ref attr 'lflag)))
      (dynamic-wind
          (lambda ()
            (set! (ref attr 'lflag)
                  (logand lflag (lognot (logior ECHO ICANON ISIG))))
            (sys-tcsetattr iport TCSANOW attr))
          (lambda ()
            (display prompt oport) (flush oport)
            (read-line iport))
          (lambda ()
            (set! (ref attr 'lflag) lflag)
            (sys-tcsetattr iport TCSANOW attr)
            (close-input-port iport)
            (close-output-port oport)
            )))))

(provide "gauche/package/util")
