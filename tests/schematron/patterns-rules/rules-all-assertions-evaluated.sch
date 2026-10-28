<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[@n = 1]">
      <sch:assert test="@a">Has a.</sch:assert>
      <sch:report test="@n">Has n.</sch:report>
      <sch:assert test="@b">Has b.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
