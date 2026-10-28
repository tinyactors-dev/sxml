<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="count(item) = 3">Three items.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
