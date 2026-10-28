<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:rules>
    <sch:rule abstract="true" id="has-n">
      <sch:assert test="@n">The <sch:name/> has an n.</sch:assert>
    </sch:rule>
  </sch:rules>
  <sch:pattern>
    <sch:rule context="item">
      <sch:extends rule="has-n"/>
    </sch:rule>
  </sch:pattern>
  <sch:pattern>
    <sch:rule context="doc">
      <sch:extends rule="has-n"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
