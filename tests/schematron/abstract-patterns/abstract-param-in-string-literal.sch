<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="named">
    <sch:rule context="*[name() = '$element']">
      <sch:report test="'$element' = 'table'">Named table.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="named">
    <sch:param name="element" value="table"/>
  </sch:pattern>
</sch:schema>
