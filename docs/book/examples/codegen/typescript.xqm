(:~ Turns an XSD schema and an API description into the tree of a TypeScript program. :)
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

declare function ts:built-in($name as xs:string) as map(*) {
  $ts:built-in($name)
  otherwise error(#ts:unsupported, "The generator has no mapping for " || $name)
};

(:~ The TypeScript type of an XSD type name. :)
declare function ts:type($name as xs:string) as element(type) {
  <type name="{ if (starts-with($name, "xs:")) then ts:built-in($name)?type else $name }"/>
};

(:~ The name of the check function of an XSD type name. :)
declare function ts:check($name as xs:string) as xs:string {
  if (starts-with($name, "xs:"))
  then ts:built-in($name)?check
  else "check" || $name
};

(:~ The statement problems.push(a + b + ...). :)
declare function ts:push($message as element()+) as element(statement) {
  <statement>
    <call method="push"><name>problems</name><binary operator="plus">{ $message }</binary></call>
  </statement>
};

(:~ A statement that adds a problem when the condition holds. :)
declare function ts:problem($condition as element(), $text as xs:string) as element(if) {
  <if>{ $condition }<then>{ ts:push((<name>path</name>, <string>: { $text }</string>)) }</then></if>
};

(:~ The comparison of an expression with the value of a facet. :)
declare function ts:compare($left as element(), $operator as xs:string, $value as xs:string)
    as element(binary) {
  <binary operator="{ $operator }">{ $left }<number>{ $value }</number></binary>
};

(:~ The check of one facet, XSD 1.1 Part 2, 4.3. :)
declare function ts:facet($facet as element()) as element(if) {
  let $value := string($facet/@value)
  let $characters := <call function="characterCount"><name>value</name></call>
  return switch (local-name($facet)) {
    case "pattern" return ts:problem(
      <not><call method="test"><regex flags="u">^(?:{ $value })$</regex><name>value</name></call></not>,
      "must match " || $value)
    case "length" return ts:problem(
      ts:compare($characters, "not-equal", $value), "must have " || $value || " characters")
    case "minLength" return ts:problem(
      ts:compare($characters, "less", $value), "must have at least " || $value || " characters")
    case "maxLength" return ts:problem(
      ts:compare($characters, "greater", $value), "must have at most " || $value || " characters")
    case "minInclusive" return ts:problem(
      ts:compare(<name>value</name>, "less", $value), "must be at least " || $value)
    case "maxInclusive" return ts:problem(
      ts:compare(<name>value</name>, "greater", $value), "must be at most " || $value)
    case "minExclusive" return ts:problem(
      ts:compare(<name>value</name>, "at-most", $value), "must be more than " || $value)
    case "maxExclusive" return ts:problem(
      ts:compare(<name>value</name>, "at-least", $value), "must be less than " || $value)
    default return error(#ts:unsupported,
      "The generator has no check for the facet " || local-name($facet))
  }
};

(:~ A check function: it checks the base type first, then adds a problem for each rule that fails. :)
declare function ts:check-function($name as xs:string, $base as xs:string, $checks as element()*)
    as element(function) {
  <function name="check{ $name }" export="true">
    <parameter name="value"><type name="unknown"/></parameter>
    <parameter name="path"><type name="string"/></parameter>
    <parameter name="problems"><array-type><type name="string"/></array-type></parameter>
    <returns><predicate parameter="value"><type name="{ $name }"/></predicate></returns>
    <body>
      <if>
        <not><call function="{ $base }"><name>value</name><name>path</name><name>problems</name></call></not>
        <then><return><boolean>false</boolean></return></then>
      </if>
      <const name="count"><member name="length"><name>problems</name></member></const>
      { $checks }
      <return>
        <binary operator="equal"><member name="length"><name>problems</name></member><name>count</name></binary>
      </return>
    </body>
  </function>
};

(:~ A simple type: a type alias and a check of its facets. :)
declare function ts:simple-type($type as element(xs:simpleType)) as element()+ {
  let $name := string($type/@name)
  let $base := string($type/xs:restriction/@base)
  let $values := $type/xs:restriction/xs:enumeration/@value ! string()
  let $facets := $type/xs:restriction/(* except xs:enumeration)
  return (
    <type-alias name="{ $name }" export="true">{
      if (exists($values))
      then <union>{ $values ! <string>{ . }</string> }</union>
      else ts:type($base)
    }</type-alias>,
    ts:check-function($name, ts:check($base), (
      if (exists($values)) {
        ts:problem(
          <not><call method="includes"><array>{ $values ! <string>{ . }</string> }</array><name>value</name></call></not>,
          "must be one of " || string-join($values, ", "))
      },
      $facets ! ts:facet(.)
    ))
  )
};

(:~ A property of a complex type: one element of its sequence. :)
declare function ts:property($element as element(xs:element)) as element(property) {
  let $many := $element/@maxOccurs = "unbounded"
  return <property name="{ $element/@name }">{
    if ($element/@minOccurs = "0" and not($many)) { attribute optional { "true" } },
    if ($many)
    then <array-type>{ ts:type($element/@type) }</array-type>
    else ts:type($element/@type)
  }</property>
};

(:~ The check of a property: required, optional, or an array. :)
declare function ts:property-check($element as element(xs:element)) as element() {
  let $name := string($element/@name)
  let $check := ts:check($element/@type)
  let $property := <member name="{ $name }"><name>value</name></member>
  let $path := <binary operator="plus"><name>path</name><string>.{ $name }</string></binary>
  return
    if ($element/@maxOccurs = "unbounded") then
      <if>
        <call method="isArray"><name>Array</name>{ $property }</call>
        <then>
          <statement>
            <call method="forEach">{ $property }
              <arrow>
                <parameter name="item"/>
                <parameter name="index"/>
                <call function="{ $check }">
                  <name>item</name>
                  <binary operator="plus">
                    <name>path</name><string>.{ $name }[</string><name>index</name><string>]</string>
                  </binary>
                  <name>problems</name>
                </call>
              </arrow>
            </call>
          </statement>
        </then>
        <else>{ ts:push((<name>path</name>, <string>.{ $name }: expected an array</string>)) }</else>
      </if>
    else if ($element/@minOccurs = "0") then
      <if>
        <binary operator="not-equal">{ $property }<name>undefined</name></binary>
        <then>
          <statement><call function="{ $check }">{ $property, $path }<name>problems</name></call></statement>
        </then>
      </if>
    else
      <statement><call function="{ $check }">{ $property, $path }<name>problems</name></call></statement>
};

(:~ A complex type: an interface and a check of its properties. :)
declare function ts:complex-type($type as element(xs:complexType)) as element()+ {
  let $name := string($type/@name)
  let $elements := $type/xs:sequence/xs:element
  return (
    <interface name="{ $name }" export="true">{ $elements ! ts:property(.) }</interface>,
    ts:check-function($name, "checkObject", (
      $elements ! ts:property-check(.),
      <for-of name="key">
        <call method="keys"><name>Object</name><name>value</name></call>
        <body>
          <if>
            <not>
              <call method="includes">
                <array>{ $elements/@name ! <string>{ string(.) }</string> }</array>
                <name>key</name>
              </call>
            </not>
            <then>{
              ts:push((<name>path</name>, <string>.</string>, <name>key</name>, <string>: is not allowed</string>))
            }</then>
          </if>
        </body>
      </for-of>
    ))
  )
};

(:~ A module: a comment, the imports its declarations need, and the declarations.
    A name is imported from types.ts when that module declares it,
    and a called function that no module of the program declares from runtime.ts. :)
declare function ts:module($file as xs:string, $comment as xs:string,
    $declarations as element()*, $types as element(module)?) as element(module) {
  let $declared := $declarations/@name
  let $from-types := function($names) {
    distinct-values($names[. = $types/*/@name][not(. = $declared)])
  }
  let $type-names := $from-types($declarations//type/@name)
  let $value-names := $from-types($declarations//(call/@function | name))
  let $runtime-names := sort(distinct-values(
    $declarations//call/@function[not(. = ($declared, $types/*/@name))]))
  return
    <module file="{ $file }">
      <comment>{ $comment }</comment>
      {
        if (exists($runtime-names)) {
          <import from="../runtime.ts">{ $runtime-names ! <name>{ . }</name> }</import>
        },
        if (exists($type-names)) {
          <import from="./types.ts" type-only="true">{ $type-names ! <name>{ . }</name> }</import>
        },
        if (exists($value-names)) {
          <import from="./types.ts">{ $value-names ! <name>{ . }</name> }</import>
        },
        $declarations
      }
    </module>
};

