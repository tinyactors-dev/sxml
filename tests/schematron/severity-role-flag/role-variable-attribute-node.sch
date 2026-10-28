<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="kind" value="@kind"/>
      <sch:report test="@n &gt; 1" role="$kind">Above one.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
