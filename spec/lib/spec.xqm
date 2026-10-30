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

declare namespace file = "http://expath.org/ns/file";

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
  "packet": "packets", "log-entry": "log", "document": "documents", "fact": "facts"
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
  else if ($reference/self::value) then $spec?index("fact:" || $reference/@of)
  else $spec?index($reference/@kind || ":" || $reference/@to)
};

(:~ Every reference in NODES: refs, terms and includes. :)
declare function spec:references($nodes as node()*) as element()* {
  $nodes/descendant-or-self::*[self::ref or self::term[@name] or self::include or self::value]
};

(:~ The key a reference names. :)
declare function spec:reference-key($reference as element()) as xs:string {
  if ($reference/self::term) then "term:" || $reference/@name
  else if ($reference/self::value) then "fact:" || $reference/@of
  else $reference/@kind || ":" || $reference/@to
};

(:~ The articles whose text refers to ARTICLE; an include is not a citation. :)
declare function spec:cited-by($spec as map(*), $article as element()) as element()* {
  let $keys := if ($article/self::term) then $article/name ! ("term:" || .) else spec:key($article)
  for $other in $spec?articles/*
  where not($other is $article)
    and (some $reference in spec:references($other)[not(self::include)] satisfies spec:reference-key($reference) = $keys)
  return $other
};

(:~ The documents that include ARTICLE, or collect its kind. :)
declare function spec:included-in($spec as map(*), $article as element()) as element()* {
  spec:of-kind($spec, "document")[.//include[@kind = local-name($article) and @to = $article/@id]
                                  or .//collect[@kind = local-name($article)]]
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
    case element(value) return "{{" || $node/@of || "}}"
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

(:~
 : NODE (an article, section or block) as Markdown, with heading LEVEL,
 : in one of three modes:
 :   "page":    what people read (docs/), includes and collects expanded;
 :   "outline": the same, each included article as one line naming it;
 :   "source":  what a write takes back (spec:prose): references other
 :              than decision numbers as [[kind:id]], an include as a
 :              line [[include:kind:id]], a collect as [[collect:kind]].
 : Paragraphs are wrapped at 79 columns.
 :)
declare function spec:markdown($spec as map(*), $node as element(), $level as xs:integer,
    $mode as xs:string) as xs:string {
  string-join(spec:wrap(spec:blocks($spec, $node, $level, "", $mode), 79), "&#10;")
};

declare function spec:markdown($spec as map(*), $node as element(), $level as xs:integer) as xs:string {
  spec:markdown($spec, $node, $level, "page")
};

(:~
 : The text of NODE in source form, as `tools/spec/spec text` shows it and
 : `amend --body` and `replace` take it back: an article's body, or the
 : blocks of a section or document, each subsection as [[section:id]].
 :)
declare function spec:source($spec as map(*), $node as element()) as xs:string {
  let $blocks := if ($node/body) then $node/body/* else $node/*[not(self::title or self::summary)]
  let $lines := for $block in $blocks return spec:block($spec, $block, 3, "", "source")
  return string-join(spec:wrap($lines, 79), "&#10;") => replace("\n+$", "")
};

(:~ The articles a collect stands for, in order: decisions by number, others by identifier. :)
declare function spec:collected($spec as map(*), $collect as element(collect)) as element()* {
  for $article in spec:of-kind($spec, $collect/@kind)
  order by spec:number($article), lower-case($article/@id),
           string($article/@id)
  return $article
};

(:~ A decision's number: 120 for D120 and D120b; 0 for other articles. :)
declare function spec:number($article as element()) as xs:integer {
  if ($article/self::decision) then xs:integer(replace($article/@id, "^D(\d+).*$", "$1")) else 0
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
    if ($in-fence or string-length($line) le $width or matches($line, "^\s*(> )*(\||#|<!--|\[\[)")) then $line
    else
      let $prefix := replace($line, "^((\s*(> )*)(([-*+]|\d+[.)]) )*(\[[ xX]\] )?).*$", "$1")
      let $quoted := replace($prefix, "^(\s*(> )*).*$", "$1")
      let $continuation := $quoted || string-join((1 to string-length($prefix) - string-length($quoted)) ! " ")
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
            or matches($word, "^(\||>|```|<!--|\[\[(include|collect|section):)|^([-*+]|\d+[.)]|#{1,6}|-{3,}|\*{3,}|_{3,}|=+)$")) then
          array:put($lines, array:size($lines), $last || " " || $word)
        else array:append($lines, $continuation || $word)
  })
  return $lines?*
};

declare %private function spec:blocks($spec as map(*), $node as element(), $level as xs:integer,
    $indent as xs:string, $mode as xs:string) as xs:string* {
  let $summary := $node/summary ! ("**Summary**: " || spec:inline(node(), false(), $mode), "")
  let $superseded := $node/@superseded-by ! ("*Superseded by " || . || ".*", "")
  let $title := spec:inline($node/title/node(), false(), $mode)
  return typeswitch ($node)
    case element(document) return (
      "# " || $title, "", $summary,
      for $child in $node/*[not(self::title or self::summary)] return spec:block($spec, $child, 2, $indent, $mode))
    case element(decision) return (
      spec:hashes($level) || " " || $node/@id || ". " || $title, "", $superseded, $summary,
      for $child in $node/body/* return spec:block($spec, $child, $level + 1, $indent, $mode),
      $node/sources ! ("**Sources**: " || string-join(source ! spec:source-line(., $mode), "; ") || ".", ""))
    case element(fact) return
      "| `" || $node/@id || "` | " || spec:inline($node/title/node(), true(), $mode) || " | " ||
      (if (exists($spec?values) and map:contains($spec?values, string($node/@id))) then $spec?values(string($node/@id)) else "") ||
      " | " || string-join($node/source ! ("`" || @file || "`" || @written-by ! (" (" || . || ")")), ", ") || " |"
    case element(packet) return (
      spec:hashes($level) || " " || $node/@id || " " || $title, $summary,
      for $child in $node/body/* return spec:block($spec, $child, $level + 1, $indent, $mode))
    case element(log-entry) return (
      spec:hashes($level) || " " || $title, "",
      for $child in $node/body/* return spec:block($spec, $child, $level + 1, $indent, $mode))
    case element(term) return
      for $definition in $node/definition
      return "| " || string-join($node/name ! ("`" || . || "`"), " / ") || " | " ||
             spec:inline($definition/node(), true(), $mode) || " |"
    case element(proposal) return spec:block($spec, spec:proposal-item($node), $level, $indent, $mode)
    default return spec:block($spec, $node, $level, $indent, $mode)
};

(:~ One source of a decision's measurements, as a page shows it. :)
declare function spec:source-line($source as element(source), $mode as xs:string) as xs:string {
  let $how := string-join((
    $source/@tool ! string-join(tokenize(., "\s+")[.] ! ("`" || . || "`"), ", "), $source/@build ! (. || " build"), $source/@at ! ("at " || .),
    $source/@file ! ("from `" || . || "`")), ", ")
  return spec:inline($source/node(), false(), $mode) || ($how[.] ! (" (" || . || ")"))
};

declare %private function spec:proposal-item($proposal as element(proposal)) as element(item) {
  <item>{
    let $first := $proposal/body/*[1]
    return (
      if ($first/self::p) then
        <p><strong>{$proposal/title/node()}</strong>{
          if (matches(string($first), "^[:;,.]")) then () else " ", $first/node() }</p>
      else (<p><strong>{$proposal/title/node()}</strong></p>, $first),
      $proposal/body/*[position() > 1])
  }</item>
};

declare %private function spec:hashes($level as xs:integer) as xs:string {
  string-join((1 to $level) ! "#")
};

declare %private function spec:block($spec as map(*), $node as element(), $level as xs:integer,
    $indent as xs:string, $mode as xs:string) as xs:string* {
  typeswitch ($node)
    case element(section) return
      if ($mode = "source") then ($indent || "[[section:" || $node/@id || "]]", "")
      else
        let $at := if ($node/@level) then xs:integer($node/@level) else $level
        return (
          $indent || spec:hashes($at) || " " || spec:inline($node/title/node(), false(), $mode), "",
          for $child in $node/*[not(self::title)] return spec:block($spec, $child, $at + 1, $indent, $mode))
    case element(include) return
      let $target := spec:target($spec, $node)
      return
        if ($mode = "source") then ($indent || "[[include:" || $node/@kind || ":" || $node/@to || "]]", "")
        else if (empty($target)) then $indent || "<!-- missing " || spec:reference-key($node) || " -->"
        else if ($mode = "outline") then ($indent || "→ " || spec:line($target), "")
        else spec:blocks($spec, $target, $level, $indent, $mode)
    case element(collect) return
      let $articles := spec:collected($spec, $node)
      return
        if ($mode = "source") then ($indent || "[[collect:" || $node/@kind || "]]", "")
        else if ($mode = "outline") then ($indent || "→ every " || $node/@kind || " (" || count($articles) || ")", "")
        else for $article in $articles[not(@status = "unused")] return spec:blocks($spec, $article, $level, $indent, $mode)
    case element(p) return ($indent || spec:inline($node/node(), false(), $mode), "")
    case element(heading) return
      ($indent || spec:hashes(xs:integer($node/@level)) || " " || spec:inline($node/node(), false(), $mode), "")
    case element(rule) return ($indent || "---", "")
    case element(comment) return ($indent || "<!-- " || $node || " -->", "")
    case element(pre) return (
      $indent || "```" || $node/@language,
      tokenize(string($node), "&#10;") ! ($indent || .),
      $indent || "```", "")
    case element(quote) return (
      for $line in $node/* ! spec:block($spec, ., $level, "", $mode) return $indent || ">" || ($line[.] ! (" " || .)), "")
    case element(table) return (
      for $row at $position in $node/*
      return (
        if ($row/self::row) then $indent || "| " || string-join($row/cell ! spec:inline(node(), true(), $mode), " | ") || " |"
        else if ($mode = "source") then $indent || "| " || spec:block($spec, $row, $level, "", $mode)[1] || " |"
        else spec:block($spec, $row, $level, $indent, $mode)[.],
        if ($position = 1 and $row/@header) then
          $indent || "| " || string-join(
            for $cell at $column in $row/cell
            return switch (tokenize(string($node/@align))[$column])
              case "left" return ":---" case "right" return "---:" case "center" return ":---:" default return "---",
            " | ") || " |"
        else ()),
      "")
    case element(list) return (
      let $items :=
        if ($mode = "source") then $node/*
        else for $child in $node/* return
          typeswitch ($child)
            case element(include) return spec:target($spec, $child) ! spec:proposal-item(.)
            case element(collect) return spec:collected($spec, $child) ! spec:proposal-item(.)
            default return $child
      for $item at $position in $items
      let $marker := if ($node/@ordered = "true") then (xs:integer(($node/@start, 1)[1]) + $position - 1) || ". " else "- "
      let $box := if ($item/@checked = "true") then "[x] " else if ($item/@checked = "false") then "[ ] " else ""
      let $lines := if ($item/self::item) then for $block in $item/* return spec:block($spec, $block, $level, "", $mode)
                    else spec:block($spec, $item, $level, "", $mode)
      let $lines := $lines[not(position() = last() and . = "")]
      let $pad := string-join((1 to string-length($marker)) ! " ")
      return (
        if ($position > 1 and ($node/@loose = "true" or $node/(include | collect))) then "" else (),
        $indent || $marker || $box || $lines[1],
        for $line in $lines[position() > 1] return if ($line = "") then "" else $indent || $pad || $line),
      "")
    case element(item) return spec:block($spec, <list>{$node}</list>, $level, $indent, $mode)
    default return $indent || spec:inline($node/node(), false(), $mode)
};

(:~ Inline content as Markdown for people (mode "page"). :)
declare function spec:inline($nodes as node()*) as xs:string {
  spec:inline($nodes, false(), "page")
};

(:~ Inline content as Markdown in MODE; IN-TABLE escapes the cell separator. :)
declare function spec:inline($nodes as node()*, $in-table as xs:boolean, $mode as xs:string) as xs:string {
  string-join(
    for $node in $nodes return
      typeswitch ($node)
        case text() return
          let $text := replace(string($node), if ($mode = "source") then "([\\`*\[])" else "([\\`*])", "\\$1")
          let $text := if ($mode = "source") then replace($text, "\{\{", "\\{{") else $text
          return if ($in-table) then replace($text, "\|", "\\|") else $text
        case element(code) return spec:code(string($node), $in-table)
        case element(term) return spec:code(string($node/@name), $in-table)
        case element(ref) return
          if ($mode = "source" and $node/@kind != "decision") then "[[" || $node/@kind || ":" || $node/@to || "]]"
          else string($node/@to)
        case element(strong) return "**" || spec:inline($node/node(), $in-table, $mode) || "**"
        case element(em) return "*" || spec:inline($node/node(), $in-table, $mode) || "*"
        case element(link) return "[" || spec:inline($node/node(), $in-table, $mode) || "](" || $node/@href || ")"
        case element(recorded) return "Recorded " || $node/@date
        case element(value) return "{{" || $node/@of || "}}"
        case element(status) return "**" || spec:capitalize($node/@value) || "**" || $node/@date ! (" " || .)
        default return spec:inline($node/node(), $in-table, $mode))
};

declare %private function spec:code($text as xs:string, $in-table as xs:boolean) as xs:string {
  let $text := if ($in-table) then replace($text, "\|", "\\|") else $text
  let $longest := max((0, analyze-string($text, "`+")/fn:match ! string-length()))
  let $fence := string-join((0 to $longest) ! "`")
  let $pad := if (contains($text, "`") or starts-with($text, " ") and ends-with($text, " ") and normalize-space($text))
              then " " else ""
  return $fence || $pad || $text || $pad || $fence
};

(: ---- Prose: the source form a write is given ---- :)

(:~
 : Blocks from TEXT in source form (spec:source): paragraphs, headings,
 : bullet, ordered and task lists (nested by indentation), ``` fences,
 : pipe tables, > quotes, ---, <!-- comments -->, and the lines
 : [[include:kind:id]], [[collect:kind]] and [[section:id]]; inline
 : content as spec:prose-inline. Markdown spec:source writes reads back
 : to the same nodes.
 :)
declare function spec:prose($spec as map(*), $text as xs:string) as element()* {
  spec:parse-blocks($spec, tokenize(replace($text, "\r", ""), "\n"))
};

(:~ Blocks of an article's body: as spec:prose, and "Recorded DATE" or
 : "**Done** DATE" at its start become the elements they stand for. :)
declare function spec:prose-body($spec as map(*), $text as xs:string) as element()* {
  let $blocks := spec:prose($spec, $text)
  let $first := $blocks[1]
  let $opening := $first/node()[1]
  return
    if ($first/self::p and $opening/self::text() and matches($opening, "^Recorded \d{4}-\d{2}-\d{2}")) then (
      <p>{ <recorded date="{ substring($opening, 10, 10) }"/>, text { substring($opening, 20) }[string()],
           $first/node()[position() > 1] }</p>,
      tail($blocks))
    else if ($first/self::p and $opening/self::strong and not($opening/*)
             and $opening = ("Done", "Paused", "Design", "Designed", "Revised")) then
      let $after := $first/node()[2]
      let $date := if ($after/self::text() and matches($after, "^ \d{4}-\d{2}-\d{2}")) then substring($after, 2, 10) else ()
      return (
        <p>{ <status value="{ lower-case($opening) }">{ $date ! attribute date { . } }</status>,
             if (exists($date)) then text { substring($after, 12) }[string()] else $after,
             $first/node()[position() > 2] }</p>,
        tail($blocks))
    else $blocks
};

declare %private function spec:first-line($lines as xs:string*, $pattern as xs:string, $after as xs:integer) as xs:integer? {
  (for $line at $position in $lines where $position > $after and matches($line, $pattern) return $position)[1]
};

declare %private function spec:dedent($lines as xs:string*, $width as xs:integer) as xs:string* {
  $lines ! (if (substring(., 1, $width) = string-join((1 to $width) ! " ")) then substring(., $width + 1) else replace(., "^\s+", ""))
};

declare %private function spec:parse-blocks($spec as map(*), $lines as xs:string*) as element()* {
  if (empty($lines)) then ()
  else if (not(normalize-space($lines[1]))) then spec:parse-blocks($spec, tail($lines))
  else
    let $first := $lines[1]
    return
      if (starts-with($first, "```")) then
        let $end := (spec:first-line($lines, "^```\s*$", 1), count($lines) + 1)[1]
        return (
          <pre>{ substring($first, 4)[normalize-space()] ! attribute language { normalize-space(.) },
                 string-join(subsequence($lines, 2, $end - 2), "&#10;") }</pre>,
          spec:parse-blocks($spec, subsequence($lines, $end + 1)))
      else if (starts-with($first, "<!--")) then
        let $end := (spec:first-line($lines, "-->", 0), count($lines))[1]
        let $text := string-join(subsequence($lines, 1, $end), "&#10;")
        return (
          <comment>{ normalize-space(replace(replace($text, "^<!--", ""), "-->\s*$", "")) }</comment>,
          spec:parse-blocks($spec, subsequence($lines, $end + 1)))
      else if (matches($first, "^\[\[(include|collect|section):[^\]]+\]\]\s*$")) then
        let $key := tokenize(replace($first, "^\[\[|\]\]\s*$", ""), ":")
        return (
          switch ($key[1])
            case "include" return <include kind="{ $key[2] }" to="{ string-join(subsequence($key, 3), ':') }"/>
            case "collect" return <collect kind="{ $key[2] }"/>
            default return <section id="{ string-join(tail($key), ':') }"/>,
          spec:parse-blocks($spec, tail($lines)))
      else if (matches($first, "^#{1,6} ")) then (
        <heading level="{ string-length(substring-before($first, ' ')) }">{
          spec:prose-inline($spec, substring-after($first, " ")) }</heading>,
        spec:parse-blocks($spec, tail($lines)))
      else if (matches($first, "^(---+|\*\*\*+|___+)\s*$")) then (<rule/>, spec:parse-blocks($spec, tail($lines)))
      else if (starts-with($first, "|")) then
        let $end := (spec:first-line($lines, "^[^|]|^$", 1), count($lines) + 1)[1] - 1
        return (spec:parse-table($spec, subsequence($lines, 1, $end)), spec:parse-blocks($spec, subsequence($lines, $end + 1)))
      else if (starts-with($first, ">")) then
        let $end := (spec:first-line($lines, "^[^>]|^$", 1), count($lines) + 1)[1] - 1
        return (
          <quote>{ spec:parse-blocks($spec, subsequence($lines, 1, $end) ! replace(., "^> ?", "")) }</quote>,
          spec:parse-blocks($spec, subsequence($lines, $end + 1)))
      else if (matches($first, "^([-*+]|\d+[.)]) ")) then spec:parse-list($spec, $lines)
      else
        let $end := (spec:first-line($lines, "^\s*$|^(```|<!--|#{1,6} |\||>|[-*+] |\d+[.)] |\[\[(include|collect|section):)", 1),
                     count($lines) + 1)[1] - 1
        return (
          <p>{ spec:prose-inline($spec, string-join(subsequence($lines, 1, $end) ! normalize-space(), " ")) }</p>,
          spec:parse-blocks($spec, subsequence($lines, $end + 1)))
};

declare %private function spec:parse-list($spec as map(*), $lines as xs:string*) as element()* {
  let $ordered := matches($lines[1], "^\d")
  let $marker := if ($ordered) then "^\d+[.)] " else "^[-*+] "
  (: Lines that start an item: a marker in column 1, until the list ends. :)
  let $starts := spec:item-starts($lines, $marker, 1, ())
  let $end := spec:list-end($lines, $marker, $starts)
  let $start := if ($ordered) then xs:integer(replace($lines[1], "^(\d+).*$", "$1")) else ()
  return (
    <list>{
      if ($ordered) then attribute ordered { "true" } else (),
      if ($ordered and $start != 1) then attribute start { $start } else (),
      if ((some $position in tail($starts) satisfies not(normalize-space($lines[$position - 1])))
          and not(every $first in $starts satisfies matches($lines[$first], "^\S+ \[\[(include|collect):")))
      then attribute loose { "true" } else (),
      for $item-start at $number in $starts
      let $item-end := (($starts[$number + 1], $end + 1)[1]) - 1
      let $text := replace($lines[$item-start], $marker, "")
      let $width := string-length($lines[$item-start]) - string-length($text)
      let $box := if (matches($text, "^\[[ xX]\] ")) then substring($text, 2, 1) else ()
      let $text := if (exists($box)) then substring($text, 5) else $text
      let $body := ($text, spec:dedent(subsequence($lines, $item-start + 1, $item-end - $item-start), $width))
      let $blocks := spec:parse-blocks($spec, $body)
      return
        if (count($blocks) = 1 and $blocks/(self::include | self::collect) and empty($box)) then $blocks
        else <item>{
          if (exists($box)) then attribute checked { $box != " " } else (),
          $blocks
        }</item>
    }</list>,
    spec:parse-blocks($spec, subsequence($lines, $end + 1))
  )
};

(: The positions of the lines that start items of the list at the start of LINES. :)
declare %private function spec:item-starts($lines as xs:string*, $marker as xs:string, $position as xs:integer,
    $found as xs:integer*) as xs:integer* {
  if ($position > count($lines)) then $found
  else
    let $line := $lines[$position]
    return
      if (matches($line, $marker)) then spec:item-starts($lines, $marker, $position + 1, ($found, $position))
      else if (matches($line, "^\s+\S") or not(normalize-space($line))) then
        (: An indented line continues the item; a blank line does if an indented line or a marker follows. :)
        if (normalize-space($line)) then spec:item-starts($lines, $marker, $position + 1, $found)
        else
          let $next := spec:first-line($lines, "\S", $position)
          return if (exists($next) and (matches($lines[$next], "^\s+\S") or matches($lines[$next], $marker)))
                 then spec:item-starts($lines, $marker, $position + 1, $found) else $found
      else $found
};

(: The last line of the list whose items start at STARTS: before the first
   line after the last start that is neither blank nor indented, blank lines
   not counted. :)
declare %private function spec:list-end($lines as xs:string*, $marker as xs:string, $starts as xs:integer*) as xs:integer {
  let $stop := (spec:first-line($lines, "^\S", $starts[last()]), count($lines) + 1)[1]
  return (for $position in reverse($starts[last()] to $stop - 1)
          where normalize-space($lines[$position]) return $position)[1]
};

declare %private function spec:parse-table($spec as map(*), $lines as xs:string*) as element(table) {
  let $rows := $lines ! spec:split-row(.)
  let $delimiter := count($lines) > 1 and matches($lines[2], "^\|?(\s*:?-+:?\s*\|)+\s*:?-*:?\s*$")
  let $alignments := if ($delimiter) then
      for $cell in spec:split-row($lines[2])
      return if (starts-with($cell, ":") and ends-with($cell, ":")) then "center"
             else if (ends-with($cell, ":")) then "right" else if (starts-with($cell, ":")) then "left" else "default"
    else ()
  return <table>{
    if ($alignments != "default") then attribute align { string-join($alignments, " ") } else (),
    for $line at $position in $lines
    let $cells := spec:split-row($line)
    where not($delimiter and $position = 2)
    return
      if (count($cells) = 1 and matches($cells, "^\[\[(include|collect):[^\]]+\]\]$")) then spec:parse-blocks($spec, $cells)
      else <row>{
        if ($delimiter and $position = 1) then attribute header { "true" } else (),
        $cells ! <cell>{ spec:prose-inline($spec, .) }</cell>
      }</row>
  }</table>
};

(: The cells of a table row: "|" separates, except escaped or in code; "\|" becomes "|". :)
declare %private function spec:split-row($line as xs:string) as xs:string* {
  let $text := replace(replace(normalize-space($line), "^\|", ""), "([^\\])\|$|^\|$", "$1")
  let $state := fold-left(analyze-string($text, "\\\||`+|\||[^\\`|]+|\\")/*, { "cells": (), "current": "", "code": 0 },
    fn($state, $part) {
      let $value := string($part)
      return
        if ($state?code > 0) then
          map:put(map:put($state, "current", $state?current || (if ($value = "\|") then "|" else $value)),
                  "code", if (matches($value, "^`+$") and string-length($value) = $state?code) then 0 else $state?code)
        else if ($value = "|") then map:put(map:put($state, "cells", ($state?cells, $state?current)), "current", "")
        else if ($value = "\|") then map:put($state, "current", $state?current || "|")
        else if (matches($value, "^`+$")) then
          map:put(map:put($state, "current", $state?current || $value), "code", string-length($value))
        else map:put($state, "current", $state?current || $value)
    })
  return ($state?cells, $state?current) ! normalize-space()
};

(:~
 : Inline content from TEXT in source form: `code` (a term when it names
 : one), **strong**, *em*, [text](link), [[kind:identifier]] for any
 : reference, backslash escapes, and decision numbers (D238), which are
 : always references.
 :)
declare function spec:prose-inline($spec as map(*), $text as xs:string) as node()* {
  if ($text = "") then ()
  else
    let $plain := replace($text, "^([^\\`*\[{]*).*$", "$1", "s")
    let $rest := substring($text, string-length($plain) + 1)
    return (spec:prose-references($spec, $plain), if ($rest = "") then () else spec:prose-special($spec, $rest))
};

declare %private function spec:prose-special($spec as map(*), $text as xs:string) as node()* {
  if (starts-with($text, "\") and string-length($text) > 1) then
    (text { substring($text, 2, 1) }, spec:prose-inline($spec, substring($text, 3)))
  else if (starts-with($text, "`")) then
    let $run := replace($text, "^(`+).*$", "$1", "s")
    let $after := substring($text, string-length($run) + 1)
    let $close := spec:closing-run($after, string-length($run))
    return
      if (empty($close)) then (text { $run }, spec:prose-inline($spec, $after))
      else
        let $content := substring($after, 1, $close - 1)
        let $content := if (string-length($content) > 2 and starts-with($content, " ") and ends-with($content, " ")
                            and normalize-space($content)) then substring($content, 2, string-length($content) - 2)
                        else $content
        return (
          if (exists($spec?index("term-name:" || $content))) then <term name="{ $content }"/> else <code>{ $content }</code>,
          spec:prose-inline($spec, substring($after, $close + string-length($run))))
  else if (matches($text, "^\{\{[a-z0-9][^}]*\}\}", "s")) then
    let $fact := replace($text, "^\{\{([^}]*)\}\}.*$", "$1", "s")
    return (<value of="{ $fact }"/>, spec:prose-inline($spec, substring($text, string-length($fact) + 5)))
  else if (starts-with($text, "{")) then (text { "{" }, spec:prose-inline($spec, substring($text, 2)))
  else if (matches($text, "^\[\[[a-z-]+:[^\]]+\]\]", "s")) then
    let $key := replace($text, "^\[\[([^\]]+)\]\].*$", "$1", "s")
    return (<ref kind="{ substring-before($key, ':') }" to="{ substring-after($key, ':') }"/>,
            spec:prose-inline($spec, substring($text, string-length($key) + 5)))
  else if (matches($text, "^\[([^\]]*)\]\(([^)\s]*)\)", "s")) then
    let $label := replace($text, "^\[([^\]]*)\]\(.*$", "$1", "s")
    let $href := replace($text, "^\[[^\]]*\]\(([^)\s]*)\).*$", "$1", "s")
    return (<link href="{ $href }">{ spec:prose-inline($spec, $label) }</link>,
            spec:prose-inline($spec, substring($text, string-length($label) + string-length($href) + 5)))
  else if (starts-with($text, "**")) then
    let $close := spec:find-outside-code(substring($text, 3), "**")
    return
      if (empty($close)) then (text { "**" }, spec:prose-inline($spec, substring($text, 3)))
      else (<strong>{ spec:prose-inline($spec, substring($text, 3, $close - 1)) }</strong>,
            spec:prose-inline($spec, substring($text, $close + 4)))
  else if (starts-with($text, "*")) then
    let $close := spec:find-outside-code(substring($text, 2), "*")
    return
      if (empty($close)) then (text { "*" }, spec:prose-inline($spec, substring($text, 2)))
      else (<em>{ spec:prose-inline($spec, substring($text, 2, $close - 1)) }</em>,
            spec:prose-inline($spec, substring($text, $close + 2)))
  else (text { substring($text, 1, 1) }, spec:prose-inline($spec, substring($text, 2)))
};

(: Where in TEXT a run of exactly LENGTH backticks starts. :)
declare %private function spec:closing-run($text as xs:string, $length as xs:integer) as xs:integer? {
  let $parts := analyze-string($text, "`+")/*
  return (
    for $part at $number in $parts
    where $part/self::fn:match and string-length($part) = $length
    return 1 + sum(subsequence($parts, 1, $number - 1) ! string-length())
  )[1]
};

(: Where in TEXT the delimiter NEEDLE ("**" or "*") is, outside code spans and escapes. :)
declare %private function spec:find-outside-code($text as xs:string, $needle as xs:string) as xs:integer? {
  let $parts := analyze-string($text, "\\.|`+|\*\*|\*")/*
  let $state := fold-left($parts, { "position": 1, "code": 0, "found": () }, fn($state, $part) {
    if (exists($state?found)) then $state
    else
      let $value := string($part)
      let $next := map:put($state, "position", $state?position + string-length($value))
      return
        if ($part/self::fn:non-match) then $next
        else if ($state?code > 0) then
          if (matches($value, "^`+$") and string-length($value) = $state?code) then map:put($next, "code", 0) else $next
        else if (matches($value, "^`+$")) then
          if (exists(spec:closing-run(substring($text, $state?position + string-length($value)), string-length($value))))
          then map:put($next, "code", string-length($value)) else $next
        else if ($value = $needle) then map:put($state, "found", $state?position)
        else $next
  })
  return $state?found
};

(: Decision numbers in TEXT as references: D238, and D120b when that decision exists. :)
declare %private function spec:prose-references($spec as map(*), $text as xs:string) as node()* {
  for $part in analyze-string($text, "[A-Za-z0-9_]+")/*
  return
    if ($part/self::fn:match and (matches($part, "^D[1-9][0-9]*$")
        or matches($part, "^D[1-9][0-9]*[a-z]$") and exists($spec?index("decision:" || $part))))
    then <ref kind="decision" to="{ $part }"/>
    else text { $part }
};

(: ---- Facts ---- :)

(:~ The module that computes FACTS: one function per fact, in their order. :)
declare function spec:fact-module($facts as element(fact)*) as xs:string {
  string-join((
    'module namespace fact = "urn:sxml:spec:fact";',
    'declare base-uri "' || $spec:root || '";',
    for $fact at $position in $facts
    return "declare function fact:f" || $position || "() as xs:anyAtomicType* { " || $fact/expression || " };"
  ), "&#10;")
};

(:~ A fact's value as text: an integer with thousands separators, anything else as it is. :)
declare function spec:fact-text($value as xs:anyAtomicType) as xs:string {
  if ($value instance of xs:integer) then format-integer($value, "#,##0") else string($value)
};

(:~
 : The values of FACTS, a map from identifier to text, computed in one
 : trusted module; or, for a fact that fails or gives other than one
 : item, "error: " and why.
 :)
declare function spec:fact-values($facts as element(fact)*) as map(xs:string, xs:string) {
  let $call := fn($module, $position) {
    let $values := $module?functions(QName("urn:sxml:spec:fact", "f" || $position))?0()
    return if (count($values) = 1) then spec:fact-text($values) else "error: " || count($values) || " values"
  }
  let $all := try { load-xquery-module("urn:sxml:spec:fact", { "trusted": true(), "content": spec:fact-module($facts) }) }
              catch * { () }
  return map:merge(
    for $fact at $position in $facts
    return map:entry(string($fact/@id),
      if (exists($all)) then try { $call($all, $position) } catch * { "error: " || $err:description }
      else
        (: The module does not compile: compile each fact alone to find which. :)
        try {
          let $one := load-xquery-module("urn:sxml:spec:fact", { "trusted": true(), "content": spec:fact-module($fact) })
          return $call($one, 1)
        } catch * { "error: " || $err:description }))
};

(:~
 : SPEC with the facts that NODES show computed: in "values", and in place
 : of each value element of the articles, as text.
 :)
declare function spec:with-values($spec as map(*), $nodes as node()*) as map(*) {
  let $wanted := distinct-values(($nodes//value/@of, $nodes/descendant-or-self::fact/@id,
                                  if ($nodes//collect[@kind = "fact"]) then spec:of-kind($spec, "fact")/@id else ()))
  let $facts := spec:of-kind($spec, "fact")[@id = $wanted]
  return
    if (empty($facts)) then $spec
    else
      let $values := spec:fact-values($facts)
      let $articles := $spec?articles ! (
        if (.//value[@of = $wanted]) then document { * transform with {
          for $value in .//value[map:contains($values, string(@of))] return replace node $value with text { $values(string($value/@of)) }
        } } else .)
      return map:put(spec:load($articles), "values", $values)
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
  let $collected := distinct-values($all/self::document//collect/@kind)
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
    where not($included(spec:key($article)) or local-name($article) = $collected)
    return "error: " || spec:key($article) || " is in no document",
    for $term in $all/self::term[count(definition) > 1]
    return "warning: term:" || $term/@id || " has " || count($term/definition) || " definitions",
    spec:check-measurements($spec)
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

(:~ A number that reports a measurement: 32,815 or 99.8 %, not a target such as "≥ 90 %". :)
declare variable $spec:measured as xs:string := "\d{1,3}(,\d{3})+|\d+(\.\d+)? ?%";

declare function spec:measurements($text as xs:string) as xs:string* {
  let $parts := analyze-string($text, $spec:measured)/*
  for $part at $position in $parts
  where $part/self::fn:match and not(matches(string($parts[$position - 1]), "[≥≤<>]\s*$"))
  return string($part)
};

(:~
 : Where measured numbers lack a source: a decision that states one and
 : has no sources; and, in a document, packet or proposal, a number that
 : is not a fact's value and stands in no row, list item or paragraph that
 : cites the decision, packet or log entry that measured it, under no
 : section or article title that does (in a packet, the decision of its
 : done line counts for all of it). Log entries are records of their own,
 : as decisions are; terms define words. Facts that fail, and files a fact or a source names that do not
 : exist, are errors too.
 :)
declare function spec:check-measurements($spec as map(*)) as xs:string* {
  let $all := $spec?articles/*
  let $texts := fn($nodes) { $nodes//text()[not(ancestor::code or ancestor::pre or ancestor::sources)] }
  return (
    for $decision in $all/self::decision[not(@status = "unused")][empty(sources)]
    let $numbers := $texts($decision) ! spec:measurements(.)
    where exists($numbers)
    return "error: " || spec:key($decision) || " states measurements (" || string-join(subsequence($numbers, 1, 3), ", ") ||
           (if (count($numbers) > 3) then ", ..." else "") || ") but has no sources",
    for $article in $all[not(self::decision or self::fact or self::log-entry or self::term)]
    let $packet-cites := $article/self::packet/body/*[1][status]//ref[@kind = "decision"]
    for $text in $texts($article)
    let $numbers := spec:measurements($text)
    where exists($numbers) and empty($packet-cites)
    let $record := ($text/ancestor::*[self::row or self::item or self::p or self::definition or self::cell or self::title or self::summary][last()])
    let $record := ($text/ancestor::row, $text/ancestor::item[parent::list[not(ancestor::item)]], $record)[1]
    let $scope := ($record, $text/ancestor::section/title, $article/title)
    where empty($scope//ref[@kind = ("decision", "log-entry", "packet")])
    return "error: " || spec:key($article) || " states " || $numbers[1] || " with no fact and no decision it comes from: " ||
           substring(normalize-space(spec:plain($text)), 1, 80),
    for $fact in $all/self::fact, $file in $fact/source/@file
    where not(unparsed-text-available(spec:uri($file)))
    return "error: " || spec:key($fact) || " reads " || $file || ", which does not exist",
    for $source in $all/self::decision/sources/source, $path in $source/(@tool, @file) ! tokenize(., "[,;\s]+")[contains(., "/")]
    where not(unparsed-text-available(spec:uri($path)))
    return "warning: " || spec:key($source/ancestor::decision) || " cites " || $path || ", which no longer exists"
  )
};

(:~ The facts that fail: computed apart, because their sources are read. :)
declare function spec:check-facts($spec as map(*)) as xs:string* {
  let $facts := spec:of-kind($spec, "fact")
  let $values := spec:fact-values($facts)
  for $fact in $facts
  let $value := $values(string($fact/@id))
  where starts-with($value, "error: ")
  return "error: " || spec:key($fact) || ": " || substring-after($value, "error: ")
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

(:~ What removing ARTICLE would break: the errors spec:check would find that it does not now. :)
declare function spec:removal-refusals($spec as map(*), $article as element()) as xs:string* {
  let $after := spec:load($spec?articles[not(* is $article)])
  return spec:check($after)[starts-with(., "error:")][not(. = spec:check($spec))]
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
    else
      (: fn:put does not make directories: a new article's is made first (EXPath File). :)
      let $made := for $article in $changed
                   let $directory := replace(spec:file($spec, $article), "/[^/]+$", "") => substring-after("file://")
                   where not(file:exists($directory))
                   return file:create-dir($directory)
      for $article in $changed return spec:put($article, spec:file($spec, $article))
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
