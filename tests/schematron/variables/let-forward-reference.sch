<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="b" value="$a * 10"/>
      <sch:let name="a" value="@n"/>
      <sch:report test="$b = 20">Twenty.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
