<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n">An item has n.</sch:assert>
      <sch:let name="n" value="@n"/>
      <sch:report test="$n = 2">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
