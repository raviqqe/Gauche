;;;
;;; library utilities - to be autoloaded.
;;;
;;;   Copyright (c) 2003-2021  Shiro Kawai  <shiro@acm.org>
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

(define-module gauche.libutil
  (use scheme.list)
  (use srfi-13)
  (export library-exists? library-fold library-map library-for-each
          library-has-module? library-name->module-name))
(select-module gauche.libutil)

;;
(cond-expand
 [gauche.os.windows
  (define (N path)
    (call-with-string-io path
      (^[i o]
        (let loop ((ch (read-char i)))
          (unless (eof-object? ch)
            (if (eqv? ch #\\) (write-char #\/ o) (write-char ch o))
            (loop (read-char i)))))))]
 [else
  (define (N path) path)])

;; library-fold - iterate over the modules or libraries whose name matches
;;  the given pattern.
;;  proc takes the matched module/library name, full pathname, and seed value.
;;  This can be more involved once customized module mapping system
;;  is introduced; for now, we simply apply the default mapping rule.
(define (library-fold pattern proc seed
                      :key (paths *load-path*) (allow-duplicates? #f)
                           (strict? #t))

  (define search-module?
    (cond [(string? pattern) #f]
          [(symbol? pattern) #t]
          [else (error "string or symbol required, but got" pattern)]))

  (define seen '())

  (define (ensure path partial)
    (if search-module?
      (let1 modname (path->module-name (N (string-drop-right partial 4)))
        (and (or (not strict?)
                 (library-has-module? path modname))
             modname))
      (string-drop-right partial 4)))

  (define (get-relative prefix path)
    (string-trim (string-drop path (string-length prefix)) #[/\\]))

  (define (fold-dirs proc seed dirname regexp)
    (fold (^[elt seed]
            (or (and-let* ([ (regexp elt) ]
                           [path (topath dirname elt)]
                           [ (file-is-directory? path) ])
                  (proc path seed))
                seed))
          seed (readdir dirname)))

  (define (fold-leaf proc seed prefix dirname regexp)
    (fold (^[elt seed]
            (or (and-let* ([m (#/(.*)\.sc[im]$/ elt) ]
                           [base (m 1)]
                           [ (regexp base) ]
                           [path (topath dirname elt)]
                           [file (get-relative prefix path)]
                           [key (ensure (N path) (N file))])
                  (if (and (not allow-duplicates?) (member key seen))
                    seed
                    (begin (push! seen key) (proc key path seed))))
                seed))
          seed (readdir dirname)))

  (define (make-folder prefix)
    (^[proc seed parent regexp non-leaf?]
      ;; We always search to the leaf nodes, so REGEXP never be 'dir.
      (let* ([dir (if (string? parent) parent prefix)]
             [elts (filter regexp (readdir dir))])
        (if non-leaf?
          (fold-dirs proc seed dir regexp)
          (fold-leaf proc seed prefix dir regexp)))))

  (let ([pattern (if search-module?
                   (module-name->path pattern)
                   pattern)])
    (fold (^[prefix seed]
            (let1 prefix (sys-normalize-pathname prefix :canonicalize #t)
              (glob-fold pattern proc seed
                         :folder (make-folder prefix)
                         :prefix (if (equal? prefix "") "." prefix)
                         :sorter #f)))
          seed paths)))

;; Just check existence of library.
(define (library-exists? mod/path :key (force-search? #f)
                         (strict? #t) (paths *load-path*))

  (or (and (not force-search?)
           ;; see if specified mod/path is already loaded
           (or (and (string? mod/path) (provided? mod/path))
               (and (symbol? mod/path) (find-module mod/path))))
      ;; scan the filesystem
      (let/cc found
        (library-fold mod/path
                      (^[mod path seed] (found #t))
                      #f
                      :strict? strict? :paths paths))))

;; Convenience wrappers
(define (library-map mod/path proc . opts)
  (reverse! (apply library-fold mod/path
                   (^[mod path seed] (cons (proc mod path) seed))
                   '() opts)))

(define (library-for-each mod/path proc . opts)
  (apply library-fold mod/path (^[mod path seed] (proc mod path)) '() opts))

;; Try to determine the file is a module source file
;;  NB: this will be more involved when we allow more than one modules
;;  in a file.  Or should we?
(define (library-has-module? file name)
  (let1 exp (guard (e [else #f])
              (with-input-from-file file read :if-does-not-exist #f))
    (and (pair? exp)
         (or (and (eq? (car exp) 'define-module)
                  (pair? (cdr exp))
                  (eq? (cadr exp) name))
             (and (eq? (car exp) 'define-library)
                  (pair? (cdr exp))
                  ;; This should be optimized---it's waste of time running
                  ;; library-name->module-name for every library on the way.
                  (eq? (library-name->module-name (cadr exp)) name)))
         file)))

;; Auxiliary procedures
;;  we don't want to depend file.util here, so some simple utils.

;; readdir with removing "." and "..".
(define (readdir dir)
  (if (file-is-directory? dir)
    (let loop ([p (sys-readdir dir)] (r '()))
      (cond [(null? p) (reverse! r)]
            [(member (car p) '("." "..")) (loop (cdr p) r)]
            [else (loop (cdr p) (cons (car p) r))]))
    '()))

(define (topath prefix file)
  (sys-normalize-pathname (string-append prefix "/" file) :canonicalize #t))

;; NB: this is also used by gauche.reload
(define (module-glob-pattern->regexp pat)
  (guard (e [else (error "bad glob pattern" pat)])
    (string->regexp
     (with-string-io pat
       (^[]
         (display "^")
         (let loop ([c (read-char)])
           (unless (eof-object? c)
             (case c
               [(#\?) (display "[^.]") (loop (read-char))]
               [(#\*) (display "[^.]*") (loop (read-char))]
               [(#\\) (let1 c2 (read-char)
                        (unless (eof-object? c2)
                          (write-char c2) (loop (read-char))))]
               [(#\.) (display "\\.") (loop (read-char))]
               [else (write-char c) (loop (read-char))])))
         (display "$"))))))

;; mapping R7RS library name to Gauche module name
;;
;; We simply maps R7RS (foo bar baz) to Gauche's foo.bar.baz . The caveat
;; is that R7RS library name component may include dots.  We map them in R7RS
;; library names into consecutive dots in Gauche module name, which will be
;; mapped to a single dot again in pathnames.
;; (The latter translation is done by module-name->path and path->module-name
;; in src/libmod.scm)
;;
;;  R7RS library name   Gauche module name      File pathname
;;
;;  (foo bar baz)       foo.bar.baz             foo/bar/baz
;;  (foo b.r baz)       foo.b..r.baz            foo/b.r/baz
;;
;; TODO: R7RS library name can contain weird characters, and we need a rule
;; to map them.
(define (library-name->module-name libname)
  (define (stringify x)
    (cond [(keyword? x) (write-to-string x)]
          [(symbol? x) (symbol->string x)]
          [(and (integer? x) (exact? x) (>= x 0)) (number->string x)]
          [else (error "Bad name component in library name:" x)]))
  ($ string->symbol $ (cut string-join <> ".")
     $ map ($ (cut regexp-replace-all #/\./ <> "..") $ stringify $) libname))
