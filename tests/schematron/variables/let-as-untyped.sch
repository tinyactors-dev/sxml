<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="n" value="@n" as="xs:integer"/>
      <sch:report test="$n + 1 = 3">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
