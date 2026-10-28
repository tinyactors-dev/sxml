<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item" flag="$f">
      <sch:let name="f" value="concat('kind-', @kind)"/>
      <sch:assert test="@n">Has n.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
