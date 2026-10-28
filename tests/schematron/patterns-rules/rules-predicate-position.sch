<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[1]">
      <sch:report test="true()">First.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="true()">Other <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
