<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:include href="include-rule-into-group/rule.sch"/>
    <sch:rule context="item">
      <sch:report test="@n = 2">Group rule.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
