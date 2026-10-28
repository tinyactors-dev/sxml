<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group documents="'documents-group/sub.xml'">
    <sch:rule context="item">
      <sch:report test="@n = 5">Rule one.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="@n = 5">Rule two.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
