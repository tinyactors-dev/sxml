<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[1]">
      <sch:let name="label" value="'first'"/>
      <sch:report test="true()">The <sch:value-of select="$label"/>.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:let name="label" value="'other'"/>
      <sch:report test="@n = 3">The <sch:value-of select="$label"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
