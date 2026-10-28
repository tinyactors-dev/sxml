<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="mark" value="'items-present'"/>
  <sch:pattern>
    <sch:rule context="item" flag="$mark">
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
