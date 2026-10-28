<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">Group one, first.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="@n = 2">Group one, second.</sch:report>
    </sch:rule>
  </sch:group>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="@n = 2">Pattern.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="true()">Blocked.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:group>
    <sch:rule context="item">
      <sch:report test="@n = 2">Group two.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
