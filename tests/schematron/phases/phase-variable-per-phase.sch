<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="lenient">
    <sch:let name="limit" value="3"/>
    <sch:active pattern="limits"/>
  </sch:phase>
  <sch:phase id="strict">
    <sch:let name="limit" value="1"/>
    <sch:active pattern="limits"/>
  </sch:phase>
  <sch:pattern id="limits">
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $limit">At most <sch:value-of select="$limit"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
