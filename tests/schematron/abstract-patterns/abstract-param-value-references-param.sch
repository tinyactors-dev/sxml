<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="indirect">
    <sch:rule context="item">
      <sch:report test="$a = 1">One.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="indirect">
    <sch:param name="a" value="$b"/>
    <sch:param name="b" value="@n"/>
  </sch:pattern>
</sch:schema>
