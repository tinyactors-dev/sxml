xquery version "4.0";
(: The text of an SVG drawing (the context item), one line per text
   element, in drawing order. :)
declare namespace svg = "http://www.w3.org/2000/svg";
declare option output:method "text";

string-join(//svg:text ! normalize-space(), "&#10;")
