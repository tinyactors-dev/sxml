xquery version "4.0";
(:
 : The driver of the tree-sitter grammars' own tests
 : (third_party/tree-sitter-grammars/LANGUAGE/test, D13, D285): checks the
 : documents of sxml:parse-code against one test file of one grammar.
 : tests/tree-sitter/run-tree-sitter-tests.sh runs it once per file,
 : through `sxml xquery`, so it uses only what any host of sxml's public
 : API has.
 :
 : $kind "corpus": a file of test/corpus, in tree-sitter's corpus format
 : (tree-sitter's documentation, "Writing Tests"): each test is a header
 : (a line of three or more "=", the test's name, attribute lines such as
 : ":skip" or ":error", and a closing line of "="), the input, a divider
 : (a line of three or more "-"; a suffix after the "=" and "-" lines
 : makes custom delimiters, which must agree), and the expected tree as an
 : S-expression of the named nodes, with field names ("name: (identifier)").
 : The document's S-expression is written from its elements (the named
 : nodes): sxml:token elements are tokens, an element with missing="true"
 : is (MISSING kind), an sxml:node element is named by its kind attribute.
 : The document is parsed without injections (D288), as tree-sitter's
 : test command parses one grammar. Both sides are normalized as tree-sitter does: comment lines (";")
 : dropped, runs of whitespace made one space, no space before ")", and
 : field names dropped from the document's when the expected tree has none.
 : A test with ":error" passes when the document has an ERROR element or
 : a missing element. Tests marked ":skip", ":cst" (it compares anonymous
 : nodes too) or ":language(...)" naming a language sxml does not have are
 : not run.
 :
 : $kind "highlight": a file of test/highlight, whose comments assert the
 : highlights (tree-sitter's documentation, "Syntax Highlighting",
 : "Unit Testing"): in a comment, "<-" names the comment's own column and
 : a "^" (the first of a run) one column; the capture name after it (with
 : "!" for "not") applies to that column on the nearest line above that
 : holds no assertion. An assertion passes when the capture name is the highlight
 : attribute of an element that covers that line and column (an element
 : and its ancestors may all have one: the stack of nested highlights),
 : and for "!" when none has it. The file is parsed with the option
 : positions; columns count characters. Each assertion is one test.
 :
 : Writes one line per test that did not pass ("fail", "error" or
 : "not-run", the language, the file, the test's name, and why), then
 :   counts: LANGUAGE/FILE tests=N passed=P failed=F errors=E not-run=R
 :)
declare namespace sxml = "urn:sxml";

declare option output:method "text";

declare variable $language external;
declare variable $file external;
declare variable $kind external;

declare variable $file-name := tokenize($file, "/")[last()];

(: ------------------------------------------------------ S-expressions :)

declare function local:kind($element as element()) as xs:string {
  if ($element/self::sxml:node) then string($element/@kind) else local-name($element)
};

(: The S-expression of a named node and its named descendants. :)
declare function local:sexp($element as element()) as xs:string {
  if ($element/@missing = "true") then
    if ($element/self::sxml:token)
    then '(MISSING "' || $element/@kind || '")'
    else "(MISSING " || local:kind($element) || ")"
  else
    "(" || local:kind($element)
    || string-join(
         for $child in $element/*[not(self::sxml:token) or @missing = "true"]
         return " " || (if ($child/@field) then $child/@field || ": " else "")
                || local:sexp($child))
    || ")"
};

declare function local:normalize($sexp as xs:string) as xs:string {
  let $lines := tokenize($sexp, "\r?\n")[not(matches(., "^\s*;"))]
  return normalize-space(string-join($lines, " "))
         => replace("\s+\)", ")") => replace("\(\s+", "(")
};

(: --------------------------------------------------------------- Corpus :)

declare function local:header-suffix($line as xs:string) as xs:string? {
  if (matches($line, "^={3,}([^=\r\n].*)?$"))
  then replace($line, "^=+", "")
  else ()
};

(: The tests of a corpus file: one map per test with its name,
   attributes, input and expected S-expression. :)
declare function local:corpus-tests($lines as xs:string*) as map(*)* {
  let $count := count($lines)
  (: A header opens at a "=" line followed by name lines and a closing
     "=" line with the same suffix. :)
  let $headers :=
    for $index in 1 to $count
    let $suffix := local:header-suffix($lines[$index])
    where exists($suffix) and $index + 2 le $count
          and empty(local:header-suffix($lines[$index + 1]))
    let $close := (
      for $next in $index + 2 to min(($count, $index + 20))
      where local:header-suffix($lines[$next]) = $suffix
      return $next)[1]
    where exists($close)
          and (every $between in $index + 2 to $close - 1
               satisfies starts-with(normalize-space($lines[$between]), ":"))
    return map { "open": $index, "close": $close, "suffix": $suffix }
  for $header at $position in $headers
  let $end := if ($position lt count($headers))
              then $headers[$position + 1]?open - 1 else $count
  let $divider-pattern :=
    "^-{3,}" || (if ($header?suffix = "") then "$"
                 else replace($header?suffix, "([.\\\[\](){}*+?^$|-])", "\\$1") || "$")
  let $divider := (
    for $index in $header?close + 1 to $end
    where matches($lines[$index], $divider-pattern)
    return $index)[last()]
  let $attributes :=
    for $index in $header?open + 2 to $header?close - 1
    return normalize-space($lines[$index])
  return map {
    "name": normalize-space($lines[$header?open + 1]),
    "attributes": $attributes,
    "input": if (exists($divider))
             then string-join($lines[position() = $header?close + 1 to $divider - 1], "&#10;")
             else (),
    "expected": if (exists($divider))
                then string-join($lines[position() = $divider + 1 to $end], "&#10;")
                else ()
  }
};

declare function local:run-corpus-test($test as map(*)) as map(*) {
  let $attributes := $test?attributes
  let $test-language :=
    ($attributes[starts-with(., ":language(")]
       ! replace(., "^:language\((.*)\)$", "$1"), $language)[1]
  return
    if (empty($test?input)) then
      map { "outcome": "error", "why": "no divider between input and expected tree" }
    else if ($attributes = (":skip", ":cst")) then
      map { "outcome": "not-run", "why": "marked " || string-join($attributes, " ") }
    else if (not($test-language = sxml:code-languages())) then
      map { "outcome": "not-run", "why": "language " || $test-language }
    else
      try {
        let $document := sxml:parse-code($test?input, $test-language,
                                        { "highlights": false(), "injections": false() })
        let $root := $document/*
        return
          if (string($document) ne $test?input) then
            map { "outcome": "fail", "why": "the string value is not the input" }
          else if ($attributes = ":error") then
            if (exists($root/descendant-or-self::*[self::ERROR or @missing = "true"]))
            then map { "outcome": "pass" }
            else map { "outcome": "fail", "why": "no ERROR or MISSING node: " || local:sexp($root) }
          else
            let $expected := local:normalize($test?expected)
            (: Field names are compared only when the expected tree has
               some (tree-sitter's test command does the same). :)
            let $actual := local:normalize(local:sexp($root))
              => (if (matches($expected, "[A-Za-z0-9_\-]+:\s*\(")) then identity#1
                  else replace(?, "[A-Za-z0-9_\-]+: ", ""))()
            return
              if ($expected eq $actual) then map { "outcome": "pass" }
              else map { "outcome": "fail",
                         "why": "expected " || $expected || " got " || $actual }
      } catch * {
        map { "outcome": "error", "why": $err:code || ": " || $err:description }
      }
};

(: ------------------------------------------------------------ Highlights :)

(: The assertions of one comment: maps of line, columns, name and
   whether it is negated; empty for a comment that asserts nothing. :)
declare function local:assertions($comment as element()) as map(*)* {
  let $text := string($comment)
  let $characters := characters($text)
  let $arrow := (
    for $character at $index in $characters
    where $character = "^"
          or ($character = "-" and $index gt 1 and $characters[$index - 1] = "<")
    return $index)[1]
  where exists($arrow)
  let $is-caret := $characters[$arrow] = "^"
  let $caret-count :=
    if ($is-caret)
    then string-length(replace(substring($text, $arrow), "^(\^+).*$", "$1", "s"))
    else 1
  let $rest := substring($text, $arrow + $caret-count)
  (: A run of carets marks a span, of which the first column is checked:
     the grammars' tests run a caret over "onclick=" for an attribute. :)
  let $match := analyze-string($rest, "^\s*(!?)\s*([\w\-.]+)")//fn:match
  where exists($match)
  return map {
    "line": xs:integer($comment/@line),
    "columns": if ($is-caret)
               then xs:integer($comment/@column) + $arrow - 1
               else xs:integer($comment/@column),
    "negated": string($match/fn:group[@nr = 1]) = "!",
    "name": string($match/fn:group[@nr = 2])
  }
};

declare function local:covers($element as element(), $line as xs:integer,
                              $column as xs:integer) as xs:boolean {
  let $start-line := xs:integer($element/@line)
  let $end-line := xs:integer($element/@end-line)
  return
    ($start-line lt $line
       or ($start-line eq $line and xs:integer($element/@column) le $column))
    and ($line lt $end-line
       or ($line eq $end-line and $column lt xs:integer($element/@end-column)))
};

declare function local:run-highlight-tests($text as xs:string) as map(*)* {
  let $document := sxml:parse-code($text, $language, { "positions": true() })
  let $comments := $document//*[contains(local-name(), "comment")]
  let $assertions := $comments ! local:assertions(.)
  let $assertion-lines := distinct-values($assertions ! ?line)
  let $highlighted := $document//*[@highlight]
  for $assertion in $assertions
  let $line := (
    for $candidate in reverse(1 to $assertion?line - 1)
    where not($candidate = $assertion-lines)
    return $candidate)[1]
  let $name := "line " || $assertion?line || " column " || $assertion?columns[1] || " "
               || (if ($assertion?negated) then "!" else "") || $assertion?name
  let $failures :=
    for $column in $assertion?columns
    let $stack := $highlighted[local:covers(., $line, $column)] ! string(@highlight)
    where ($assertion?name = $stack) eq $assertion?negated
    return "at " || $line || ":" || $column || " found ("
           || string-join($stack, " ") || ")"
  return
    if (empty($line)) then map { "name": $name, "outcome": "error", "why": "no line above" }
    else if (empty($failures)) then map { "name": $name, "outcome": "pass" }
    else map { "name": $name, "outcome": "fail", "why": string-join($failures, "; ") }
};

(: ----------------------------------------------------------------- Main :)

let $text := unparsed-text($file)
let $results :=
  if ($kind = "corpus") then
    for $test in local:corpus-tests(tokenize($text, "\r?\n"))
    return map:put(local:run-corpus-test($test), "name", $test?name)
  else
    try {
      local:run-highlight-tests($text)
    } catch * {
      map { "name": "(the file)", "outcome": "error",
            "why": $err:code || ": " || $err:description }
    }
let $count := function($outcome) { count($results[?outcome = $outcome]) }
return string-join((
  for $result in $results[?outcome ne "pass"]
  return string-join(($result?outcome, $language, $file-name, $result?name, $result?why), " | "),
  "counts: " || $language || "/" || $file-name || " tests=" || count($results)
    || " passed=" || $count("pass") || " failed=" || $count("fail")
    || " errors=" || $count("error") || " not-run=" || $count("not-run")
), "&#10;")
