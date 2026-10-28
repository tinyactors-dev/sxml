<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="item" flag="first">
      <sch:assert test="true()">Always.</sch:assert>
    </sch:rule>
    <sch:rule context="item[@n = 3]" flag="third">
      <sch:assert test="true()">Always.</sch:assert>
    </sch:rule>
  </sch:group>
</sch:schema>
