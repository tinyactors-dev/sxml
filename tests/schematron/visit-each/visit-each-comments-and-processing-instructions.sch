<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="comment() | processing-instruction()">
      <sch:report test="true()">Visited <sch:value-of select="."/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
