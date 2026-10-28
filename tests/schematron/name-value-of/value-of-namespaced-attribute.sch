<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="q" uri="urn:example:q"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@q:code">Code <sch:value-of select="@q:code"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
