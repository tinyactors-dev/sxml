<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="attribute" value="@n"/>
      <sch:report test="$attribute = 1">Attribute <sch:name path="$attribute"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
