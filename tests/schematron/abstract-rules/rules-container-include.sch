<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:rules>
    <sch:include href="rules-container-include/rule.sch"/>
    <sch:rule abstract="true" id="own">
      <sch:report test="@n = 1">One.</sch:report>
    </sch:rule>
  </sch:rules>
  <sch:pattern>
    <sch:rule context="item">
      <sch:extends rule="own"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
