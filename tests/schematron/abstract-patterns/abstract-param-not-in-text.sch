<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="literal">
    <sch:rule context="$element">
      <sch:report test="true()">Found $element here.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="literal">
    <sch:param name="element" value="table"/>
  </sch:pattern>
</sch:schema>
