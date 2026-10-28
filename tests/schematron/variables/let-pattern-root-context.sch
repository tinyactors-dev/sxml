<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:let name="c" value="count(*)"/>
    <sch:rule context="item">
      <sch:report test="@n = 1">Children of the root: <sch:value-of select="$c"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
