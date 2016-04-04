## REPL

<img width="130" align="right" style="margin: 0ex 1em" src="img/repl.jpg">
If you don't provide any `-i` or `-e` options or args to `planck` when launching it (or if you explicitly specify `-r` or `-​-​repl` as the _main-opt_), Planck will enter an interactive Read-Eval-Print Loop, or _REPL_.

```
$ planck
Planck 1.10
ClojureScript 1.8.28
    Docs: (doc function-name-here)
          (find-doc "part-of-name-here")
  Source: (source function-name-here)
    Exit: Control+D or :cljs/quit or exit or quit
 Results: Stored in vars *1, *2, *3, an exception in *e

cljs.user=> ▊
```

To the left of `=>` is the _current namespace_. 

> In ClojureScript, `def` and derived forms create vars in the current namespace. In addition, unqualified non-local symbols are resolved to vars in the current namespace.

You can enter forms to be evaluated in the REPL, and any printed output will be displayed in the REPL, followed by the value of the evaluated form: Try evaluating `(+ 1 2)` and `3` will be printed. Or, try `(println "Hi")` and `Hi` will be printed followed by `nil` (the value of the `println` call).

You can hit return prior to typing a complete form and input will continue on the next line. A `#_=>` prompt will be used (padded to line up with the initial `=>`) for secondary input lines. Multi-line input can continue this way until a full form is entered:

```clojure-repl
cljs.user=> (defn square
       #_=>  [x]
       #_=>  (* x x))
#'cljs.user/square
```

You can enter multiple forms on a line and they will be evaluated serially:

```clojure-repl
cljs.user=> (def x 1) (def y 2) (+ x y)
#'cljs.user/x
#'cljs.user/y
3
```

At any point in entering a form, Ctrl-C can be hit to discard form entry and start with a fresh prompt.

As you type closing delimiters (`)`, `]`, and `}`), the cursor will temporarily hop to the matching opening delimiter.

###  Line Editing

#### Arrow Keys

You can use the up and down arrow keys to navigate through previously-entered lines. The line history includes lines entered in previous Planck sessions, with the last 100 lines saved in the `.planck_history` file in your home directory.

#### Tab Completion

You can use the tab key to auto-complete. Try typing `(map` and then hitting the tab key. You will be presented choices like `map-indexed`, `map?`, `mapcat`, _etc._ Hitting shift-tab returns to the originally entered text. Tab completion works aginst core names and also against names you introduce. If you do

```clj
(def supercalifragilisticexpialidocious "something quite atrocious")
```

then `su` followed by tab will yield `subs`, and other choices, including the gem above.

#### Control Keys

Planck employs the line editing library [Linenoise](https://github.com/antirez/linenoise), which provides control characters that you may expect:

* Ctrl-A: Go to beginning of line
* Ctrl-E: Go to end of line
* Ctrl-B: Go back one space
* Ctrl-F: Go forward one space
* Ctrl-K: Kill to the end of the line
* Ctrl-W: Kill to beginning of the line
* Ctrl-H: Delete backwards one character
* Ctrl-L: Clear the screen
* Ctrl-P: Previous line
* Ctrl-N: Next line
* Ctrl-T: Transpose current and previous character
* Ctrl-U: Undo all typing on current line

###  Color Themes

Planck employs various colors for the REPL prompt, results, errors, _etc._ If you'd prefer to work in a monochrome REPL, pass `-t plain` or `-​-theme plain` when starting Planck.

Planck attempts to automatically detect if you are running in a light or dark terminal (first checking and honoring the `COLORFGBG` environment variable, if set) and picks the light or dark theme, which adjusts the colors accordingly. If this detection fails, you can always override it via `-t light` or `-t dark`.

###  Dumb Terminal
Normally, Planck employs the use of VT100 and ANSI codes to perform brace matching, line editing features, and to add color. If you are using Planck in an environment where these codes are not supported, or you would prefer to disable them, you can pass `-d` or `-​-dumb-terminal` when starting Planck.

> If you'd prefer to use Planck with the line-editing capabilities offered by [GNU Readline](http://cnswww.cns.cwru.edu/php/chet/readline/rltop.html), you can use [`rlwrap`](https://github.com/hanslub42/rlwrap), (which is also installable via `brew`). When using `rlwrap`, it is necessary to pass `-d` to `planck` so that `rlwrap`'s terminal controls become active: `rlwrap planck -d`.

###  Exit
You can exit the REPL by typeing Ctrl-D, `exit`, `quit`, or `:cljs/quit`.

### Verbose Mode

If you started Planck in verbose mode (by passing `-v` or `--verbose`) then you will see the JavaScript that is executed for forms that you enter in the REPL, along with other useful diagnostic information.

### Quiet Mode

If you started Planck in quiet mode (by passing `-q` or `--quiet`) then you will not see any banners from REPL, just your script output.

### REPL Specials

REPL specials are, in essence, special forms that exist only in the REPL. (They can't be used in regular ClojureScript code and must be entered as top-level forms.)

#### `in-ns` 

Planck supports `in-ns`, which will switch you to a new namespace, creating it if it doesn't already exist.

```clojure-repl
cljs.user=> (in-ns 'bar.core)
nil
bar.core=> ▊
```

As in Clojure, Planck's `in-ns` REPL special accepts any expression, so long as it evaluates to a symbol, so you can do someting like this

```clojure-repl
cljs.user=> (def my-ns 'foo.core)
#'cljs.user/my-ns
cljs.user=> (in-ns my-ns)
nil
foo.core=> ▊
```

#### `require`, `require-macros`, `import`

The `require`, `require-macros`, and  `import` REPL specials make it possible to load namespaces and Google Closure code into Planck. These work on the namespaces and Google Closure code that ships with Planck, like `planck.core`, and also on namespaces defined in source directories or JARs specified by the `-c` or `-​-​classpath` comand-line option.


```clojure-repl
cljs.user=> (require '[planck.core :as planck])
nil
cljs.user=> planck/*planck-version*
"1.10"
cljs.user=> (import '[goog.events EventType])
nil
cljs.user=> EventType.CLICK
"click"
```

> These REPL specials are implemented in terms of the `ns` special form, just as is done in regular ClojureScript REPLs.

#### `load-file`
The `load-file` REPL special can be used to load ClojureScript source from any file on the filesystem. It causes the REPL to sequentially read and evaluate the set of forms contained in the file.

### Auto-Referred Symbols

When you launch Planck into REPL mode, a few macros from the `planck.repl` namespace are automatically referred into the `cljs.user` namespace. These comprise `doc`, `source`, `pst`, `apropos`, `find-doc`, and `dir`. 

If you switch to another namespace and find that `doc` no longer works, this is because `doc` is a macro in the `planck.repl` namespace. You can refer it into your current namespace by doing the following: 

```clj
(require '[planck.repl :refer-macros [doc]])
```

The same works for `source`, `pst`, `apropos`, `find-doc`, and `dir`.
