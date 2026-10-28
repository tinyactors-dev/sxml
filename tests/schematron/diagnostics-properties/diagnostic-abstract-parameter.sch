<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="t">
    <sch:rule context="$e">
      <sch:assert test="@n != 2" diagnostics="d">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="t">
    <sch:param name="e" value="item"/>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="d">Count <sch:value-of select="count(//$e)"/>.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
