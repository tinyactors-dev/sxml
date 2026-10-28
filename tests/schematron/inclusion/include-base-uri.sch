<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="item">Has items.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:include href="include-base-uri/pattern.sch"/>
</sch:schema>
