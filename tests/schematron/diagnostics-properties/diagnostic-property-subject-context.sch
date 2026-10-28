<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2" subject=".." diagnostics="d" properties="p">Not two: <sch:name/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="d">Diagnostic for <sch:name/>.</sch:diagnostic>
  </sch:diagnostics>
  <sch:properties>
    <sch:property id="p">Property for <sch:name/>.</sch:property>
  </sch:properties>
</sch:schema>
