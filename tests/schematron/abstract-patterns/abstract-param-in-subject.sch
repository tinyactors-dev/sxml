<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="pointed">
    <sch:rule context="$element">
      <sch:assert test="$part != 'bad'" subject="$part">Not bad.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="pointed">
    <sch:param name="element" value="item"/>
    <sch:param name="part" value="@code"/>
  </sch:pattern>
</sch:schema>
