<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="@n">
      <sch:report test=". = 1">Attribute rule one.</sch:report>
    </sch:rule>
    <sch:rule context="@*">
      <sch:report test=". = 1">Attribute rule two.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="@n = 1">Element rule.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
