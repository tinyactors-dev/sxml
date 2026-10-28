<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p">
    <sch:active pattern="t"/>
  </sch:phase>
  <sch:pattern abstract="true" id="t">
    <sch:rule context="$e">
      <sch:report test="@n = 2">Two.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="t" id="i">
    <sch:param name="e" value="item"/>
  </sch:pattern>
</sch:schema>
