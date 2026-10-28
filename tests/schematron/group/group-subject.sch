<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="item">
      <sch:assert test="@n != 2" subject="..">Parent of two.</sch:assert>
    </sch:rule>
    <sch:rule context="item" subject="@n">
      <sch:assert test="@n != 2">Attribute two.</sch:assert>
    </sch:rule>
  </sch:group>
</sch:schema>
