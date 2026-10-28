<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="labelled">
    <sch:rule context="$element">
      <sch:report test="$label">Has <sch:name path="$label"/> = <sch:value-of select="$label"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="labelled">
    <sch:param name="element" value="item"/>
    <sch:param name="label" value="@code"/>
  </sch:pattern>
</sch:schema>