(:~ types.ts: every type of the schema, in the schema's order. :)
declare function ts:types($schema as document-node()) as element(module) {
  ts:module("types.ts", "Generated from the XSD schema by program.xq and print.xsl. Do not edit.",
    $schema/xs:schema/(xs:simpleType | xs:complexType) !
      (if (self::xs:simpleType) then ts:simple-type(.) else ts:complex-type(.)),
    ())
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

(:~ The names of the clients that the API description offers, sorted. :)
declare function ts:client-names($api as document-node()) as xs:string* {
  sort(distinct-values($api/api:api/*/@clients ! tokenize(.)))
};

(:~ The module of one client, such as browser.ts: one async function for
    each remote function that the client offers. :)
declare function ts:client($api as document-node(), $client as xs:string, $types as element(module))
    as element(module) {
  ts:module($client || ".ts", "Generated from the API description by program.xq and print.xsl. Do not edit.",
    for $function in ts:functions($api)[tokenize(@clients) = $client]
    let $input := string($function/@input)
    let $output := string($function/@output)
    return
      <function name="{ $function/@name }" export="true" async="true">
        <doc>{ normalize-space($function) }</doc>
        <parameter name="input"><type name="{ $input }"/></parameter>
        <returns><type name="Promise"><type name="{ $output }"/></type></returns>
        <body>
          <statement>
            <call function="expect"><type name="{ $input }"/><name>input</name><name>{ ts:check($input) }</name></call>
          </statement>
          <const name="output">
            <await>
              <call function="call">
                <string>{ string($function/@name) }</string>
                <string>{ local-name($function) }</string>
                <name>input</name>
              </call>
            </await>
          </const>
          <return>
            <call function="expect"><type name="{ $output }"/><name>output</name><name>{ ts:check($output) }</name></call>
          </return>
        </body>
      </function>,
    $types)
};

(:~ server.ts: the interface the service implements, and a request handler
    that checks every function, whichever client calls it. :)
declare function ts:server($api as document-node(), $types as element(module)) as element(module) {
  let $functions := ts:functions($api)
  return ts:module("server.ts", "Generated from the API description by program.xq and print.xsl. Do not edit.", (
    <interface name="Implementation" export="true">
      <doc>What the service does: one method for each remote function.</doc>
      {
        for $function in $functions
        return
          <method name="{ $function/@name }">
            <doc>{ normalize-space($function) }</doc>
            <parameter name="input"><type name="{ $function/@input }"/></parameter>
            <returns><type name="Promise"><type name="{ $function/@output }"/></type></returns>
          </method>
      }
    </interface>,
    <function name="createHandler" export="true">
      <doc>A Node.js request listener that checks every input and output.</doc>
      <parameter name="implementation"><type name="Implementation"/></parameter>
      <body>
        <return>
          <call function="serve">
            <object>{
              for $function in $functions
              return
                <entry name="{ $function/@name }">
                  <object>
                    <entry name="kind"><string>{ local-name($function) }</string></entry>
                    <entry name="checkInput"><name>{ ts:check($function/@input) }</name></entry>
                    <entry name="checkOutput"><name>{ ts:check($function/@output) }</name></entry>
                    <entry name="run">
                      <arrow>
                        <parameter name="input"/>
                        <call method="{ $function/@name }"><name>implementation</name><name>input</name></call>
                      </arrow>
                    </entry>
                  </object>
                </entry>
            }</object>
          </call>
        </return>
      </body>
    </function>
  ), $types)
};
