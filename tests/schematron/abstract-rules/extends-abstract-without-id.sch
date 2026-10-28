<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
