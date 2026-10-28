<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="limit">
    <sch:rule context="item">
      <sch:assert test="@n != 2" role="$r">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="limit">
    <sch:param name="r" value="business"/>
  </sch:pattern>
</sch:schema>
