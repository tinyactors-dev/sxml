<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:rules>
    <sch:rule abstract="true" id="one">
      <sch:report test="@n = 1">One.</sch:report>
    </sch:rule>
  </sch:rules>
  <sch:rules>
    <sch:rule abstract="true" id="two">
      <sch:report test="@n = 2">Two.</sch:report>
    </sch:rule>
  </sch:rules>
  <sch:pattern>
    <sch:rule context="item">
      <sch:extends rule="one"/>
      <sch:extends rule="two"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
