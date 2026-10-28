<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="n-one">
      <sch:report test="@n = 1">External n one.</sch:report>
    </sch:rule>
    <sch:rule abstract="true" id="n-two">
      <sch:report test="@n = 2">External n two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
