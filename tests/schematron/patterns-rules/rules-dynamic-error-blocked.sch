<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 2">Two.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:assert test="xs:integer(concat('x', @n)) gt 0">Never decided.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
