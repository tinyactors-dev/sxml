declare namespace app = "https://example.org/app";

declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $books := //book;

(:~ The book with the id, or the error app:unknown-book. :)
declare function local:book($id as xs:string) as element(book) {
  $books[@id = $id]
  otherwise error(xs:QName("app:unknown-book"), "No book has the id " || $id, $id)
};

for $id in ("dune", "emma", "sicp")
return
  try {
    local:book($id)/title/string()
  } catch app:unknown-book {
    "(skipped " || $err:value || ": " || $err:description || ")"
  }
