<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="limited">
      <sch:assert test="not(@n &gt; $limit)">At most <sch:value-of select="$limit"/>.</sch:assert>
    </sch:rule>
    <sch:rule context="item">
      <sch:let name="limit" value="1"/>
      <sch:extends rule="limited"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
