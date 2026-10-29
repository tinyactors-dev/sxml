xquery version "4.0";

(:~
 : Every write of tools/spec/spec. VERB says which; each computes the
 : articles it changes or adds, and spec:store stores them all or none:
 : nothing is stored that does not validate or that adds an error to
 : spec:check. Text parameters are in source form (spec:prose).
 :
 :   add-decision   title, summary?, body?, after?   → D<next>
 :   add-proposal   title, summary?, body?
 :   add-term       name, definition
 :   add-packet     name, document, title, summary?, body?  → in DOCUMENT#packets
 :   amend          name (an article, document or section),
 :                  field (title, summary, definition, body), text, value?
 :   supersede      name, by
 :   rename-term    name, to
 :   set-status     name (a packet), value, date?
 :   log            text, date?                       → this week's log entry
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

declare variable $verb as xs:string external;
declare variable $name as xs:string external := "";
declare variable $title as xs:string external := "";
declare variable $summary as xs:string external := "";
declare variable $body as xs:string external := "";
declare variable $after as xs:string external := "";
declare variable $definition as xs:string external := "";
declare variable $document as xs:string external := "";
declare variable $field as xs:string external := "";
declare variable $text as xs:string external := "";
declare variable $by as xs:string external := "";
declare variable $to as xs:string external := "";
declare variable $value as xs:string external := "";
declare variable $date as xs:string external := "";

declare variable $today as xs:date := current-date() => string() => substring(1, 10) => xs:date();

declare function local:required($parameter as xs:string, $what as xs:string) as xs:string {
  if (normalize-space($parameter)) then $parameter
  else error(xs:QName("spec:usage"), $verb || " needs " || $what)
};

declare function local:summary($spec as map(*)) as element(summary)? {
  if ($summary) then <summary>{ spec:prose-inline($spec, normalize-space($summary)) }</summary> else ()
};

declare function local:inline($spec as map(*), $element as xs:string, $text as xs:string) as element() {
  element { $element } { spec:prose-inline($spec, normalize-space($text)) }
};

(:~ The new content of a document or section from TEXT: its subsections stay, at their [[section:id]]. :)
declare function local:content($spec as map(*), $node as element(), $text as xs:string) as element()* {
  let $blocks := spec:prose($spec, $text)
  let $kept := $blocks/self::section/@id ! string()
  let $existing := $node/section/@id ! string()
  return
    if (not(deep-equal(sort($kept), sort($existing)))) then
      error(xs:QName("spec:refused"), "refused: the text must keep one line [[section:ID]] for each subsection (" ||
        string-join($existing, ", ") || ") and name no other")
    else for $block in $blocks return if ($block/self::section) then $node/section[@id = $block/@id] else $block
};

declare function local:changes($spec as map(*)) as element()* {
  switch ($verb)
  case "add-decision" return
    let $numbers := spec:of-kind($spec, "decision") ! spec:number(.)
    let $cited := tokenize($after, "[\s,]+")[.]
    return
      <decision id="D{ max(($numbers, 0)) + 1 }" status="accepted">{
        local:inline($spec, "title", local:required($title, "--title")),
        local:summary($spec),
        <body>
          <p><recorded date="{ $today }"/>{
            if (exists($cited)) then (", after ", for $other at $position in $cited return (
              if ($position > 1) then ", " else (), spec:prose-inline($spec, $other))) else (),
            "."
          }</p>
          { spec:prose($spec, $body) }
        </body>
      }</decision>
  case "add-proposal" return
    <proposal id="{ replace(lower-case(local:required($title, "--title")), "[^a-z0-9]+", "-") => replace("^-|-$", "") }">{
      local:inline($spec, "title", $title),
      local:summary($spec),
      <body>{ spec:prose($spec, $body) }</body>
    }</proposal>
  case "add-term" return
    let $term := local:required($name, "a name")
    return <term id="{ $term }"><name>{ $term }</name>{
             local:inline($spec, "definition", local:required($definition, "--definition")) }</term>
  case "add-packet" return
    let $article :=
      <packet id="{ local:required($name, "an identifier") }">{
        local:inline($spec, "title", local:required($title, "--title")),
        local:summary($spec),
        <body>{ spec:prose($spec, $body) }</body>
      }</packet>
    return ($article,
            spec:with-include(spec:one($spec, "document:" || local:required($document, "--document")), "packets", $article))
  case "amend" return
    let $target := spec:one($spec, local:required($name, "a name"))
    let $section := $target/self::section/@id
    let $new := local:required($text, "the new text")
    return $target/ancestor-or-self::*[last()] transform with {
      let $node := if (exists($section)) then .//section[@id = $section] else .
      return switch ($field)
      case "title" return replace node $node/title with local:inline($spec, "title", $new)
      case "summary" return
        if ($node/summary) then replace node $node/summary with local:inline($spec, "summary", $new)
        else insert node local:inline($spec, "summary", $new) after $node/title
      case "definition" return
        let $number := if ($value) then xs:integer($value) else 1
        return replace node $node/definition[$number] with local:inline($spec, "definition", $new)
      case "body" return
        if ($node/body) then replace node $node/body with <body>{ spec:prose-body($spec, $new) }</body>
        else (delete nodes $node/*[not(self::title or self::summary)],
              insert nodes local:content($spec, $target, $new) as last into $node)
      default return error(xs:QName("spec:usage"), "amend changes --title, --summary, --definition or --body")
    }
  case "supersede" return
    let $article := spec:one($spec, "decision:" || replace(local:required($name, "a decision"), "^decision:", ""))
    let $successor := spec:one($spec, "decision:" || replace(local:required($by, "--by"), "^decision:", ""))
    return $article transform with {
      replace value of node @status with "superseded",
      if (@superseded-by) then replace value of node @superseded-by with $successor/@id
      else insert node attribute superseded-by { $successor/@id } into .
    }
  case "rename-term" return
    let $term := spec:one($spec, "term:" || replace(local:required($name, "a term"), "^term:", ""))
    let $old := replace($name, "^term:", "")
    let $new := local:required($to, "--to")
    return (
      if (exists($spec?index("term-name:" || $new))) then error(xs:QName("spec:refused"), "refused: term:" || $new || " exists")
      else (),
      for $article in $spec?articles/*
      where $article is $term or exists($article//term[@name = $old])
      return $article transform with {
        for $use in .//term[@name = $old] return replace value of node $use/@name with $new,
        for $own in self::term/name[. = $old] return replace value of node $own with $new
      })
  case "set-status" return
    let $packet := spec:one($spec, "packet:" || replace(local:required($name, "a packet"), "^packet:", ""))
    let $status := <status value="{ local:required($value, "a status") }">{
                     attribute date { if ($date) then $date else $today } }</status>
    return $packet transform with {
      let $first := body/*[1]
      return
        if ($first/status) then replace node $first/status with $status
        else if ($first/self::p) then insert node ($status, text { " " }) as first into $first
        else insert node <p>{ $status }</p> as first into body
    }
  case "log" return
    let $day := if ($date) then xs:date($date) else $today
    (: 2024-01-01 was a Monday; a week's entry is named after its Monday. :)
    let $monday := $day - xs:dayTimeDuration("P" || days-from-duration($day - xs:date("2024-01-01")) mod 7 || "D")
    let $identifier := "week-of-" || $monday
    let $item := <item>{ spec:prose($spec, local:required($text, "the entry's text")) }</item>
    let $entry := $spec?index("log-entry:" || $identifier)
    return
      if (exists($entry)) then $entry transform with {
        if (body/list) then insert node $item as first into body/list[1]
        else insert node <list loose="true">{ $item }</list> as first into body
      }
      else (
        <log-entry id="{ $identifier }"><title>Week of { string($monday) }</title><body><list loose="true">{ $item }</list></body></log-entry>,
        spec:one($spec, "document:LOG") transform with {
          let $include := <include kind="log-entry" to="{ $identifier }"/>
          let $first := (.//include[@kind = "log-entry"])[1]
          return if (exists($first)) then insert node $include before $first else insert node $include as last into .
        })
  default return error(xs:QName("spec:usage"), "no write is called " || $verb)
};

let $spec := spec:load()
return spec:store($spec, local:changes($spec))
