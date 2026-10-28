<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="allowed" value="(1, 3)"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n = $allowed">An allowed n.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
