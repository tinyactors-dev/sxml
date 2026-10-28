<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:rules>
    <sch:rule context="item" id="has-n">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
  </sch:rules>
  <sch:pattern>
    <sch:rule context="item">
      <sch:extends rule="has-n"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
