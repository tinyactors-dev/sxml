module namespace geometry = "http://example.com/geometry";
declare variable $geometry:unit := "m2";
declare function geometry:area($width, $height) { $width * $height };
