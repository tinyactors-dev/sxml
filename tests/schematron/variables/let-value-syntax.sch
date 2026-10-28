<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="broken" value="1 +"/>
      <sch:assert test="true()">Always.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
