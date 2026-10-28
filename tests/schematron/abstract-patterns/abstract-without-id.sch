<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true">
    <sch:rule context="$element">
      <sch:assert test="*">Has children.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="*">Has children.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
