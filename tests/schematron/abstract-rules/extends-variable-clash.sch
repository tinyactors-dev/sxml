<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="a">
      <sch:let name="v" value="1"/>
      <sch:assert test="@n != $v">Not v.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:let name="v" value="2"/>
      <sch:extends rule="a"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
