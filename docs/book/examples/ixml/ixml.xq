(: Parses the file $input with the Invisible XML grammar in the file
   $grammar and writes the XML, indented. :)
declare option output:indent "yes";
declare variable $grammar external;
declare variable $input external;
invisible-xml(unparsed-text($grammar))(unparsed-text($input))
