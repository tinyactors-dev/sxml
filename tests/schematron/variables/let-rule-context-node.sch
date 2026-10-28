<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="here" value="name()"/>
      <sch:report test="@n = 1">At <sch:value-of select="$here"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
