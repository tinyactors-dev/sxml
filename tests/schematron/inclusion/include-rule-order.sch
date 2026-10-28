<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:include href="include-rule-order/rule.sch"/>
    <sch:rule context="item">
      <sch:report test="@n = 2">Local rule.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
