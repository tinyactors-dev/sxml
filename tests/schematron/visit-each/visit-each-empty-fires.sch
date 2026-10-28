<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="entry" flag="visited">
      <sch:report test="true()">Visited.</sch:report>
    </sch:rule>
    <sch:rule context="list">
      <sch:report test="true()">Later rule.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
