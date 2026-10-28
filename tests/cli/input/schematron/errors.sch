<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="line[">
      <sch:assert test="@quantity &gt;">Quantity.</sch:assert>
      <sch:unknown/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
