xquery version "4.0";

(:~
 : The spec's articles and what can be asked of them (spec/README.md).
 :
 : Every function takes the loaded spec, a map that spec:load() makes:
 :   "articles": the document node of each article file,
 :   "index":    map from a key ("decision:D238", "term:host_shim") to its
 :               article; a term is in it under each of its names.
 : tools/spec/spec runs every query from the repository root.
 :)
module namespace spec = "urn:sxml:spec";

import schema default element namespace "" at "../schema/spec.xsd";

(:~ The repository root: this module is spec/lib/spec.xqm. :)
declare variable $spec:root as xs:string := resolve-uri("../../", static-base-uri());

(:~ The directory of each kind of article, under spec/articles. :)
(:~
 : The canonical form of an article file: indented, except inside the
 : elements whose content is text, where indentation would add text.
 :)
declare variable $spec:serialization as map(*) := {
  "indent": true(),
  "suppress-indentation": ("title", "summary", "name", "definition", "p", "heading", "cell",
                           "strong", "em", "link", "code", "pre", "comment") ! xs:QName(.)
};

declare variable $spec:directories as map(xs:string, xs:string) := {
  "decision": "decisions", "proposal": "proposals", "term": "terms",
  "packet": "packets", "log-entry": "log", "document": "documents"
};

(:~ The URI of PATH, a path relative to the repository root. :)
declare function spec:uri($path as xs:string) as xs:string {
  resolve-uri($path, $spec:root)
};

(:~ The spec as its files are now. :)
declare function spec:load() as map(*) {
  spec:load(collection(spec:uri("spec/articles") || "?recurse=yes"))
};

