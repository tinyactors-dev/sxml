<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <sch:let name="names" value="('a', 'b')" as="xs:string+"/>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:report test="count($names) = 2">Two names.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
