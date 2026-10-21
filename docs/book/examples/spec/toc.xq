declare option output:method "text";
declare option output:item-separator "&#10;";

(:~ The table of contents of some sections and, after each one, of the
    sections inside it. :)
declare function local:toc($sections as element(section)*, $prefix as xs:string) as xs:string* {
  for $section at $position in $sections
  let $number := $prefix || $position
  return ($number || ". " || $section/name, local:toc($section/section, $number || "."))
};

local:toc(/rfc/middle/section, "")
