<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">First.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">Second.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">Third.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
