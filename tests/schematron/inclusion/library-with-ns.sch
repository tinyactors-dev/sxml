<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:extends href="library-with-ns/library.sch"/>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:assert test="true()">Always.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
