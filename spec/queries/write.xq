xquery version "4.0";

(:~
 : Every write of tools/spec/spec. VERB says which; each computes the
 : articles it changes or adds, and spec:store stores them all or none:
 : nothing is stored that does not validate or that adds an error to
 : spec:check. Text parameters are prose (spec:prose).
 :
 :   add-decision   title, summary?, body?, after?   → D<next>, in DECISIONS#accepted
 :   add-proposal   title, summary?, body?           → in DECISIONS#proposed
 :   add-term       name, definition, after?         → in the GLOSSARY table
 :   add-packet     name, document, title, body?     → in DOCUMENT#packets
 :   amend          name, field (title, summary, definition, body), text
 :   supersede      name, by
 :   rename-term    name, to
 :   set-status     name (a packet), value, date?
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

declare variable $today as xs:string := string(current-date()) => substring(1, 10);

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

(:~ The new article, and the document that now includes it. :)
declare function local:placed($spec as map(*), $article as element(), $document as xs:string, $section as xs:string?) as element()+ {
  ($article, spec:with-include(spec:one($spec, "document:" || $document), $section, $article))
};

declare function local:changes($spec as map(*)) as element()* {
  switch ($verb)
  case "add-decision" return
    let $numbers := spec:of-kind($spec, "decision")/@id ! xs:integer(substring(., 2))
    let $identifier := "D" || (max(($numbers, 0)) + 1)
    let $cited := tokenize($after, "[\s,]+")[.]
    let $article :=
      <decision id="{ $identifier }" status="accepted">{
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
    return local:placed($spec, $article, "DECISIONS", "accepted")
  case "add-proposal" return
    let $article :=
      <proposal id="{ replace(lower-case(local:required($title, "--title")), "[^a-z0-9]+", "-") => replace("^-|-$", "") }">{
        local:inline($spec, "title", $title),
        local:summary($spec),
        <body>{ spec:prose($spec, $body) }</body>
      }</proposal>
    let $decisions := spec:one($spec, "document:DECISIONS")
    let $include := <include kind="proposal" to="{ $article/@id }"/>
    return ($article, $decisions transform with {
      insert node $include as last into (.//section[@id = "proposed"]//list)[1]
    })
  case "add-term" return
    let $term := local:required($name, "a name")
    let $article := <term id="{ $term }"><name>{ $term }</name>{ local:inline($spec, "definition", local:required($definition, "--definition")) }</term>
    let $glossary := spec:one($spec, "document:GLOSSARY")
    let $include := <include kind="term" to="{ $term }"/>
    return ($article,
      if ($after) then
        let $previous := spec:one($spec, "term:" || $after)
        return $glossary transform with {
          insert node $include after (.//include[@kind = "term" and @to = $previous/@id])[last()]
        }
      else spec:with-include($glossary, (), $article))
  case "add-packet" return
    let $article :=
      <packet id="{ local:required($name, "an identifier") }">{
        local:inline($spec, "title", local:required($title, "--title")),
        local:summary($spec),
        <body>{ spec:prose($spec, $body) }</body>
      }</packet>
    return local:placed($spec, $article, local:required($document, "--document"), "packets")
  case "amend" return
    let $article := spec:one($spec, local:required($name, "a name"))
    let $new := local:required($text, "the new text")
    return $article transform with {
      switch ($field)
      case "title" return replace node title with local:inline($spec, "title", $new)
      case "summary" return
        if (summary) then replace node summary with local:inline($spec, "summary", $new)
        else insert node local:inline($spec, "summary", $new) after title
      case "definition" return
        let $number := if ($value) then xs:integer($value) else 1
        return replace node definition[$number] with local:inline($spec, "definition", $new)
      case "body" return replace node body with <body>{ spec:prose($spec, $new) }</body>
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
  default return error(xs:QName("spec:usage"), "no write is called " || $verb)
};

let $spec := spec:load()
return spec:store($spec, local:changes($spec))
