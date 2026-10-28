<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="t">
    <sch:rule context="item">
      <sch:assert test="$check">Checked.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="t">
    <sch:param name="check" value="@n ="/>
  </sch:pattern>
</sch:schema>
