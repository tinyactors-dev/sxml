<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="list" visit-each="entry">
      <sch:report test="true()">Visited <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
    <sch:rule context="entry">
      <sch:report test="true()">Entry <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
