<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="s" value="'warning'"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="r" value="concat('item-', @n)"/>
      <sch:assert test="@n != 2" severity="$s" role="$r" flag="$s">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
