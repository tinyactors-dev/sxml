<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="totals">
    <sch:let name="total" value="count(//$child)"/>
    <sch:rule context="$parent">
      <sch:report test="true()">Total <sch:value-of select="$total"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="totals">
    <sch:param name="parent" value="table"/>
    <sch:param name="child" value="row"/>
  </sch:pattern>
</sch:schema>
