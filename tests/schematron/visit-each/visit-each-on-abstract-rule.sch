<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="a" visit-each="entry">
      <sch:report test="true()">Never.</sch:report>
    </sch:rule>
    <sch:rule context="list">
      <sch:extends rule="a"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
