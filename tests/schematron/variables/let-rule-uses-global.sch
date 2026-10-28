<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="factor" value="10"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="scaled" value="@n * $factor"/>
      <sch:report test="$scaled = 30">Thirty.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
