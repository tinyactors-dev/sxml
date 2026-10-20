import module namespace catalog = "https://example.org/catalog" at "catalog.xqm";

declare option output:method "text";
declare option output:item-separator "&#10;";

declare variable $id as xs:string external := "sicp";

catalog:describe(catalog:book(/, $id))
