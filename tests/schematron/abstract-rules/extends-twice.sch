<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="a">
      <sch:assert test="@n != 2">Not two.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:extends rule="a"/>
      <sch:extends rule="a"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
