<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:let name="v" value="1"/>
    <sch:let name="v" value="2"/>
    <sch:rule context="item">
      <sch:assert test="$v">Holds.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
