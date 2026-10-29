<?xml version="1.0" encoding="UTF-8"?>
<!-- Rules of an API description that api.xsd cannot say: they need
     another document, the XSD schema of the values. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
        schematronEdition="2025">
  <title>Rules of an API description</title>
  <ns prefix="api" uri="https://example.org/api"/>

  <!-- The schema of the values, which the description names. -->
  <let name="types" value="doc(resolve-uri(/api:api/@types, base-uri(/)))"/>
  <let name="complex-types" value="$types/xs:schema/xs:complexType/@name ! string()"/>
  <!-- The type names the functions use. -->
  <let name="used-types" value="/api:api/*/(@input | @output) ! string()"/>

  <pattern id="types">
    <rule context="api:query | api:command" visit-each="@input | @output">
      <assert id="type-exists" test=". = $complex-types">The <name/>
        type <value-of select="."/> of <value-of select="../@name"/>
        is no complex type of <value-of select="/api:api/@types"/>.</assert>
    </rule>
  </pattern>

  <!-- Rules over the schema of the values: its findings are in that
       document. -->
  <pattern id="used" documents="/api:api/@types">
    <rule context="xs:schema/xs:complexType">
      <assert id="type-used" test="@name = ($used-types, //xs:element/@type)">The
        complex type <value-of select="@name"/> is used by no function and
        by no other type.</assert>
    </rule>
  </pattern>

  <pattern id="clients">
    <rule context="api:command">
      <report id="browser-command" test="data(@clients) = 'browser'">The
        command <value-of select="@name"/> changes flags, and a browser
        page must not call it.</report>
    </rule>
  </pattern>
</schema>
