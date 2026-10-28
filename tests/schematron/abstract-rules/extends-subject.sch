<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="n-not-three">
      <sch:assert test="not(@n = 3)" subject="@n">Not three.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:extends rule="n-not-three"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