(:~ The spec made of ARTICLES, document nodes whose elements are articles. :)
declare function spec:load($articles as document-node()*) as map(*) {
  {
    "articles": $articles,
    "index": map:merge((
      for $article in $articles/* return (
        map:entry(spec:key($article), $article),
        for $name in $article/self::term/name return map:entry("term-name:" || $name, $article))
    ), { "duplicates": "use-first" })
  }
};

(:~ The article elements of KIND ("decision", "term", ...), or all. :)
declare function spec:of-kind($spec as map(*), $kind as xs:string?) as element()* {
  $spec?articles/*[empty($kind) or local-name() = $kind]
};

(:~ The kind and identifier of an article, as references write it. :)
declare function spec:key($article as element()) as xs:string {
  local-name($article) || ":" || $article/@id
};

(:~
 : The articles NAME names. "D238", "6.2" and "host_shim" name themselves;
 : "kind:identifier" chooses a kind; a term is also found by any of its
 : names; "document#section" names a section of a document.
 :)
declare function spec:lookup($spec as map(*), $name as xs:string) as element()* {
  let $kind := if (matches($name, "^[a-z-]+:")) then substring-before($name, ":") else ()
  let $rest := if (exists($kind)) then substring-after($name, ":") else $name
  let $identifier := if (contains($rest, "#")) then substring-before($rest, "#") else $rest
  let $section := if (contains($rest, "#")) then substring-after($rest, "#") else ()
  let $found := spec:distinct((
                  if (exists($kind)) then $spec?index($kind || ":" || $identifier)
                  else map:keys($spec:directories) ! $spec?index(. || ":" || $identifier),
                  if (empty($kind) or $kind = "term") then $spec?index("term-name:" || $identifier) else ()))
  return if (exists($section)) then $found//section[@id = $section] else $found
};

(:~ The one article NAME names, or an error that says what it names instead. :)
declare function spec:one($spec as map(*), $name as xs:string) as element() {
  let $found := spec:lookup($spec, $name)
  return
    if (count($found) = 1) then $found
    else if (empty($found)) then error(xs:QName("spec:not-found"), spec:no-such($spec, $name))
    else error(xs:QName("spec:ambiguous"), $name || " names " || string-join($found ! spec:key(.), " and ") || "; give the kind")
};

declare %private function spec:no-such($spec as map(*), $name as xs:string) as xs:string {
  let $identifier := replace($name, "^[a-z-]+:", "")
  let $near := (for $article in spec:of-kind($spec, ())
                where starts-with($article/@id, $identifier) or starts-with($identifier, $article/@id)
                order by string-length($article/@id)
                return spec:key($article))[position() le 5]
  return "nothing is named " || $name || (if (exists($near)) then "; did you mean " || string-join($near, ", ") || "?" else "")
};

(:~ The article a reference names, or nothing when it dangles. :)
declare function spec:target($spec as map(*), $reference as element()) as element()? {
  if ($reference/self::term) then $spec?index("term-name:" || $reference/@name)
  else $spec?index($reference/@kind || ":" || $reference/@to)
};

(:~ Every reference in NODES: refs, terms and includes. :)
declare function spec:references($nodes as node()*) as element()* {
  $nodes/descendant-or-self::*[self::ref or self::term[@name] or self::include]
};

(:~ The key a reference names. :)
declare function spec:reference-key($reference as element()) as xs:string {
  if ($reference/self::term) then "term:" || $reference/@name else $reference/@kind || ":" || $reference/@to
};

(:~ The articles whose text refers to ARTICLE; an include is not a citation. :)
declare function spec:cited-by($spec as map(*), $article as element()) as element()* {
  let $keys := if ($article/self::term) then $article/name ! ("term:" || .) else spec:key($article)
  for $other in $spec?articles/*
  where not($other is $article)
    and (some $reference in spec:references($other)[not(self::include)] satisfies spec:reference-key($reference) = $keys)
  return $other
};

(:~ The documents that include ARTICLE. :)
declare function spec:included-in($spec as map(*), $article as element()) as element()* {
  spec:of-kind($spec, "document")[.//include[@kind = local-name($article) and @to = $article/@id]]
};

(:~ The articles ARTICLE refers to, each once, in the order first cited. :)
declare function spec:cites($spec as map(*), $article as element()) as element()* {
  let $targets := spec:references($article)[not(self::include)] ! spec:target($spec, .)
  return spec:distinct($targets[not(. is $article)])
};

(:~ NODES without repeats, in their order. :)
declare function spec:distinct($nodes as node()*) as node()* {
  fold-left($nodes, (), fn($kept, $node) {
    if (some $seen in $kept satisfies $seen is $node) then $kept else ($kept, $node)
  })
};

(: ---- State ---- :)

(:~ An article's state in a few words: a decision's status, a packet's. :)
declare function spec:status($article as element()) as xs:string {
  typeswitch ($article)
    case element(decision) return
      string-join(($article/@status, $article/@superseded-by ! ("by " || .)), " ")
    case element(proposal) return "proposed"
    case element(packet) return
      let $status := $article/body/*[1]/status
      return if (empty($status)) then "not recorded" else string-join(($status/@value, $status/@date), " ")
    case element(term) return
      if (count($article/definition) > 1) then count($article/definition) || " definitions" else "defined"
    default return ""
};

(:~ The date an article was recorded or reached its status, when it says. :)
declare function spec:date($article as element()) as xs:string? {
  ($article/body/*[1]/(recorded, status)/@date)[1] ! string()
};

(:~ The task boxes in NODES: [checked, open]. :)
declare function spec:boxes($nodes as node()*) as xs:integer+ {
  let $items := $nodes//item[@checked]
  return (count($items[@checked = "true"]), count($items[@checked = "false"]))
};

(: ---- Text ---- :)

(:~ The title of an article or section as one line of plain text. :)
declare function spec:title($article as element()) as xs:string {
  typeswitch ($article)
    case element(term) return string-join($article/name, " / ")
    default return normalize-space(spec:plain($article/title))
};

(:~ One line naming an article: its key, state and title. :)
declare function spec:line($article as element()) as xs:string {
  let $status := spec:status($article)
  return string-join((
    if ($article/self::section) then "section " || $article/@id else spec:key($article),
    if ($status = ("", "defined")) then () else "[" || $status || "]",
    if ($article/self::term) then string-join($article/name[. ne $article/@id] ! ("(" || . || ")"), " ") || spec:summary($article) ! (if ($article/name[. ne $article/@id]) then " " || . else .) else spec:title($article)), " ")
};

(:~ Plain text of inline content: references as their identifiers. :)
declare function spec:plain($nodes as node()*) as xs:string {
  string-join(for $node in $nodes return spec:plain-node($node))
};

declare %private function spec:plain-node($node as node()) as xs:string {
  typeswitch ($node)
    case text() return string($node)
    case element(ref) return string($node/@to)
    case element(term) return if ($node/@name) then string($node/@name) else spec:plain($node/node())
    case element(recorded) return "Recorded " || $node/@date
    case element(status) return string-join((spec:capitalize($node/@value), $node/@date), " ")
    case element() return spec:plain($node/node())
    default return ""
};

declare %private function spec:capitalize($word as xs:string) as xs:string {
  upper-case(substring($word, 1, 1)) || substring($word, 2)
};

(:~
 : The summary of an article: its summary element, or else the start of
 : its text, marked with a leading "~" because nobody wrote it as one.
 :)
declare function spec:summary($article as element()) as xs:string {
  if ($article/summary) then normalize-space(spec:plain($article/summary))
  else
    let $first := ($article/definition, $article/body//p[not(recorded or status)], $article/p, $article//p, $article/body//p)[1]
    let $text := replace(normalize-space(spec:plain($first)), "^Recorded \d{4}-\d{2}-\d{2}[.,;]?\s*", "")
    let $sentence := replace($text, "^(.{40,}?[.;:])\s.*$", "$1")
    return "~ " || (if (string-length($sentence) > 240) then substring($sentence, 1, 237) || "..." else $sentence)
};

(: ---- Markdown ---- :)

(:~ NODE (an article, section or block) as Markdown, with heading LEVEL. :)
declare function spec:markdown($spec as map(*), $node as element(), $level as xs:integer) as xs:string {
  string-join(spec:wrap(spec:blocks($spec, $node, $level, "", true()), 79), "&#10;")
};

(:~ NODE as Markdown, each included article as one line naming it. :)
declare function spec:outline-markdown($spec as map(*), $node as element(), $level as xs:integer) as xs:string {
  string-join(spec:wrap(spec:blocks($spec, $node, $level, "", false()), 79), "&#10;")
};

(:~
 : LINES of Markdown with each paragraph line wrapped at WIDTH columns:
 : continuation lines are indented under the text of a list item or
 : quoted. Code fences, tables, headings and comments are left alone, and
 : no line starts with a word that would begin a block there.
 :)
declare function spec:wrap($lines as xs:string*, $width as xs:integer) as xs:string* {
  let $fences := for $line at $position in $lines where matches($line, "^\s*(> )*```") return $position
  for $line at $position in $lines
  let $in-fence := count($fences[. le $position]) mod 2 = 1 or $position = $fences
  return
    if ($in-fence or string-length($line) le $width or matches($line, "^\s*(> )*(\||#|<!--)")) then $line
    else
      let $prefix := replace($line, "^((\s*(> )*)([-*+] |\d+[.)] )?(\[[ xX]\] )?).*$", "$1")
      let $continuation := replace($prefix, "[-*+\d.)\[\]xX ]", " ") => replace("^(\s*)((> )*)", "$1$2")
      let $continuation := string-join((
        replace($prefix, "^(\s*(> )*).*$", "$1"),
        (1 to string-length($prefix) - string-length(replace($prefix, "^(\s*(> )*).*$", "$1"))) ! " "))
      let $words := tokenize(substring($line, string-length($prefix) + 1), " ")[.]
      return spec:fill($words, $prefix, $continuation, $width)
};

declare %private function spec:fill($words as xs:string*, $prefix as xs:string, $continuation as xs:string,
    $width as xs:integer) as xs:string* {
  let $lines := fold-left($words, [], fn($lines, $word) {
    if (array:size($lines) = 0) then [$prefix || $word]
    else
      let $last := array:foot($lines)
      return
        if (string-length($last) + 1 + string-length($word) le $width
            or matches($word, "^([-*+>|=]+|\d+[.)]|#+|```.*|<!--.*)$")) then
          array:put($lines, array:size($lines), $last || " " || $word)
        else array:append($lines, $continuation || $word)
  })
  return $lines?*
};

declare %private function spec:blocks($spec as map(*), $node as element(), $level as xs:integer,
    $indent as xs:string, $expand as xs:boolean) as xs:string* {
  let $summary := $node/summary ! ("**Summary**: " || spec:inline(node()), "")
  let $superseded := $node/@superseded-by ! ("*Superseded by " || . || ".*", "")
  return typeswitch ($node)
    case element(document) return (
      "# " || spec:inline($node/title/node()), "", $summary,
      for $child in $node/*[not(self::title or self::summary)] return spec:block($spec, $child, 2, $indent, $expand))
    case element(decision) return (
      spec:hashes($level) || " " || $node/@id || ". " || spec:inline($node/title/node()), "", $superseded, $summary,
      for $child in $node/body/* return spec:block($spec, $child, $level + 1, $indent, $expand))
    case element(packet) return (
      spec:hashes($level) || " " || $node/@id || " " || spec:inline($node/title/node()), $summary,
      for $child in $node/body/* return spec:block($spec, $child, $level + 1, $indent, $expand))
    case element(log-entry) return (
      spec:hashes($level) || " " || spec:inline($node/title/node()), "",
      for $child in $node/body/* return spec:block($spec, $child, $level + 1, $indent, $expand))
    case element(term) return
      for $definition in $node/definition
      return "| " || string-join($node/name ! ("`" || . || "`"), " / ") || " | " || spec:inline($definition/node(), true()) || " |"
    case element(proposal) return spec:block($spec, spec:proposal-item($node), $level, $indent, $expand)
    default return spec:block($spec, $node, $level, $indent, $expand)
};

declare %private function spec:proposal-item($proposal as element(proposal)) as element(item) {
  <item>{
    let $first := $proposal/body/*[1]
    return (
      if ($first/self::p) then <p><strong>{$proposal/title/node()}</strong>{$first/node()}</p>
      else (<p><strong>{$proposal/title/node()}</strong></p>, $first),
      $proposal/body/*[position() > 1])
  }</item>
};

declare %private function spec:hashes($level as xs:integer) as xs:string {
  string-join((1 to $level) ! "#")
};

declare %private function spec:block($spec as map(*), $node as element(), $level as xs:integer,
    $indent as xs:string, $expand as xs:boolean) as xs:string* {
  typeswitch ($node)
    case element(section) return (
      let $at := if ($node/@level) then xs:integer($node/@level) else $level
      return (
        $indent || spec:hashes($at) || " " || spec:inline($node/title/node()), "",
        for $child in $node/*[not(self::title)] return spec:block($spec, $child, $at + 1, $indent, $expand)))
    case element(include) return
      let $target := spec:target($spec, $node)
      return
        if (empty($target)) then $indent || "<!-- missing " || spec:reference-key($node) || " -->"
        else if (not($expand)) then ($indent || "→ " || spec:line($target), "")[not($target/self::term and $node/@definition)]
        else if ($target/self::term) then
          let $number := xs:integer(($node/@definition, 1)[1])
          let $term := $target transform with { delete node definition[position() != $number] }
          return spec:blocks($spec, $term, $level, $indent, $expand)
        else spec:blocks($spec, $target, $level, $indent, $expand)
    case element(p) return ($indent || spec:inline($node/node()), "")
    case element(heading) return ($indent || spec:hashes(xs:integer($node/@level)) || " " || spec:inline($node/node()), "")
    case element(rule) return ($indent || "---", "")
    case element(comment) return ($indent || "<!-- " || $node || " -->", "")
    case element(pre) return (
      $indent || "```" || $node/@language,
      tokenize(string($node), "&#10;") ! ($indent || .),
      $indent || "```", "")
    case element(quote) return (
      for $line in $node/* ! spec:block($spec, ., $level, "", $expand) return $indent || "> " || $line, "")
    case element(table) return (
      for $row at $position in $node/*
      return (
        if ($row/self::include) then spec:block($spec, $row, $level, $indent, $expand)
        else $indent || "| " || string-join($row/cell ! spec:inline(node(), true()), " | ") || " |",
        if ($position = 1 and $row/@header) then
          $indent || "| " || string-join(
            for $cell at $column in $row/cell
            return switch (tokenize(string($node/@align))[$column])
              case "left" return ":---" case "right" return "---:" case "center" return ":---:" default return "---",
            " | ") || " |"
        else ()),
      "")
    case element(list) return (
      for $child at $position in $node/*
      let $item := if ($child/self::include and $expand) then spec:proposal-item(spec:target($spec, $child)) else $child
      let $marker := if ($node/@ordered = "true") then (xs:integer(($node/@start, 1)[1]) + $position - 1) || ". " else "- "
      let $box := if ($item/@checked = "true") then "[x] " else if ($item/@checked = "false") then "[ ] " else ""
      let $lines := if ($item/self::include) then substring-after(spec:block($spec, $item, $level, "", false())[1], "→ ")
                    else for $block in $item/* return spec:block($spec, $block, $level, "", $expand)
      let $lines := $lines[not(position() = last() and . = "")]
      let $pad := string-join((1 to string-length($marker)) ! " ")
      return (
        $indent || $marker || $box || $lines[1],
        for $line in $lines[position() > 1] return if ($line = "") then "" else $indent || $pad || $line),
      "")
    case element(item) return spec:block($spec, <list>{$node}</list>, $level, $indent, $expand)
    default return $indent || spec:inline($node/node())
};

(:~ Inline content as Markdown; IN-TABLE escapes the cell separator. :)
declare function spec:inline($nodes as node()*) as xs:string {
  spec:inline($nodes, false())
};

declare function spec:inline($nodes as node()*, $in-table as xs:boolean) as xs:string {
  string-join(
    for $node in $nodes return
      typeswitch ($node)
        case text() return
          let $text := replace(string($node), "([\\`*])", "\\$1")
          return if ($in-table) then replace($text, "\|", "\\|") else $text
        case element(code) return spec:code(string($node), $in-table)
        case element(term) return spec:code(string($node/@name), $in-table)
        case element(ref) return string($node/@to)
        case element(strong) return "**" || spec:inline($node/node(), $in-table) || "**"
        case element(em) return "*" || spec:inline($node/node(), $in-table) || "*"
        case element(link) return "[" || spec:inline($node/node(), $in-table) || "](" || $node/@href || ")"
        case element(recorded) return "Recorded " || $node/@date
        case element(status) return "**" || spec:capitalize($node/@value) || "**" || $node/@date ! (" " || .)
        default return spec:inline($node/node(), $in-table))
};

declare %private function spec:code($text as xs:string, $in-table as xs:boolean) as xs:string {
  let $text := if ($in-table) then replace($text, "\|", "\\|") else $text
  let $longest := max((0, analyze-string($text, "`+")/fn:match ! string-length()))
  let $fence := string-join((0 to $longest) ! "`")
  let $pad := if (contains($text, "`")) then " " else ""
  return $fence || $pad || $text || $pad || $fence
};

(: ---- Prose: what a write is given ---- :)

(:~
 : Blocks from TEXT, the Markdown a write is given: paragraphs, "- " lists
 : and ``` fences, separated by blank lines; inline as spec:prose-inline.
 :)
declare function spec:prose($spec as map(*), $text as xs:string) as element()* {
  for $block in tokenize(replace($text, "\r", ""), "\n[ \t]*\n")[normalize-space()]
  let $lines := tokenize($block, "\n")
  return
    if (starts-with($lines[1], "```")) then
      <pre>{ substring-after($lines[1], "```")[.] ! attribute language { . } }{
        string-join(subsequence($lines, 2, count($lines) - 1 - (if (starts-with($lines[last()], "```")) then 1 else 0)), "&#10;")
      }</pre>
    else if (matches($lines[1], "^- ")) then
      <list>{
        for tumbling window $item in $lines start $first when starts-with($first, "- ")
        return <item><p>{ spec:prose-inline($spec, normalize-space(substring(string-join($item, " "), 3))) }</p></item>
      }</list>
    else <p>{ spec:prose-inline($spec, normalize-space(string-join($lines, " "))) }</p>
};

(:~
 : Inline content from TEXT: `code` (a term when it is one), **strong**,
 : *em*, [text](link), [[kind:identifier]] for any reference, and decision
 : numbers (D238), which are always references.
 :)
declare function spec:prose-inline($spec as map(*), $text as xs:string) as node()* {
  for $part in analyze-string($text, "\[\[[a-z-]+:[^\]]+\]\]|``[^`]+``|`[^`]+`|\*\*[^*]+\*\*|\*[^*\s][^*]*\*|\[[^\]]+\]\([^)]+\)")/*
  let $value := string($part)
  return
    if ($part/self::fn:non-match) then spec:prose-references($value)
    else if (starts-with($value, "[[")) then
      let $key := substring($value, 3, string-length($value) - 4)
      return <ref kind="{ substring-before($key, ':') }" to="{ substring-after($key, ':') }"/>
    else if (starts-with($value, "`")) then
      let $code := replace($value, "^`+ ?| ?`+$", "")
      return if (exists($spec?index("term-name:" || $code))) then <term name="{ $code }"/> else <code>{ $code }</code>
    else if (starts-with($value, "**")) then <strong>{ spec:prose-inline($spec, substring($value, 3, string-length($value) - 4)) }</strong>
    else if (starts-with($value, "*")) then <em>{ spec:prose-inline($spec, substring($value, 2, string-length($value) - 2)) }</em>
    else
      let $label := substring-before(substring($value, 2), "](")
      return <link href="{ substring-before(substring-after($value, "]("), ")") }">{ spec:prose-inline($spec, $label) }</link>
};

declare %private function spec:prose-references($text as xs:string) as node()* {
  for $part in analyze-string($text, "[A-Za-z0-9_]+")/*
  return
    if ($part/self::fn:match and matches($part, "^D[1-9][0-9]*$")) then <ref kind="decision" to="{ $part }"/>
    else text { $part }
};

(: ---- Checks ---- :)

(:~
 : What is wrong with the spec, one line each. "error: ..." breaks it: a
 : reference to nothing, two articles with one key, an article no
 : document includes. "warning: ..." asks for work: a term defined twice.
 :)
declare function spec:check($spec as map(*)) as xs:string* {
  let $all := $spec?articles/*
  let $included := map:merge(
    for $include in $all/self::document//include return map:entry(spec:reference-key($include), true()),
    { "duplicates": "use-first" })
  return (
    for $article in $all
    group by $key := spec:key($article)
    where count($article) > 1
    return "error: " || $key || " is defined " || count($article) || " times",
    for $name in $all/self::term/name
    group by $value := string($name)
    where count($name) > 1
    return "error: the term name " || $value || " belongs to " || count($name) || " terms",
    for $article in $all
    for $reference in spec:references($article)
    let $key := spec:reference-key($reference)
    where empty(spec:target($spec, $reference))
    group by $from := spec:key($article), $key
    return "error: " || $from || " refers to " || $key || ", which does not exist",
    for $decision in $all/self::decision[@superseded-by]
    where empty($spec?index("decision:" || $decision/@superseded-by))
    return "error: " || spec:key($decision) || " is superseded by " || $decision/@superseded-by || ", which does not exist",
    for $article in $all[not(self::document)]
    where not($included(spec:key($article)))
    return "error: " || spec:key($article) || " is in no document",
    for $term in $all/self::term[count(definition) > 1]
    return "warning: term:" || $term/@id || " has " || count($term/definition) || " definitions"
  )
};

(:~
 : The article files not in canonical form ($spec:serialization, with a
 : final newline), the form spec:put writes. One that is not was edited
 : as text.
 :)
declare function spec:not-canonical($spec as map(*)) as xs:string* {
  for $article in $spec?articles
  let $uri := document-uri($article)
  where serialize($article, $spec:serialization) || "&#10;" ne unparsed-text($uri)
  return "error: " || substring-after($uri, $spec:root) || " is not in canonical form; it was edited as text"
};

(: ---- Writes ---- :)

(:~ The file of ARTICLE: where it is, or where a new one goes. :)
declare function spec:file($spec as map(*), $article as element()) as xs:string {
  let $old := $spec?index(spec:key($article))
  return
    if (exists($old)) then document-uri(root($old))
    else spec:uri("spec/articles/" || $spec:directories(local-name($article)) || "/" || $article/@id || ".xml")
};

(:~
 : What storing CHANGED would break: each article must validate against
 : spec/schema/spec.xsd, and the check must find no error it did not find
 : before. The errors are the lines of spec:check that are new.
 :)
declare function spec:refusals($spec as map(*), $changed as element()*) as xs:string* {
  let $keys := $changed ! spec:key(.)
  let $after := spec:load((
    $spec?articles[not(spec:key(*) = $keys)],
    $changed ! document { . }))
  let $before := spec:check($spec)
  return (
    for $article in $changed
    return try {
      let $valid := validate strict { document { $article } }
      return if ($valid instance of document-node()) then () else "error: " || spec:key($article) || " is not valid"
    } catch * { "error: " || spec:key($article) || " is not valid: " || $err:description },
    spec:check($after)[starts-with(., "error:")][not(. = $before)])
};

(:~
 : Store CHANGED, new or replacing articles with the same keys, each in
 : canonical form (spec:put), or
 : store nothing and raise spec:refused with what the change would break.
 :)
declare updating function spec:store($spec as map(*), $changed as element()*) {
  let $refusals := spec:refusals($spec, $changed)
  return
    if (exists($refusals)) then error(xs:QName("spec:refused"), "refused: " || string-join($refusals, "&#10;"))
    else for $article in $changed return spec:put($article, spec:file($spec, $article))
};

(:~ Write ARTICLE to the file URI in canonical form. :)
declare updating function spec:put($article as element(), $uri as xs:string) {
  put(document { $article, text { "&#10;" } }, $uri, $spec:serialization)
};

(:~ DOCUMENT with an include of ARTICLE added at the end of SECTION (or the document). :)
declare function spec:with-include($document as element(document), $section as xs:string?, $article as element()) as element(document) {
  let $include := <include kind="{ local-name($article) }" to="{ $article/@id }"/>
  return $document transform with {
    let $place := if (exists($section)) then .//section[@id = $section] else .
    return
      if (empty($place)) then error(xs:QName("spec:not-found"), $document/@id || " has no section " || $section)
      else if ($article/self::term) then insert node $include as last into ($place//table)[last()]
      else insert node $include as last into $place
  }
};
