(: A syntax error inside an enclosed expression of a string
   constructor is reported at its own place (D287). :)
declare function local:field($name as xs:string) as xs:string {
  ``[  `{ $name }`: `{ upper-case($name) + }`;]``
};
local:field("id")
