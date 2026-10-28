<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:rules>
    <sch:rule abstract="true" id="has-n">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
  </sch:rules>
  <sch:group>
    <sch:rule context="item">
      <sch:extends rule="has-n"/>
    </sch:rule>
    <sch:rule context="item[2]">
      <sch:report test="true()">Second.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
