<?xml version="1.0" encoding="UTF-8"?>
<!-- Rules of an API description that api.xsd cannot say: they need
     another document, the XSD schema of the values. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
        schematronEdition="2025">
  <title>Rules of an API description</title>
  <ns prefix="api" uri="https://example.org/api"/>
  <ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>

  <!-- The schema of the values, which the description names. -->
  <let name="types" value="doc(resolve-uri(/api:api/@types, base-uri(/)))"/>
  <let name="complex-types" value="$types/xs:schema/xs:complexType/@name ! string()"/>

  <pattern id="types">
    <rule context="api:query | api:command" visit-each="@input | @output">
      <assert id="type-exists" test=". = $complex-types">The <name/>
        type <value-of select="."/> of <value-of select="../@name"/>
        is no complex type of <value-of select="/api:api/@types"/>.</assert>
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
