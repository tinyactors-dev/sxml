<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item" flag="has-items">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
    <sch:rule context="missing" flag="has-missing">
      <sch:assert test="true()">Holds.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
