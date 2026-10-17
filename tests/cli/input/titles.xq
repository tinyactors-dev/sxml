(: The titles of the context document, as an HTML list. :)
declare option output:method "html";
declare option output:indent "no";
declare variable $heading external := "Books";
<ul title="{$heading}">{
  for $book in //book
  order by $book/title
  return <li>{$book/title/string()}<br/></li>
}</ul>
