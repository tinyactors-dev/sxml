<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="entry">
      <sch:report test="true()">Entry.</sch:report>
    </sch:rule>
    <sch:rule context="entry[@kind = 'x']">
      <sch:report test="true()">Entry of kind x.</sch:report>
    </sch:rule>
    <sch:rule context="@kind">
      <sch:report test="true()">Kind <sch:value-of select="."/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
