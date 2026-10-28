<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern documents="'sub.xml'">
    <sch:rule context="/*">
      <sch:report test="true()">Found <sch:value-of select="@where"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
