import schema namespace sc = "http://www.w3.org/2005/07/scxml" at "scxml.xsd";
declare namespace vane = "urn:example:vane";

let $machine := validate { / }
for $data in $machine//sc:data
return `{$data/@id}: {data($data/@vane:type)}`
