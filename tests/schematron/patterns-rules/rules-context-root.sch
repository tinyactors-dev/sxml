<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="/">
      <sch:assert test="doc">The document element is doc.</sch:assert>
      <sch:report test="count(//item) = 3">Three items.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
