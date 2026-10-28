<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="größe" value="2"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $größe">At most <sch:value-of select="$größe"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
