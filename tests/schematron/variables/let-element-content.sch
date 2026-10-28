<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="config"><config xmlns=""><max>2</max></config></sch:let>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n &lt;= $config//max">At most the configured maximum.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
