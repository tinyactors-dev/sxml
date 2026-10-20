(:~ Turns an XSD schema and an API description into TypeScript. :)
module namespace ts = "https://example.org/typescript";

declare namespace xs = "http://www.w3.org/2001/XMLSchema";
declare namespace api = "https://example.org/api";

(:~ The built-in XSD types the generator knows: their TypeScript type and check. :)
declare variable $ts:built-in := {
  "xs:string":  { "type": "string",  "check": "checkString" },
  "xs:integer": { "type": "number",  "check": "checkInteger" },
  "xs:decimal": { "type": "number",  "check": "checkDecimal" },
  "xs:boolean": { "type": "boolean", "check": "checkBoolean" },
  "xs:date":    { "type": "string",  "check": "checkDate" }
};

(:~ The TypeScript type of an XSD type name. :)
declare function ts:type($name as xs:string) as xs:string {
  if (starts-with($name, "xs:"))
  then ts:built-in($name)?type
  else $name
};

(:~ The name of the check function of an XSD type name. :)
declare function ts:check($name as xs:string) as xs:string {
  if (starts-with($name, "xs:"))
  then ts:built-in($name)?check
  else "check" || $name
};

declare function ts:built-in($name as xs:string) as map(*) {
  $ts:built-in($name)
  otherwise error(#ts:unsupported, "The generator has no mapping for " || $name)
};

(:~ A string as a JavaScript string literal. :)
declare function ts:literal($text as xs:string) as xs:string {
  serialize($text, { "method": "json" })
};

(:~ One line of a check that adds a problem when the condition holds. :)
declare function ts:problem($condition as xs:string, $message as xs:string) as xs:string {
  ``[  if (`{ $condition }`) problems.push(path + `{ ts:literal(": " || $message) }`);]``
};

(:~ The check of one facet, XSD 1.1 Part 2, 4.3. :)
declare function ts:facet($facet as element()) as xs:string {
  let $value := string($facet/@value)
  return switch (local-name($facet)) {
    case "pattern" return ts:problem(
      "!/^(?:" || replace($value, "/", "\\/") || ")$/u.test(value)",
      "must match " || $value)
    case "length" return ts:problem(
      "characterCount(value) !== " || $value, "must have " || $value || " characters")
    case "minLength" return ts:problem(
      "characterCount(value) < " || $value, "must have at least " || $value || " characters")
    case "maxLength" return ts:problem(
      "characterCount(value) > " || $value, "must have at most " || $value || " characters")
    case "minInclusive" return ts:problem("value < " || $value, "must be at least " || $value)
    case "maxInclusive" return ts:problem("value > " || $value, "must be at most " || $value)
    case "minExclusive" return ts:problem("value <= " || $value, "must be more than " || $value)
    case "maxExclusive" return ts:problem("value >= " || $value, "must be less than " || $value)
    default return error(#ts:unsupported,
      "The generator has no check for the facet " || local-name($facet))
  }
};

(:~ A simple type: a TypeScript type and a check of its facets. :)
declare function ts:simple-type($type as element(xs:simpleType)) as xs:string {
  let $name := string($type/@name)
  let $base := string($type/xs:restriction/@base)
  let $values := $type/xs:restriction/xs:enumeration/@value ! string()
  let $facets := $type/xs:restriction/(* except xs:enumeration)
  return ``[
export type `{ $name }` = `{
  if (exists($values))
  then string-join($values ! ts:literal(.), " | ")
  else ts:type($base)
}`;

export function check`{ $name }`(value: unknown, path: string, problems: string[]): value is `{ $name }` {
  if (!`{ ts:check($base) }`(value, path, problems)) return false;
  const count = problems.length;
`{ string-join((
  if (exists($values)) {
    ts:problem(
      "![" || string-join($values ! ts:literal(.), ", ") || "].includes(value)",
      "must be one of " || string-join($values, ", "))
  },
  $facets ! ts:facet(.)
), char("\n")) }`
  return problems.length === count;
}
]``
};

(:~ A property of a complex type: one element of its sequence. :)
declare function ts:property($element as element(xs:element)) as xs:string {
  let $many := $element/@maxOccurs = "unbounded"
  let $optional := $element/@minOccurs = "0" and not($many)
  return "  " || $element/@name || (if ($optional) then "?" else "") || ": "
    || ts:type($element/@type) || (if ($many) then "[]" else "") || ";"
};

(:~ The check of a property: required, optional, or an array. :)
declare function ts:property-check($element as element(xs:element)) as xs:string {
  let $name := string($element/@name)
  let $check := ts:check($element/@type)
  let $path := "path + " || ts:literal("." || $name)
  return if ($element/@maxOccurs = "unbounded") then ``[
  if (Array.isArray(value.`{ $name }`)) {
    value.`{ $name }`.forEach((item, index) => `{ $check }`(item, path + `{ ts:literal("." || $name || "[") }` + index + "]", problems));
  } else {
    problems.push(`{ $path }` + ": expected an array");
  }]``
  else if ($element/@minOccurs = "0") then ``[
  if (value.`{ $name }` !== undefined) `{ $check }`(value.`{ $name }`, `{ $path }`, problems);]``
  else ``[
  `{ $check }`(value.`{ $name }`, `{ $path }`, problems);]``
};

(:~ A complex type: an interface and a check of its properties. :)
declare function ts:complex-type($type as element(xs:complexType)) as xs:string {
  let $name := string($type/@name)
  let $elements := $type/xs:sequence/xs:element
  return ``[
export interface `{ $name }` {
`{ string-join($elements ! ts:property(.), char("\n")) }`
}

export function check`{ $name }`(value: unknown, path: string, problems: string[]): value is `{ $name }` {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;`{ string-join($elements ! ts:property-check(.)) }`
  for (const key of Object.keys(value)) {
    if (![`{ string-join($elements/@name ! ts:literal(.), ", ") }`].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}
]``
};

(:~ types.ts: every type of the schema, in the schema's order. :)
declare function ts:types($schema as document-node()) as xs:string {
  ``[// Generated from the XSD schema by generate.xq. Do not edit.
import { characterCount, checkBoolean, checkDate, checkDecimal, checkInteger, checkObject, checkString } from "../runtime.ts";
`{ string-join($schema/xs:schema/(xs:simpleType | xs:complexType) !
  (if (self::xs:simpleType) then ts:simple-type(.) else ts:complex-type(.))) }`]``
};

(:~ The remote functions of an API description, after checking that their types exist. :)
declare function ts:functions($api as document-node()) as element()* {
  let $schema := doc(resolve-uri($api/api:api/@types, base-uri($api)))
  let $names := $schema/xs:schema/xs:complexType/@name ! string()
  for $function in $api/api:api/(api:query | api:command)
  for $type in ($function/@input, $function/@output)
  where not($type = $names)
  return error(#ts:unknown-type,
    "The function " || $function/@name || " uses the type " || $type
    || ", which the schema doesn't declare"),
  $api/api:api/(api:query | api:command)
};

(:~ The type names that the functions use, each once. :)
declare function ts:used-types($functions as element()*) as xs:string* {
  distinct-values($functions/(@input, @output))
};

(:~ client.ts: one async function for each remote function. :)
declare function ts:client($api as document-node()) as xs:string {
  let $functions := ts:functions($api)
  let $types := ts:used-types($functions)
  return ``[// Generated from the API description by generate.xq. Do not edit.
import { call, expect } from "../runtime.ts";
import type { `{ string-join($types, ", ") }` } from "./types.ts";
import { `{ string-join($types ! ts:check(.), ", ") }` } from "./types.ts";
`{ string-join(
  for $function in $functions
  return ``[
/** `{ normalize-space($function) }` */
export async function `{ $function/@name }`(input: `{ $function/@input }`): Promise<`{ $function/@output }`> {
  expect<`{ $function/@input }`>(input, `{ ts:check($function/@input) }`);
  const output = await call("`{ $function/@name }`", "`{ local-name($function) }`", input);
  return expect<`{ $function/@output }`>(output, `{ ts:check($function/@output) }`);
}
]``) }`]``
};

(:~ server.ts: the interface the server implements, and a request handler. :)
declare function ts:server($api as document-node()) as xs:string {
  let $functions := ts:functions($api)
  let $types := ts:used-types($functions)
  return ``[// Generated from the API description by generate.xq. Do not edit.
import { serve } from "../runtime.ts";
import type { `{ string-join($types, ", ") }` } from "./types.ts";
import { `{ string-join($types ! ts:check(.), ", ") }` } from "./types.ts";

/** What the server does: one method for each remote function. */
export interface Implementation {
`{ string-join(
  for $function in $functions
  return ``[  /** `{ normalize-space($function) }` */
  `{ $function/@name }`(input: `{ $function/@input }`): Promise<`{ $function/@output }`>;]``,
  char("\n")) }`
}

/** A Node.js request listener that checks every input and output. */
export function createHandler(implementation: Implementation) {
  return serve({
`{ string-join(
  for $function in $functions
  return ``[    `{ $function/@name }`: {
      kind: "`{ local-name($function) }`",
      checkInput: `{ ts:check($function/@input) }`,
      checkOutput: `{ ts:check($function/@output) }`,
      run: (input) => implementation.`{ $function/@name }`(input),
    },]``,
  char("\n")) }`
  });
}
]``
};
