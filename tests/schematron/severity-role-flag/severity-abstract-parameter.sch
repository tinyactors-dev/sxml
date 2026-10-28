<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="limit">
    <sch:param name="sev"/>
    <sch:rule context="item">
      <sch:assert test="@n != 2" severity="$sev">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="limit">
    <sch:param name="sev" value="error"/>
  </sch:pattern>
</sch:schema>
