declare variable $file external := "AGENTS.md";

let $parse := invisible-xml(unparsed-text("rules.ixml"))
let $markdown := sxml:parse-code(unparsed-text($file),
                                 "markdown")
return <rules>{
  for $item in $markdown//section[atx_heading/inline
                                  = "Rules"]//list_item
  let $text := normalize-space($item/paragraph)
  let $rule := $parse($text)/rule/*
  return ($rule, <unknown-rule text="{$text}"/>)[1]
}</rules>
