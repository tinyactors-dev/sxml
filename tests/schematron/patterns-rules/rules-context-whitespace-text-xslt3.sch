<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="text()[not(normalize-space())]">
      <sch:report test="true()">Whitespace.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
